// Windows implementation of the hub platform layer.
#ifdef _WIN32

#include "hub_platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <thread>

namespace fs = std::filesystem;

namespace hub::platform {

namespace {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string last_error_message() {
    DWORD code = GetLastError();
    LPWSTR buffer = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string msg = len ? narrow(std::wstring(buffer, len)) : "";
    if (buffer) LocalFree(buffer);
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' ')) msg.pop_back();
    return "error " + std::to_string(code) + (msg.empty() ? "" : ": " + msg);
}

struct ComScope {
    bool ok = false;
    ComScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        uninit = SUCCEEDED(hr);
    }
    ~ComScope() {
        if (uninit) CoUninitialize();
    }
    bool uninit = false;
};

bool run_file_dialog(const std::vector<FileFilter>& filters, bool allow_multiple, bool pick_folders, std::vector<std::string>& out_paths) {
    ComScope com;
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) return false;

    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    /*
     * FOS_FILEMUSTEXIST is about files, and asking for it while picking a
     * folder is what stopped a drive's root -- H:\ and the like -- from being
     * choosable at all: the shell wants a file inside, finds none, and refuses
     * the selection. Somewhere to put an export is exactly the case where the
     * answer is a drive, or a folder that does not exist yet.
     */
    if (!pick_folders) options |= FOS_FILEMUSTEXIST;
    if (allow_multiple) options |= FOS_ALLOWMULTISELECT;
    if (pick_folders) options |= FOS_PICKFOLDERS;
    dialog->SetOptions(options);

    std::vector<std::wstring> storage;
    std::vector<COMDLG_FILTERSPEC> specs;
    if (!pick_folders && !filters.empty()) {
        storage.reserve(filters.size() * 2);
        for (const auto& f : filters) {
            storage.push_back(widen(f.label));
            storage.push_back(widen(f.pattern));
        }
        for (size_t i = 0; i < filters.size(); ++i) {
            specs.push_back({storage[i * 2].c_str(), storage[i * 2 + 1].c_str()});
        }
        dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    }

    hr = dialog->Show(GetActiveWindow());
    bool ok = false;
    if (SUCCEEDED(hr)) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->GetResults(&items)) && items) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                        out_paths.push_back(narrow(path));
                        CoTaskMemFree(path);
                        ok = true;
                    }
                    item->Release();
                }
            }
            items->Release();
        }
    }
    dialog->Release();
    return ok;
}

}  // namespace

std::string executable_dir() {
    wchar_t buffer[MAX_PATH * 4];
    DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (!len) return ".";
    return from_path(fs::path(std::wstring(buffer, len)).parent_path());
}

std::string executable_path() {
    wchar_t buffer[MAX_PATH * 4];
    DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (!len) return "";
    return from_path(fs::path(std::wstring(buffer, len)));
}

