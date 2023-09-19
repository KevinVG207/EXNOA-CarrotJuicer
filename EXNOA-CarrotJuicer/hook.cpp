#include <filesystem>
#include <iostream>
#include <locale>
#include <string>
#include <thread>
#include <Windows.h>
#include <MinHook.h>

#include "hook.hpp"
#include "config.hpp"
#include "edb.hpp"
#include "responses.hpp"
#include "notifier.hpp"
#include "requests.hpp"

using namespace std::literals;

namespace
{
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

	void* localize_jp_get_orig = nullptr;
	Il2CppString* localize_jp_get_hook(int id)
	{
		printf("localize_jp_get_hook %d\n", id);

		// Easiest way:
		// Hash the jp text and use that as key.

		// Find a way to loop over all text in the dictionary of Localize.JP OR the TextId enum.

		// TODO: Try to access the dictionary of Localize.JP.




		// Now I have the textID.
		// Find what string this belongs to in the textid enum.
		
		// If dumping:
		// Write as dictionary key (enum string) and value (jp text).

		// If patching:
		// Fetch translation from tl dict by enum string as key.

		auto orig_result = reinterpret_cast<decltype(localize_jp_get_hook)*>(localize_jp_get_orig)(id);

		return orig_result;
	}

	// Il2CppString* NewWStr(std::wstring_view str, HMODULE game_assembly_module)
	// {
	// 	il2cpp_string_new_utf16_t il2cpp_string_new_utf16 = reinterpret_cast<il2cpp_string_new_utf16_t>(GetProcAddress(game_assembly_module, "il2cpp_string_new_utf16"));
	// 	return il2cpp_string_new_utf16(str.data(), str.size());
	// }

	// template <typename T = void*>
	// T read_field(const void* ptr, const FieldInfo* field)
	// {
	// 	T result;
	// 	const auto fieldPtr = static_cast<const std::byte*>(ptr) + field->offset;
	// 	std::memcpy(std::addressof(result), fieldPtr, sizeof(T));
	// 	return result;
	// }

	// template <typename T>
	// T read_field(const void* ptr, TypedField<T> field)
	// {
	// 	return read_field<T>(ptr, field.Field);
	// }

	// template <typename T>
	// void write_field(void* ptr, const FieldInfo* field, const T& value)
	// {
	// 	const auto fieldPtr = static_cast<std::byte*>(ptr) + field->offset;
	// 	std::memcpy(fieldPtr, std::addressof(value), sizeof(T));
	// }


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

			printf("1\n");

			auto domain = il2cpp_domain_get();
			// Print domain
			printf("Domain: %p\n", domain);


			printf("1aaa\n");

			auto assembly = il2cpp_domain_assembly_open(domain, "umamusume.dll");
			// Print assembly
			printf("Assembly: %p\n", assembly);

			printf("2\n");
			auto image = il2cpp_assembly_get_image(assembly);
			// Print image
			printf("Image: %p\n", image);

			printf("3\n");
			auto localize_class = il2cpp_class_from_name(image, "Gallop", "Localize");
			printf("4\n");

			const char* name = "JP";
			const auto localize_jp_class = find_nested_class_by_name(localize_class, name);


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

				// Extract the text
				for (int i = 0; i <= 18446744073709551615; i++)
				{
					Il2CppString* jp_text = reinterpret_cast<decltype(localize_jp_get_hook)*>(localize_jp_get_orig)(i);

					if (jp_text->length == 0)
					{
						continue;
					}

					printf("ID: %d, length: %d\n", i, jp_text->length);
				}


				MH_EnableHook(localize_jp_get_addr_offset);
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
