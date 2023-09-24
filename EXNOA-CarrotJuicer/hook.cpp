#include <filesystem>
#include <iostream>
#include <fstream>
#include <locale>
#include <string>
#include <thread>
#include <Windows.h>
#include <MinHook.h>
#include <map>
#include <sstream>

#include "hook.hpp"
#include "config.hpp"
#include "edb.hpp"
#include "responses.hpp"
#include "notifier.hpp"
#include "requests.hpp"

using namespace std::literals;

namespace
{
	il2cpp_string_new_t il2cpp_string_new = nullptr;

	bool file_exists(std::string file_path)
	{
		std::ifstream infile(file_path);
		bool file_exists = infile.good();
		infile.close();
		return file_exists;
	}


	// copy-pasted from https://stackoverflow.com/questions/3418231/replace-part-of-a-string-with-another-string
	void replaceAll(std::string& str, const std::string& from, const std::string& to)
	{
		if (from.empty())
			return;
		size_t start_pos = 0;
		while ((start_pos = str.find(from, start_pos)) != std::string::npos)
		{
			str.replace(start_pos, from.length(), to);
			start_pos += to.length(); // In case 'to' contains 'from', like replacing 'x' with 'yx'
		}
	}

	std::string il2cppstring_to_utf8(std::wstring str)
	{
		std::string result;
		result.resize(str.length() * 4);

		int len = WideCharToMultiByte(CP_UTF8, 0, str.data(), str.length(), result.data(), result.size(), nullptr, nullptr);

		result.resize(len);

		return result;
	}

	std::string il2cppstring_to_jsonstring(std::wstring str)
	{
		auto unicode = il2cppstring_to_utf8(str);
		replaceAll(unicode, "\n", "\\n");
		replaceAll(unicode, "\r", "\\r");
		replaceAll(unicode, "\"", "\\\"");

		return unicode;
	}


	void create_debug_console()
	{
		AllocConsole();

		FILE* _;
		// open stdout stream
		freopen_s(&_, "CONOUT$", "w", stdout);
		freopen_s(&_, "CONOUT$", "w", stderr);
		freopen_s(&_, "CONIN$", "r", stdin);

		SetConsoleTitle(L"Umapyoi");

		// set this to avoid turn japanese texts into question mark
		SetConsoleOutputCP(CP_UTF8);
		std::locale::global(std::locale(""));

		const HANDLE handle = CreateFile(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		                                 NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD mode;
		if (!GetConsoleMode(handle, &mode))
		{
			std::cout << "GetConsoleMode " << GetLastError() << "\n";
		}
		mode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
		if (!SetConsoleMode(handle, mode))
		{
			std::cout << "SetConsoleMode " << GetLastError() << "\n";
		}
	}

	std::string current_time()
	{
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch());
		return std::to_string(ms.count());
	}

	void write_file(const std::string& file_name, const char* buffer, const int len)
	{
		FILE* fp;
		fopen_s(&fp, file_name.c_str(), "wb");
		if (fp != nullptr)
		{
			fwrite(buffer, 1, len, fp);
			fclose(fp);
		}
	}


	void* LZ4_decompress_safe_ext_orig = nullptr;

	int LZ4_decompress_safe_ext_hook(
		char* src,
		char* dst,
		int compressedSize,
		int dstCapacity)
	{
		const int ret = reinterpret_cast<decltype(LZ4_decompress_safe_ext_hook)*>(LZ4_decompress_safe_ext_orig)(
			src, dst, compressedSize, dstCapacity);

		if (config::get().save_response)
		{
			const auto out_path = std::string("CarrotJuicer\\").append(current_time()).append("R.msgpack");
			write_file(out_path, dst, ret);
			std::cout << "wrote response to " << out_path << "\n";
		}

		const std::string data(dst, ret);

		auto notifier_thread = std::thread([&]
		{
			notifier::notify_response(data);
		});

		responses::print_response_additional_info(data);

		notifier_thread.join();

		return ret;
	}

	void* LZ4_compress_default_ext_orig = nullptr;