std::string steam_install_dir() {
    auto read_value = [](HKEY root, const wchar_t* key, const wchar_t* name) -> std::string {
        wchar_t buffer[2048];
        DWORD size = sizeof(buffer);
        if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return "";
        return narrow(std::wstring(buffer));
    };
    const std::string candidates[] = {
        read_value(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
        read_value(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"),
        read_value(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"),
    };
    for (const std::string& candidate : candidates) {
        if (candidate.empty()) continue;
        fs::path p = to_path(candidate);
        p.make_preferred();
        std::error_code ec;
        if (fs::is_directory(p, ec)) return from_path(p);
    }
    return "";
}

std::string home_dir() {
    std::string profile = getenv_utf8("USERPROFILE");
    return profile.empty() ? "." : profile;
}

std::string default_data_dir() {
    std::string base = getenv_utf8("LOCALAPPDATA");
    if (base.empty()) base = home_dir();
    return join(join(base, "NativeConsole"), "Hub");
}

std::string getenv_utf8(const char* name) {
    std::wstring wname = widen(name);
    DWORD len = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (!len) return "";
    std::wstring value(static_cast<size_t>(len), L'\0');
    DWORD got = GetEnvironmentVariableW(wname.c_str(), value.data(), len);
    value.resize(got);
    return narrow(value);
}

std::string find_on_path(const std::string& executable_name) {
    std::wstring name = widen(executable_name);
    if (name.find(L'.') == std::wstring::npos) name += L".exe";
    wchar_t buffer[MAX_PATH * 4];
    LPWSTR file_part = nullptr;
    DWORD len = SearchPathW(nullptr, name.c_str(), nullptr, static_cast<DWORD>(std::size(buffer)), buffer, &file_part);
    if (!len || len >= std::size(buffer)) return "";
    return narrow(std::wstring(buffer, len));
}

bool pick_files(const std::vector<FileFilter>& filters, bool allow_multiple, std::vector<std::string>& out_paths) {
    return run_file_dialog(filters, allow_multiple, false, out_paths);
}

bool pick_folder(std::string& out_path) {
    std::vector<std::string> paths;
    if (!run_file_dialog({}, false, true, paths) || paths.empty()) return false;
    out_path = paths.front();
    return true;
}

void open_in_file_manager(const std::string& path) {
    std::wstring wpath = widen(path);
    ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void show_message_box(const std::string& title, const std::string& message) {
    MessageBoxW(GetActiveWindow(), widen(message).c_str(), widen(title).c_str(), MB_OK | MB_ICONINFORMATION);
}

std::string script_command(const std::string& script_path) {
    return "cmd.exe /S /C \"\"" + script_path + "\"\"";
}

RunResult run_capture(const std::string& command_line,
                      const std::string& working_dir,
                      const std::function<void(const std::string& line)>& on_line,
                      std::atomic<bool>* cancel_flag) {
    RunResult result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        result.error = "CreatePipe failed: " + last_error_message();
        return result;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};

    // Everything the command starts goes in one job object with it. A build
    // script is cmd, which starts ninja, which starts the compilers: ending
    // cmd alone left the rest compiling for minutes after Cancel, holding
    // the files of a build nobody was waiting for. The job is also closed
    // when the hub is, which ends them then too.
    HANDLE tree = CreateJobObjectW(nullptr, nullptr);
    if (tree) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(tree, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(tree);
            tree = nullptr;
        }
    }

    std::wstring cmd = widen(command_line);
    std::wstring cwd = widen(working_dir);
    // Suspended until it is in the job, so that nothing it starts is outside.
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(write_end);
    if (!ok) {
        result.error = "CreateProcess failed: " + last_error_message();
        CloseHandle(read_end);
        if (tree) CloseHandle(tree);
        return result;
    }
    if (tree && !AssignProcessToJobObject(tree, pi.hProcess)) {
        CloseHandle(tree);
        tree = nullptr;
    }
    ResumeThread(pi.hThread);
    result.started = true;
    CloseHandle(pi.hThread);

    std::string pending;
    char buffer[4096];
    while (true) {
        if (cancel_flag && cancel_flag->load()) {
            if (tree) TerminateJobObject(tree, 130);
            else TerminateProcess(pi.hProcess, 130);
            on_line("[hub] cancelled");
            break;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available == 0) {
            if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
                // Drain anything written between the peek and exit.
                if (!PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
            } else {
                continue;
            }
        }
        DWORD got = 0;
        if (!ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr) || got == 0) break;
        pending.append(buffer, got);
        size_t start = 0;
        while (true) {
            size_t nl = pending.find('\n', start);
            if (nl == std::string::npos) break;
            std::string line = pending.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            on_line(line);
            start = nl + 1;
        }
        pending.erase(0, start);
    }
    if (!pending.empty()) {
        if (pending.back() == '\r') pending.pop_back();
        on_line(pending);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(read_end);
    if (tree) CloseHandle(tree);
    return result;
}

bool launch_detached(const std::string& executable,
                     const std::vector<std::string>& args,
                     const std::string& working_dir,
                     bool hide_console,
                     Process& out_process,
                     std::string& error) {
    std::string command = quote_arg(executable);
    for (const auto& a : args) {
        command += " ";
        command += quote_arg(a);
    }
    std::wstring cmd = widen(command);
    std::wstring cwd = widen(working_dir);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | (hide_console ? CREATE_NO_WINDOW : CREATE_NEW_CONSOLE);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        error = "CreateProcess failed: " + last_error_message();
        return false;
    }
    CloseHandle(pi.hThread);
    out_process.handle = pi.hProcess;
    out_process.pid = pi.dwProcessId;
    return true;
}

bool process_running(Process& process) {
    if (!process.handle) return false;
    return WaitForSingleObject(static_cast<HANDLE>(process.handle), 0) == WAIT_TIMEOUT;
}

void process_release(Process& process) {
    if (process.handle) CloseHandle(static_cast<HANDLE>(process.handle));
    process.handle = nullptr;
    process.pid = 0;
}

void process_terminate(Process& process) {
    if (process.handle) TerminateProcess(static_cast<HANDLE>(process.handle), 1);
}

bool tie_to_process_lifetime(Process& process) {
    if (!process.handle) return false;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)) ||
        !AssignProcessToJobObject(job, static_cast<HANDLE>(process.handle))) {
        CloseHandle(job);
        return false;
    }
    // Deliberately left open: Windows closes it when this process exits, which ends the child.
    return true;
}

