/**
 * Copyright (C) 2016 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/blink#license
 */

#include "blink.hpp"
#include "pdb_reader.hpp"
#include <string>
#include <algorithm>
#include <unordered_map>
#include <Windows.h>

blink::application::application()
{
	_image_base = reinterpret_cast<BYTE *>(GetModuleHandle(nullptr));

	_symbols.insert({ "__ImageBase", _image_base });
}

void blink::application::run()
{
	DWORD size = 0;

	const auto headers = reinterpret_cast<const IMAGE_NT_HEADERS *>(_image_base + reinterpret_cast<const IMAGE_DOS_HEADER *>(_image_base)->e_lfanew);

	{	print("Reading PE import directory ...");

		// Search import directory for additional symbols
		const IMAGE_DATA_DIRECTORY import_directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		const auto import_directory_entries = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(_image_base + import_directory.VirtualAddress);

		for (unsigned int i = 0; import_directory_entries[i].FirstThunk != 0; i++)
		{
			const auto name = reinterpret_cast<const char *>(_image_base + import_directory_entries[i].Name);
			const auto import_name_table = reinterpret_cast<const IMAGE_THUNK_DATA *>(_image_base + import_directory_entries[i].Characteristics);
			const auto import_address_table = reinterpret_cast<const IMAGE_THUNK_DATA *>(_image_base + import_directory_entries[i].FirstThunk);

			for (unsigned int k = 0; import_name_table[k].u1.AddressOfData != 0; k++)
			{
				const char *import_name = nullptr;

				// We need to figure out the name of symbols imported by ordinal by going through the export table of the target module
				if (IMAGE_SNAP_BY_ORDINAL(import_name_table[k].u1.Ordinal))
				{
					// The module should have already been loaded by Windows when the application was launched, so just get its handle here
					const auto target_base = reinterpret_cast<const BYTE *>(GetModuleHandleA(name));
					if (target_base == nullptr)
						continue; // Bail out if that is not the case to be safe

					const auto target_headers = reinterpret_cast<const IMAGE_NT_HEADERS *>(target_base + reinterpret_cast<const IMAGE_DOS_HEADER *>(target_base)->e_lfanew);
					const auto export_directory = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(target_base + target_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
					const auto export_name_strings = reinterpret_cast<const DWORD *>(target_base + export_directory->AddressOfNames);
					const auto export_name_ordinals = reinterpret_cast<const WORD *>(target_base + export_directory->AddressOfNameOrdinals);

					const auto ordinal = std::find(export_name_ordinals, export_name_ordinals + export_directory->NumberOfNames, IMAGE_ORDINAL(import_name_table[k].u1.Ordinal));
					if (ordinal != export_name_ordinals + export_directory->NumberOfNames)
						import_name = reinterpret_cast<const char *>(target_base + export_name_strings[std::distance(export_name_ordinals, ordinal)]);
					else
						continue; // Ignore ordinal imports for which the name could not be resolved
				}
				else
				{
					import_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(_image_base + import_name_table[k].u1.AddressOfData)->Name;
				}

				_symbols.insert({ import_name, reinterpret_cast<void *>(import_address_table[k].u1.AddressOfData) });
			}
		}
	}

	{	print("Reading PE debug info directory ...");

		struct RSDS_DEBUG_FORMAT
		{
			uint32_t signature;
			guid guid;
			uint32_t age;
			char path[1];
		} const *debug_data = nullptr;

		// Search debug directory for program debug database file name
		const IMAGE_DATA_DIRECTORY debug_directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
		const auto debug_directory_entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY *>(_image_base + debug_directory.VirtualAddress);

		for (unsigned int i = 0; i < debug_directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY); i++)
		{
			if (debug_directory_entries[i].Type == IMAGE_DEBUG_TYPE_CODEVIEW)
			{
				debug_data = reinterpret_cast<const RSDS_DEBUG_FORMAT *>(reinterpret_cast<const BYTE *>(_image_base + debug_directory_entries[i].AddressOfRawData));

				if (debug_data->signature == 0x53445352) // RSDS
					break;
			}
		}

		if (debug_data != nullptr)
		{
			print("  Found program debug database: " + std::string(debug_data->path));

			pdb_reader pdb(debug_data->path);

			// Check if the debug information actually matches the executable
			if (pdb.guid() == debug_data->guid)
			{
				// The linker working directory should equal the project root directory
				std::string linker_cmd;
				pdb.read_link_info(_source_dir, linker_cmd);

				pdb.read_symbol_table(_image_base, _symbols);
				pdb.read_object_files(_object_files);
				pdb.read_source_files(_source_files);
			}
			else
			{
				print("  Error: Program debug database was created for a different executable file.");
				return;
			}
		}
		else
		{
			print("  Error: Could not find path to program debug database in executable image.");
			return;
		}
	}

	HANDLE compiler_stdin = INVALID_HANDLE_VALUE;
	HANDLE compiler_stdout = INVALID_HANDLE_VALUE;

	{	print("Starting compiler process ...");

		// Launch compiler process
		STARTUPINFO si = { sizeof(si) };
		si.dwFlags = STARTF_USESTDHANDLES;
		SECURITY_ATTRIBUTES sa = { sizeof(sa) };
		sa.bInheritHandle = TRUE;

		if (!CreatePipe(&si.hStdInput, &compiler_stdin, &sa, 0))
		{
			print("  Error: Could not create input communication pipe.");
			return;
		}

		SetHandleInformation(compiler_stdin, HANDLE_FLAG_INHERIT, FALSE);

		if (!CreatePipe(&compiler_stdout, &si.hStdOutput, &sa, 0))
		{
			print("  Error: Could not create output communication pipe.");

			CloseHandle(si.hStdInput);
			return;
		}

		SetHandleInformation(compiler_stdout, HANDLE_FLAG_INHERIT, FALSE);

		si.hStdError = si.hStdOutput;

		TCHAR cmdline[] = TEXT("cmd.exe /q /d /k @echo off");
		PROCESS_INFORMATION pi;

		if (!CreateProcess(nullptr, cmdline, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		{
			print("  Error: Could not create process.");

			CloseHandle(si.hStdInput);
			CloseHandle(si.hStdOutput);
			return;
		}

		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		CloseHandle(si.hStdInput);
		CloseHandle(si.hStdOutput);

		print("  Started process with PID " + std::to_string(pi.dwProcessId));

		// Set up compiler process environment variables
#if _M_IX86
		const std::string command = "\"C:\\Program Files (x86)\\Microsoft Visual Studio 15.0\\VC\\Auxiliary\\Build\\vcvarsall.bat\" x86\n";
#endif
#if _M_AMD64
		const std::string command = "\"C:\\Program Files (x86)\\Microsoft Visual Studio 15.0\\VC\\Auxiliary\\Build\\vcvarsall.bat\" x86_amd64\n";
#endif
		WriteFile(compiler_stdin, command.c_str(), static_cast<DWORD>(command.size()), &size, nullptr);
	}

	print("Starting file system watcher for '" + _source_dir.string() + "' ...");

	// Open handle to the common source code directory
	const HANDLE watcher_handle = CreateFileW(_source_dir.native().c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

	if (watcher_handle == INVALID_HANDLE_VALUE)
	{
		print("  Error: Could not open directory handle.");
		return;
	}

	BYTE watcher_buffer[4096];

	// Check for modifications to any of the source code files
	while (ReadDirectoryChangesW(watcher_handle, watcher_buffer, sizeof(watcher_buffer), TRUE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME, &size, nullptr, nullptr))
	{
		bool first_notification = true;

		// Iterate over all notification items
		for (auto info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(watcher_buffer); first_notification || info->NextEntryOffset != 0;
			first_notification = false, info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(reinterpret_cast<BYTE *>(info) + info->NextEntryOffset))
		{
			std::filesystem::path object_file, source_file =
				_source_dir / std::wstring(info->FileName, info->FileNameLength / sizeof(WCHAR));

			// Ignore changes to files that are not C++ source files
			if (source_file.extension() != ".cpp")
				continue;

			print("Detected modification to: " + source_file.string());

			// Build compiler command line
			std::string cmdline = build_compile_command_line(source_file, object_file);

			// Append special completion message
			cmdline += "\necho compile complete %errorlevel%\n"; // Message used to confirm that compile finished in message loop above

			// Execute compiler command line
			WriteFile(compiler_stdin, cmdline.c_str(), static_cast<DWORD>(cmdline.size()), &size, nullptr);

			// Read and react to compiler output messages
			while (WaitForSingleObject(compiler_stdout, INFINITE) == WAIT_OBJECT_0 && PeekNamedPipe(compiler_stdout, nullptr, 0, nullptr, &size, nullptr))
			{
				std::string message(size, '\0');
				ReadFile(compiler_stdout, message.data(), size, &size, nullptr);

				for (size_t offset = 0, next; (next = message.find('\n', offset)) != std::string::npos; offset = next + 1)
				{
					const auto line = message.substr(offset, next - offset);

					// Only print error information
					if (line.find("error") != std::string::npos || line.find("warning") != std::string::npos)
						print(line.c_str());
				}

				// Listen for special completion message
				if (const size_t offset = message.find("compile complete"); offset != std::string::npos)
				{
					const std::string exit_code = message.substr(offset + 17 /* compile complete */, message.find('\n', offset) - offset - 18);

					print("Finished compiling \"" + object_file.string() + "\" with code " + exit_code + ".");

					// Only load the compiled module if compilation was successful
					if (exit_code == "0")
						link(object_file);
					break;
				}
			}
		}
	}

	CloseHandle(watcher_handle);
	CloseHandle(compiler_stdin);
	CloseHandle(compiler_stdout);
}

std::string blink::application::build_compile_command_line(const std::filesystem::path &source_file, std::filesystem::path &object_file)
{
	object_file = source_file;
	object_file.replace_extension(".obj");

	std::string cmdline =
		"cl.exe "
		"/c " // Compile only, do not link
		"/nologo " // Suppress copyright message
		"/Z7 " // Enable COFF debug information (required for symbol parsing in blink_linker.cpp!)
		"/MDd " // Link with 'MSVCRTD.lib'
		"/Od " // Disable optimizations
		"/EHsc " // Enable C++ exceptions
		"/std:c++latest " // C++ standard version
		"/Zc:wchar_t /Zc:forScope /Zc:inline"; // C++ language conformance
	for (const auto &define : _defines)
		cmdline += " /D \"" + define + "\"";
	for (const auto &include_path : _include_dirs)
		cmdline += " /I \"" + include_path + "\"";
	cmdline += " /Fo\"" + object_file.string() + "\""; // Output object file
	cmdline += " \"" + source_file.string() + "\""; // Input source code file

	return cmdline;
}