	int LZ4_compress_default_ext_hook(
		char* src,
		char* dst,
		int srcSize,
		int dstCapacity)
	{
		const int ret = reinterpret_cast<decltype(LZ4_compress_default_ext_hook)*>(LZ4_compress_default_ext_orig)(
			src, dst, srcSize, dstCapacity);

		if (config::get().save_request)
		{
			const auto out_path = std::string("CarrotJuicer\\").append(current_time()).append("Q.msgpack");
			write_file(out_path, src, srcSize);
			std::cout << "wrote request to " << out_path << "\n";
		}

		if (config::get().print_request)
		{
			const std::string data(src, srcSize);
			requests::print_request_additional_info(data);
		}

		return ret;
	}

	void bootstrap_carrot_juicer()
	{
		std::filesystem::create_directory("CarrotJuicer");

		const auto libnative_module = GetModuleHandle(L"libnative.dll");
		printf("libnative.dll at %p\n", libnative_module);
		if (libnative_module == nullptr)
		{
			return;
		}

		const auto LZ4_decompress_safe_ext_ptr = GetProcAddress(libnative_module, "LZ4_decompress_safe_ext");
		printf("LZ4_decompress_safe_ext at %p\n", LZ4_decompress_safe_ext_ptr);
		if (LZ4_decompress_safe_ext_ptr == nullptr)
		{
			return;
		}
		MH_CreateHook(LZ4_decompress_safe_ext_ptr, LZ4_decompress_safe_ext_hook, &LZ4_decompress_safe_ext_orig);
		MH_EnableHook(LZ4_decompress_safe_ext_ptr);

		const auto LZ4_compress_default_ext_ptr = GetProcAddress(libnative_module, "LZ4_compress_default_ext");
		printf("LZ4_compress_default_ext at %p\n", LZ4_compress_default_ext_ptr);
		if (LZ4_compress_default_ext_ptr == nullptr)
		{
			return;
		}
		MH_CreateHook(LZ4_compress_default_ext_ptr, LZ4_compress_default_ext_hook, &LZ4_compress_default_ext_orig);
		MH_EnableHook(LZ4_compress_default_ext_ptr);

		// const auto umamusume_module = GetModuleHandle(L"umamusume.dll");
		// const auto get_class = GetProcAddress
	}


	void* populate_with_errors_orig = nullptr;
	bool populate_with_errors_hook(void* _this, Il2CppString* str, TextGenerationSettings_t* settings, void* context)
	{
		std::string str_utf8 = il2cppstring_to_jsonstring(str->start_char);
		printf("PopulateWithErrors: %s\n", str_utf8.c_str());
		return reinterpret_cast<decltype(populate_with_errors_hook)*>(populate_with_errors_orig)(_this, str, settings, context);
	}



	void* textcommon_gettextid_orig = nullptr;
	int textcommon_gettextid_hook (void* _this)
	{
		return reinterpret_cast<decltype(textcommon_gettextid_hook)*>(textcommon_gettextid_orig)(_this);
	}


	void* textcommon_settextid_orig = nullptr;
	void* textcommon_settextid_hook (void* _this, int id)
	{
		return reinterpret_cast<decltype(textcommon_settextid_hook)*>(textcommon_settextid_orig)(_this, id);
	}


	bool first_textcommon = true;

	void* textcommon_settext_orig = nullptr;
	void* textcommon_settext_hook (void* _this, Il2CppString* str)
	{
		// std::string str_utf8 = il2cppstring_to_jsonstring(str->start_char);
		// printf("TextCommon.set_text: %s\n", str_utf8.c_str());

		int textid = textcommon_gettextid_hook(_this);
		printf("TextCommon.set_text: %d\n", textid);

		if (textid > 0 && first_textcommon)
		{
			// Index text
			first_textcommon = false;
			index_text(_this);
			textcommon_settextid_hook(_this, textid);
		}


		return reinterpret_cast<decltype(textcommon_settext_hook)*>(textcommon_settext_orig)(_this, str);
	}


