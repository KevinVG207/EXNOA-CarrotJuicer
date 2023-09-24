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

	bool tl_first_check = true;
	std::filesystem::file_time_type tl_last_modified;
	std::map<int, std::string> textid_to_text;


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


	void setup_tl_map(){
		printf("Checking tl map\n");

		std::string file_name = "translations.txt";

		if (!file_exists(file_name)) {
			return;
		}

		// Print tl_first_check
		printf("tl_first_check: %d\n", tl_first_check);

		if (tl_first_check)
		{
			tl_first_check = false;
			tl_last_modified = std::filesystem::last_write_time(file_name);
			printf("First check, tl_last_modified: %lld\n", tl_last_modified.time_since_epoch().count());
		}
		else
		{
			auto last_modified = std::filesystem::last_write_time(file_name);

			printf("Previous tl_last_modified: %lld\n", tl_last_modified.time_since_epoch().count());
			printf("Last modified: %lld\n", last_modified.time_since_epoch().count());

			if (last_modified == tl_last_modified)
			{
				printf("No change in tl map\n");
				return;
			}
			else
			{
				printf("Change in tl map\n");
				tl_last_modified = last_modified;
			}
		}

		printf("Setting up tl map\n");

		std::ifstream infile(file_name);
		std::string line;
		while (std::getline(infile, line))
		{
			std::istringstream iss(line);
			
			// Split line by tab. First part is textid, second part is translation
			std::string textid;
			std::string translation;
			std::getline(iss, textid, '\t');
			std::getline(iss, translation, '\t');

			// Skip if translation is empty
			if (translation.empty() || translation.length() == 0)
			{
				continue;
			}

			// Convert textid to int
			int textid_int = std::stoi(textid);

			// Debug print
			printf("textid: %d, translation: %s\n", textid_int, translation.c_str());

			replaceAll(translation, "\\n", "\n");
			replaceAll(translation, "\\r", "\r");
			replaceAll(translation, "\\\"", "\"");

			// Add to map
			textid_to_text[textid_int] = translation;
		}
	}

	void* find_nested_class_by_name(void* klass, const char* name)
	{
		printf("7\n");
		il2cpp_class_get_nested_types_t il2cpp_class_get_nested_types = reinterpret_cast<il2cpp_class_get_nested_types_t>(GetProcAddress(GetModuleHandle(L"GameAssembly.dll"), "il2cpp_class_get_nested_types"));

		void* iter{};
		while (const auto curNestedClass = il2cpp_class_get_nested_types(klass, &iter))
		{
			printf("8\n");
			if (static_cast<bool>(std::string_view(name) ==
								static_cast<Il2CppClassHead*>(curNestedClass)->name))
			{
				printf("9\n");
				return curNestedClass;
			}
		}
		printf("10\n");
		return nullptr;
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
		// Try refreshing the TL map
		setup_tl_map();

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

	void* localize_jp_get_orig = nullptr;
	Il2CppString* localize_jp_get_hook(int id)
	{
		auto orig_result = reinterpret_cast<decltype(localize_jp_get_hook)*>(localize_jp_get_orig)(id);

		if (id != 1030 && id != 1107)  // Don't print annoying time strings
		{
			std::string unicode = il2cppstring_to_jsonstring(orig_result->start_char);
			printf("GET ID: %d: %s\n", id, unicode.c_str());
		}

		// If id is in tl dict, return translation
		if (textid_to_text.find(id) != textid_to_text.end())
		{
			// Convert translation to wstring
			std::string* translation_str = &textid_to_text[id];

			return il2cpp_string_new(translation_str->data());
		}

		return orig_result;
	}

	void* textcommon_setid_orig = nullptr;
	void* textcommon_setid_hook (void* _this, int id)
	{
		printf("SETID: %d\n", id);

		return reinterpret_cast<decltype(textcommon_setid_hook)*>(textcommon_setid_orig) (
			_this, id
			);
	}

		void* textcommon_gettextidstring_orig = nullptr;
	Il2CppString* textcommon_gettextidstring_hook (void* _this)
	{
		return reinterpret_cast<decltype(textcommon_gettextidstring_hook)*>(textcommon_gettextidstring_orig) (
			_this
			);
	}



	void dump_text_db(void* textcommon_object)
	{
		// Dump DB to file
		std::string file_name = "db_dump.json";
		if (!file_exists(file_name))
		{
			printf("db_dump.json not found, skipping.\n");
			return;
		}

		printf("Dumping DB to file\n");

		std::ofstream outfile;
		outfile.open(file_name, std::ios_base::trunc);

		outfile << "{\n";

		// Create an array to store the text
		// Extract the text
		bool first = true;
		for (int i = 0; i <= 99999; i++)
		{
			textcommon_setid_hook(textcommon_object, i);
			Il2CppString* text_id_string = textcommon_gettextidstring_hook(textcommon_object);
			Il2CppString* jp_text = reinterpret_cast<decltype(localize_jp_get_hook)*>(localize_jp_get_orig)(i);

			if (jp_text->length == 0)
			{
				continue;
			}

			if (!first)
			{
				outfile << ",\n";
			}
			else
			{
				first = false;
			}

			// printf("ID: %d, length: %d\n", i, jp_text->length);

			std::string unicode = il2cppstring_to_jsonstring(text_id_string->start_char);
			std::string unicode2 = il2cppstring_to_jsonstring(jp_text->start_char);

			// // Convert i to string
			// char buffer [50];
			// sprintf_s(buffer, "%d", i);
			// std::string textid = buffer;

			outfile << "\t\"" << unicode << "\": \"" << unicode2 << "\"";

		}

		outfile << "\n}";

		outfile.close();
	}


	void* populate_with_errors_orig = nullptr;
	bool populate_with_errors_hook(void* _this, Il2CppString* str, TextGenerationSettings_t* settings, void* context)
	{
		// printf("populate_with_errors_hook: %ls\n", str->start_char);
		// printf("%ls\n\n", environment_get_stacktrace()->start_char);

		std::string unicode = il2cppstring_to_utf8(str->start_char);

		std::string unicode2 = il2cppstring_to_jsonstring(str->start_char);
		printf("TEXT: %s\n", unicode2.c_str());

		// if string starts with <nb>
		if (unicode.find("<nb>") != std::string::npos)
		{
			replaceAll(unicode, "<nb>", "");
			settings->horizontalOverflow = 1;
		}
		if (unicode.find("<slogan>") != std::string::npos)
		{
			replaceAll(unicode, "<slogan>", "");
			replaceAll(unicode, "\n", "");
			settings->horizontalOverflow = 0;
		}

		Il2CppString* new_str = il2cpp_string_new(unicode.c_str());

		return reinterpret_cast<decltype(populate_with_errors_hook)*>(populate_with_errors_orig) (
			_this, new_str, settings, context
			);
	}

	void* localize_extension_text_orig = nullptr;
	Il2CppString* localize_extension_text_hook(int id, int region)
	{
		auto orig_result = reinterpret_cast<decltype(localize_extension_text_hook)*>(localize_extension_text_orig)(id, region);

		std::string unicode = il2cppstring_to_jsonstring(orig_result->start_char);
		printf("EXTENSION ID: %d: %s\n", id, unicode.c_str());

		return orig_result;
	}

	void* localize_get_orig = nullptr;
	Il2CppString* localize_get_hook(int id)
	{
		auto orig_result = reinterpret_cast<decltype(localize_get_hook)*>(localize_get_orig)(id);

		std::string unicode = il2cppstring_to_jsonstring(orig_result->start_char);
		printf("GET2 ID: %d: %s\n", id, unicode.c_str());

		return orig_result;
	}


	void* textcommon_gettextid_orig = nullptr;
	int textcommon_gettextid_hook (void* _this)
	{
		return reinterpret_cast<decltype(textcommon_gettextid_hook)*>(textcommon_gettextid_orig) (
			_this
			);
		
		// printf("GETTEXTID: %d\n", orig_result);

		// return orig_result;
	}

	bool first_textcommon = true;

	void* textcommon_settext_orig = nullptr;
	void* textcommon_settext_hook (void* _this, Il2CppString* str)
	{
		int textid = reinterpret_cast<decltype(textcommon_gettextid_hook)*>(textcommon_gettextid_orig) (
			_this
			);

		std::string id_string = il2cppstring_to_jsonstring(textcommon_gettextidstring_hook(_this)->start_char);

		std::string unicode = il2cppstring_to_jsonstring(str->start_char);

		printf("===SETTEXT===\n");
		printf("ID: %d\n", textid);
		printf("ID STRING: %s\n", id_string.c_str());
		printf("TEXT: %s\n", unicode.c_str());

		if (first_textcommon)
		{
			first_textcommon = false;
			dump_text_db(_this);
			textcommon_setid_hook(_this, textid);
		}

		return reinterpret_cast<decltype(textcommon_settext_hook)*>(textcommon_settext_orig) (
			_this, str
			);
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



			// auto populate_with_errors_addr = get_method_pointer(
			// "UnityEngine.TextRenderingModule.dll",
			// "UnityEngine", "TextGenerator",
			// "PopulateWithErrors", 3
			// )



			printf("1aaa\n");

			auto assembly = il2cpp_domain_assembly_open(domain, "umamusume.dll");
			// Print assembly
			printf("Assembly: %p\n", assembly);

			printf("2\n");
			auto image = il2cpp_assembly_get_image(assembly);
			// Print image
			printf("Image: %p\n", image);


			// const auto system_runtime_module = GetModuleHandle(L"System.Runtime.dll");
			// printf("System.Runtime.dll at %p\n", system_runtime_module);

			// const auto enum_class = il2cpp_class_from_name(system_runtime_module, "System", "Enum");
			// printf("enum_class: %p\n", enum_class);

			// const auto enum_getnames_addr = il2cpp_class_get_method_from_name(enum_class, "GetNames", 1)->methodPointer;
			// printf("enum_getnames_addr: %p\n", enum_getnames_addr);

			// while (true)
			// {
			// 	continue;
			// }


			// // Get TextId enum that is umamusume.dll Gallop TextId
			// auto textid_enum_class = il2cpp_class_from_name(image, "Gallop", "TextId");
			// printf("textid_enum: %p\n", textid_enum_class);

			// auto textid_enum_basetype = il2cpp_class_enum_basetype(textid_enum_class);
			// printf("textid_enum_basetype: %p\n", textid_enum_basetype);


			// void* iter = nullptr;
			// printf("f");
			// while (const MethodInfo* method = il2cpp_class_get_methods(textid_enum_class, &iter))
			// {
			// 	printf("method: %p\n", method->name);
			// }

			// printf("g");


			// while (true)
			// {
			// 	continue;
			// }


			// auto localize_extension_class = il2cpp_class_from_name(image, "Gallop", "LocalizeExtention");
			// printf("localize_extension_class: %p\n", localize_extension_class);

			// auto localize_extension_text_addr = il2cpp_class_get_method_from_name(localize_extension_class, "GetInsertHalfSizeSpace", 2)->methodPointer;
			// printf("localize_extension_text_addr: %p\n", localize_extension_text_addr);

			// auto localize_extension_text_addr_offset = reinterpret_cast<void*>(localize_extension_text_addr);
			// printf("localize_extension_text_addr_offset: %p\n", localize_extension_text_addr_offset);

			// MH_CreateHook(localize_extension_text_addr_offset, localize_extension_text_hook, &localize_extension_text_orig);
			// MH_EnableHook(localize_extension_text_addr_offset);


			auto textcommon_class = il2cpp_class_from_name(image, "Gallop", "TextCommon");
			printf("textcommon_class: %p\n", textcommon_class);




			auto textcommon_setid_addr = il2cpp_class_get_method_from_name(textcommon_class, "set_TextId", 1)->methodPointer;
			printf("textcommon_setid_addr: %p\n", textcommon_setid_addr);

			auto textcommon_setid_addr_offset = reinterpret_cast<void*>(textcommon_setid_addr);
			printf("textcommon_setid_addr_offset: %p\n", textcommon_setid_addr_offset);

			MH_CreateHook(textcommon_setid_addr_offset, textcommon_setid_hook, &textcommon_setid_orig);
			MH_EnableHook(textcommon_setid_addr_offset);


			auto textcommon_gettextid_addr = il2cpp_class_get_method_from_name(textcommon_class, "get_TextId", 0)->methodPointer;
			printf("textcommon_gettextid_addr: %p\n", textcommon_gettextid_addr);

			auto textcommon_gettextid_addr_offset = reinterpret_cast<void*>(textcommon_gettextid_addr);
			printf("textcommon_gettextid_addr_offset: %p\n", textcommon_gettextid_addr_offset);

			MH_CreateHook(textcommon_gettextid_addr_offset, textcommon_gettextid_hook, &textcommon_gettextid_orig);
			MH_EnableHook(textcommon_gettextid_addr_offset);



			auto textcommon_gettextidstring_addr = il2cpp_class_get_method_from_name(textcommon_class, "get_TextIdString", 0)->methodPointer;
			printf("textcommon_gettextidstring_addr: %p\n", textcommon_gettextidstring_addr);

			auto textcommon_gettextidstring_addr_offset = reinterpret_cast<void*>(textcommon_gettextidstring_addr);
			printf("textcommon_gettextidstring_addr_offset: %p\n", textcommon_gettextidstring_addr_offset);

			MH_CreateHook(textcommon_gettextidstring_addr_offset, textcommon_gettextidstring_hook, &textcommon_gettextidstring_orig);





			auto textcommon_settext_addr = il2cpp_class_get_method_from_name(textcommon_class, "set_text", 1)->methodPointer;
			printf("textcommon_settext_addr: %p\n", textcommon_settext_addr);

			auto textcommon_settext_addr_offset = reinterpret_cast<void*>(textcommon_settext_addr);
			printf("textcommon_settext_addr_offset: %p\n", textcommon_settext_addr_offset);

			MH_CreateHook(textcommon_settext_addr_offset, textcommon_settext_hook, &textcommon_settext_orig);
			MH_EnableHook(textcommon_settext_addr_offset);








			printf("3\n");
			auto localize_class = il2cpp_class_from_name(image, "Gallop", "Localize");
			printf("4\n");





			// auto localize_get_addr = il2cpp_class_get_method_from_name(localize_class, "Get", 1)->methodPointer;
			// printf("localize_get_addr: %p\n", localize_get_addr);

			// auto localize_get_addr_offset = reinterpret_cast<void*>(localize_get_addr);
			// printf("localize_get_addr_offset: %p\n", localize_get_addr_offset);

			// MH_CreateHook(localize_get_addr_offset, localize_get_hook, &localize_get_orig);
			// MH_EnableHook(localize_get_addr_offset);










			const char* name = "JP";
			const auto localize_jp_class = find_nested_class_by_name(localize_class, name);
			printf("localize_jp_class: %p\n", localize_jp_class);


			// auto localize_jp_jpdict = il2cpp_class_get_field_from_name(localize_jp_class, "JPdict");

			// auto jp_dict = read_field<Dictionary>(localize_jp_class, localize_jp_jpdict);


			printf("5\n");

			if (localize_jp_class == nullptr)
			{
				printf("Failed to find localize class\n");
			}
			else
			{
				auto localize_jp_get_addr = il2cpp_class_get_method_from_name(localize_jp_class, "Get", 1)->methodPointer;
				printf("6\n");
				printf("localize_jp_get_addr: %p\n", localize_jp_get_addr);


				auto localize_jp_get_addr_offset = reinterpret_cast<void*>(localize_jp_get_addr);
				printf("localize_jp_get_addr_offset: %p\n", localize_jp_get_addr_offset);

				MH_CreateHook(localize_jp_get_addr_offset, localize_jp_get_hook, &localize_jp_get_orig);

				std::string file_name = "db_dump.json";

				printf("7\n");

				// Check if translations file exists
				setup_tl_map();

				MH_EnableHook(localize_jp_get_addr_offset);

				printf("Done\n");
			}



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