bool process_running_by_name(const std::string& exe_file_name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    const std::wstring wanted = widen(exe_file_name);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, wanted.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

long long file_size_on_disk(const std::string& path) {
    DWORD high = 0;
    DWORD low = GetCompressedFileSizeW(widen(path).c_str(), &high);
    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) return file_size(path);
    return (static_cast<long long>(high) << 32) | low;
}

bool supports_transparent_compression() { return true; }

bool compress_file_transparent(const std::string& path, std::string& error) {
    std::string windir = getenv_utf8("WINDIR");
    if (windir.empty()) windir = "C:\\Windows";
    std::string compact = join(join(windir, "System32"), "compact.exe");
    std::string command = quote_arg(compact) + " /C /EXE:LZX " + quote_arg(path);
    std::string output;
    RunResult result = run_capture(command, parent_path(path), [&](const std::string& line) {
        if (!line.empty()) output += line + "\n";
    }, nullptr);
    if (!result.started) {
        error = result.error;
        return false;
    }
    if (result.exit_code != 0) {
        error = "compact.exe exited with code " + std::to_string(result.exit_code) + (output.empty() ? "" : ": " + output);
        return false;
    }
    return true;
}

bool load_image_native(const std::string& path, Image& out, std::string& error) {
    ComScope com;
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        error = "WIC factory unavailable";
        return false;
    }
    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(widen(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || !decoder) {
        factory->Release();
        error = "cannot decode image " + path;
        return false;
    }
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;
    if (SUCCEEDED(decoder->GetFrame(0, &frame)) && frame &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) && converter &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        if (w > 0 && h > 0 && w <= 16384 && h <= 16384) {
            out.width = static_cast<int>(w);
            out.height = static_cast<int>(h);
            out.rgba.resize(static_cast<size_t>(w) * h * 4);
            if (SUCCEEDED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.rgba.size()), out.rgba.data()))) {
                ok = true;
            }
        }
    }
    if (!ok) error = "cannot read pixels from " + path;
    if (converter) converter->Release();
    if (frame) frame->Release();
    decoder->Release();
    factory->Release();
    return ok;
}

bool save_image_png(const std::string& path, const Image& image, std::string& error) {
    ComScope com;
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        error = "WIC factory unavailable";
        return false;
    }
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;
    do {
        if (FAILED(factory->CreateStream(&stream)) || !stream) break;
        if (FAILED(stream->InitializeFromFilename(widen(path).c_str(), GENERIC_WRITE))) break;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) || !encoder) break;
        if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || !frame) break;
        if (FAILED(frame->Initialize(nullptr))) break;
        if (FAILED(frame->SetSize(static_cast<UINT>(image.width), static_cast<UINT>(image.height)))) break;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
        if (FAILED(frame->SetPixelFormat(&format))) break;
        std::vector<uint8_t> pixels = image.rgba;
        if (format != GUID_WICPixelFormat32bppRGBA) {
            // Encoder chose BGRA; swap channels.
            for (size_t i = 0; i + 3 < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
        }
        if (FAILED(frame->WritePixels(static_cast<UINT>(image.height), static_cast<UINT>(image.width) * 4,
                                      static_cast<UINT>(pixels.size()), pixels.data()))) break;
        if (FAILED(frame->Commit())) break;
        if (FAILED(encoder->Commit())) break;
        ok = true;
    } while (false);
    if (!ok) error = "cannot write PNG " + path;
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    factory->Release();
    return ok;
}

}  // namespace hub::platform

#endif  // _WIN32