	void index_text(void* textcommon_obj)
	{
		printf("Indexing text\n");
		bool dump = false;
		std::string file_name = "assembly_dump.json";
		if (file_exists(file_name))
		{
			dump = true;
			printf("Dumping text to file.\n");
		}

		std::ofstream outfile;
		if (dump)
		{
			outfile.open(file_name, std::ios_base::trunc);
			outfile << "{\n";
		}

		bool first = true;
		for (int i; i <= 6000; i++)
		{
			textcommon_settextid_hook(textcommon_obj, i);
			Il2CppString* textid_string = textcommon_gettextid_string_hook(textcommon_obj);
			Il2CppString* jp_text = localize_jp_get_hook(i);

			if (jp_text->length == 0){
				continue;
			}

			std::string textid_string_utf8 = il2cppstring_to_jsonstring(textid_string->start_char);
			std::string jp_text_utf8 = il2cppstring_to_jsonstring(jp_text->start_char);

			printf("index %s: %s\n", textid_string_utf8.c_str(), jp_text_utf8.c_str());
		}
	}


	void* load_library_w_orig = nullptr;

	HMODULE __stdcall load_library_w_hook(const wchar_t* path)
	{
		printf("Saw %ls\n", path);

		// GameAssembly.dll code must be loaded and decrypted while loading criware library
		if (path == L"cri_ware_unity.dll"s)
		{
			const auto game_assembly_module = GetModuleHandle(L"GameAssembly.dll");


			const il2cpp_domain_get_t il2cpp_domain_get = reinterpret_cast<il2cpp_domain_get_t>(GetProcAddress(game_assembly_module, "il2cpp_domain_get"));
			const il2cpp_domain_assembly_open_t il2cpp_domain_assembly_open = reinterpret_cast<il2cpp_domain_assembly_open_t>(GetProcAddress(game_assembly_module, "il2cpp_domain_assembly_open"));
			const il2cpp_assembly_get_image_t il2cpp_assembly_get_image = reinterpret_cast<il2cpp_assembly_get_image_t>(GetProcAddress(game_assembly_module, "il2cpp_assembly_get_image"));
			const il2cpp_class_from_name_t il2cpp_class_from_name = reinterpret_cast<il2cpp_class_from_name_t>(GetProcAddress(game_assembly_module, "il2cpp_class_from_name"));
			const il2cpp_class_get_method_from_name_t il2cpp_class_get_method_from_name = reinterpret_cast<il2cpp_class_get_method_from_name_t>(GetProcAddress(game_assembly_module, "il2cpp_class_get_method_from_name"));
			const il2cpp_class_get_nested_types_t il2cpp_class_get_nested_types = reinterpret_cast<il2cpp_class_get_nested_types_t>(GetProcAddress(game_assembly_module, "il2cpp_class_get_nested_types"));
			const il2cpp_class_get_field_from_name_t il2cpp_class_get_field_from_name = reinterpret_cast<il2cpp_class_get_field_from_name_t>(GetProcAddress(game_assembly_module, "il2cpp_class_get_field_from_name"));
			const il2cpp_class_enum_basetype_t il2cpp_class_enum_basetype = reinterpret_cast<il2cpp_class_enum_basetype_t>(GetProcAddress(game_assembly_module, "il2cpp_class_enum_basetype"));
			const il2cpp_class_get_methods_t il2cpp_class_get_methods = reinterpret_cast<il2cpp_class_get_methods_t>(GetProcAddress(game_assembly_module, "il2cpp_class_get_methods"));
			il2cpp_string_new = reinterpret_cast<il2cpp_string_new_t>(GetProcAddress(game_assembly_module, "il2cpp_string_new"));

			printf("1\n");

			auto domain = il2cpp_domain_get();
			// Print domain
			printf("Domain: %p\n", domain);


			auto assembly2 = il2cpp_domain_assembly_open(domain, "UnityEngine.TextRenderingModule.dll");
			printf("Assembly2: %p\n", assembly2);
			auto image2 = il2cpp_assembly_get_image(assembly2);
			printf("Image2: %p\n", image2);
			auto text_generator_class = il2cpp_class_from_name(image2, "UnityEngine", "TextGenerator");
			printf("TextGenerator: %p\n", text_generator_class);
			auto populate_with_errors_addr = il2cpp_class_get_method_from_name(text_generator_class, "PopulateWithErrors", 3)->methodPointer;
			printf("populate_with_errors_addr: %p\n", populate_with_errors_addr);
			auto populate_with_errors_addr_offset = reinterpret_cast<void*>(populate_with_errors_addr);

			MH_CreateHook(populate_with_errors_addr_offset, populate_with_errors_hook, &populate_with_errors_orig);
			MH_EnableHook(populate_with_errors_addr_offset);



			// Uma Assembly
			auto uma_assembly = il2cpp_domain_assembly_open(domain, "umamusume.dll");
			printf("uma_assembly: %p\n", uma_assembly);

			auto uma_image = il2cpp_assembly_get_image(uma_assembly);
			printf("uma_image: %p\n", uma_image);


			// TextCommon
			auto textcommon_class = il2cpp_class_from_name(uma_image, "Gallop", "TextCommon");
			printf("TextCommon: %p\n", textcommon_class);

			// set_text
			auto textcommon_settext_addr = il2cpp_class_get_method_from_name(textcommon_class, "set_text", 1)->methodPointer;
			printf("textcommon_settext_addr: %p\n", textcommon_settext_addr);

			auto textcommon_settext_addr_offset = reinterpret_cast<void*>(textcommon_settext_addr);
			printf("textcommon_settext_addr_offset: %p\n", textcommon_settext_addr_offset);

			MH_CreateHook(textcommon_settext_addr_offset, textcommon_settext_hook, &textcommon_settext_orig);
			MH_EnableHook(textcommon_settext_addr_offset);



			// get_textid
			auto textcommon_gettextid_addr = il2cpp_class_get_method_from_name(textcommon_class, "get_TextId", 0)->methodPointer;
			printf("textcommon_gettextid_addr: %p\n", textcommon_gettextid_addr);

			auto textcommon_gettextid_addr_offset = reinterpret_cast<void*>(textcommon_gettextid_addr);
			printf("textcommon_gettextid_addr_offset: %p\n", textcommon_gettextid_addr_offset);

			MH_CreateHook(textcommon_gettextid_addr_offset, textcommon_gettextid_hook, &textcommon_gettextid_orig);
			MH_EnableHook(textcommon_gettextid_addr_offset);


			// set_textid
			auto textcommon_settextid_addr = il2cpp_class_get_method_from_name(textcommon_class, "set_TextId", 1)->methodPointer;
			printf("textcommon_settextid_addr: %p\n", textcommon_settextid_addr);

			auto textcommon_settextid_addr_offset = reinterpret_cast<void*>(textcommon_settextid_addr);
			printf("textcommon_settextid_addr_offset: %p\n", textcommon_settextid_addr_offset);

			MH_CreateHook(textcommon_settextid_addr_offset, textcommon_settextid_hook, &textcommon_settextid_orig);
			MH_EnableHook(textcommon_settextid_addr_offset);


			// get_textidstring
			auto textcommon_gettextid_string_addr = il2cpp_class_get_method_from_name(textcommon_class, "get_TextIdString", 0)->methodPointer;
			printf("textcommon_gettextid_string_addr: %p\n", textcommon_gettextid_string_addr);

			auto textcommon_gettextid_string_addr_offset = reinterpret_cast<void*>(textcommon_gettextid_string_addr);
			printf("textcommon_gettextid_string_addr_offset: %p\n", textcommon_gettextid_string_addr_offset);

			MH_CreateHook(textcommon_gettextid_string_addr_offset, textcommon_gettextid_string_hook, &textcommon_gettextid_string_orig);
			MH_EnableHook(textcommon_gettextid_string_addr_offset);

			bootstrap_carrot_juicer();

			MH_DisableHook(LoadLibraryW);
			MH_RemoveHook(LoadLibraryW);

			return LoadLibraryW(path);
		}

		return reinterpret_cast<decltype(LoadLibraryW)*>(load_library_w_orig)(path);
	}
}

void attach()
{
	create_debug_console();

	if (MH_Initialize() != MH_OK)
	{
		printf("Failed to initialize MinHook.\n");
		return;
	}
	printf("MinHook initialized.\n");

	MH_CreateHook(LoadLibraryW, load_library_w_hook, &load_library_w_orig);
	MH_EnableHook(LoadLibraryW);

	config::load();

	std::thread(edb::init).detach();
	std::thread(notifier::init).detach();
}

void detach()
{
	MH_DisableHook(MH_ALL_HOOKS);
	MH_Uninitialize();
}
