#include "core.hpp"

#include <sddl.h>
#include <shellapi.h>
#include <winrt/base.h>

#include <array>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using m720::Log;
namespace fs = std::filesystem;
constexpr wchar_t transport_mutex[] = L"Local\\M720Mapper-B015"; // interoperates with the previous mapper
constexpr std::uintmax_t log_limit = 1024 * 1024;
constexpr DWORD shutdown_timeout = 45000;

struct LocalMemory {
    HLOCAL pointer{};
    ~LocalMemory() { if (pointer) LocalFree(pointer); }
};

class InstanceSecurity {
public:
    InstanceSecurity() {
        winrt::handle token;
        winrt::check_bool(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()));
        DWORD bytes{};
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
        if (!bytes) winrt::throw_last_error();
        std::vector<std::max_align_t> buffer((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        winrt::check_bool(GetTokenInformation(token.get(), TokenUser, buffer.data(), bytes, &bytes));
        const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
        PWSTR sid{};
        winrt::check_bool(ConvertSidToStringSidW(user->User.Sid, &sid));
        const LocalMemory sid_memory{sid};
        prefix_ = std::wstring(L"Local\\M720Mapper.Native.") + sid;
        const auto sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
        PSECURITY_DESCRIPTOR descriptor{};
        winrt::check_bool(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
            &descriptor, nullptr));
        descriptor_.pointer = descriptor;
        attributes_.lpSecurityDescriptor = descriptor;
    }

    SECURITY_ATTRIBUTES* attributes() { return &attributes_; }
    std::wstring name(std::wstring_view kind) const { return prefix_ + L"." + std::wstring(kind); }

private:
    LocalMemory descriptor_;
    SECURITY_ATTRIBUTES attributes_{sizeof(attributes_), nullptr, FALSE};
    std::wstring prefix_;
};

fs::path app_directory() {
    const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!size) winrt::throw_last_error();
    std::wstring value(size, L'\0');
    const auto count = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), size);
    if (!count || count >= size) winrt::throw_last_error();
    value.resize(count);
    return fs::path(value) / L"M720Mapper";
}

std::string time_stamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond);
}

void console(std::string_view message, DWORD stream = STD_OUTPUT_HANDLE) {
    const auto handle = GetStdHandle(stream);
    if (!handle || handle == INVALID_HANDLE_VALUE) return;
    DWORD ignored{};
    DWORD mode{};
    if (GetConsoleMode(handle, &mode)) {
        const auto line = winrt::to_hstring(std::string(message) + "\r\n");
        WriteConsoleW(handle, line.c_str(), line.size(), &ignored, nullptr);
    } else {
        const auto line = std::string(message) + "\r\n";
        WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &ignored, nullptr);
    }
}

class FileLogger {
public:
    explicit FileLogger(fs::path path, bool echo) : path_(std::move(path)), echo_(echo) {}

    void write(std::string_view message) {
        std::scoped_lock lock(mutex_);
        if (echo_) console(message);
        const auto line = time_stamp() + " " + std::string(message) + "\r\n";
        std::error_code error;
        fs::create_directories(path_.parent_path(), error);
        if (error) return;
        const auto size = fs::file_size(path_, error);
        if (!error && size + line.size() > log_limit) {
            auto previous = path_;
            previous += L".1";
            fs::remove(previous, error);
            if (error) return;
            fs::rename(path_, previous, error);
            if (error) return;
        }
        std::ofstream file(path_, std::ios::binary | std::ios::app);
        if (file) file.write(line.data(), static_cast<std::streamsize>(line.size()));
    }

private:
    fs::path path_;
    bool echo_;
    std::mutex mutex_;
};

class MutexLease {
public:
    explicit MutexLease(HANDLE mutex) : mutex_(mutex) {
        const auto result = WaitForSingleObject(mutex_, 0);
        if (result == WAIT_FAILED) winrt::throw_last_error();
        owned_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    ~MutexLease() { if (owned_) ReleaseMutex(mutex_); }
    explicit operator bool() const { return owned_; }
private:
    HANDLE mutex_;
    bool owned_{};
};

winrt::handle ipc_event(InstanceSecurity& security, std::wstring_view name) {
    winrt::handle result{CreateEventW(security.attributes(), TRUE, FALSE, security.name(name).c_str())};
    winrt::check_bool(static_cast<bool>(result));
    winrt::check_bool(ResetEvent(result.get()));
    return result;
}

winrt::handle open_ipc_event(const InstanceSecurity& security, std::wstring_view name, DWORD rights) {
    winrt::handle result{OpenEventW(rights, FALSE, security.name(name).c_str())};
    if (!result && GetLastError() != ERROR_FILE_NOT_FOUND) winrt::throw_last_error();
    return result;
}

winrt::handle open_transport_mutex() {
    winrt::handle result{CreateMutexW(nullptr, FALSE, transport_mutex)};
    winrt::check_bool(static_cast<bool>(result));
    return result;
}

int resident(bool echo) {
    InstanceSecurity security;
    winrt::handle instance{CreateMutexW(security.attributes(), FALSE, security.name(L"instance").c_str())};
    winrt::check_bool(static_cast<bool>(instance));
    const MutexLease owner(instance.get());
    if (!owner) {
        if (echo) console("M720 Mapper is already running.");
        return 0;
    }

    FileLogger logger(app_directory() / L"Mapper.log", echo);
    const Log log = [&logger](std::string_view message) { logger.write(message); };
    auto transport = open_transport_mutex();
    const MutexLease transport_owner(transport.get());
    if (!transport_owner) {
        log("M720 Mapper or the previous mapper already owns the Bluetooth transport.");
        return 2;
    }

    auto active = ipc_event(security, L"active");
    auto waiting = ipc_event(security, L"waiting");
    auto finished = ipc_event(security, L"finished");
    auto failed = ipc_event(security, L"failed");
    auto stop = ipc_event(security, L"stop");
    try {
        m720::run_mapper(stop.get(), active.get(), waiting.get(), log);
        SetEvent(finished.get());
        return 0;
    } catch (const winrt::hresult_error& ex) {
        log(winrt::to_string(ex.message()));
    } catch (const std::exception& ex) {
        log(ex.what());
    }
    SetEvent(failed.get());
    return 2;
}

int status_or_stop(bool stop_requested) {
    InstanceSecurity security;
    winrt::handle instance{OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, security.name(L"instance").c_str())};
    if (!instance) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) winrt::throw_last_error();
        console("M720 Mapper is stopped in this session.");
        return stop_requested ? 0 : 3;
    }
    const MutexLease stopped(instance.get());
    if (stopped) {
        console("M720 Mapper is stopped in this session.");
        return stop_requested ? 0 : 3;
    }
    auto stop = open_ipc_event(security, L"stop", stop_requested ? EVENT_MODIFY_STATE : SYNCHRONIZE);
    if (!stop) {
        console("M720 Mapper is initializing. Try again shortly.");
        return 4;
    }

    if (!stop_requested) {
        auto active = open_ipc_event(security, L"active", SYNCHRONIZE);
        if (active && WaitForSingleObject(active.get(), 0) == WAIT_OBJECT_0) {
            console("M720 Mapper is running and the shortcuts are active.");
            return 0;
        }
        auto waiting = open_ipc_event(security, L"waiting", SYNCHRONIZE);
        if (waiting && WaitForSingleObject(waiting.get(), 0) == WAIT_OBJECT_0) {
            console("M720 Mapper is running and waiting for the Bluetooth M720.");
            return 1;
        }
        console("M720 Mapper is initializing or retrying the Bluetooth connection.");
        return 4;
    }

    auto finished = open_ipc_event(security, L"finished", SYNCHRONIZE);
    auto failed = open_ipc_event(security, L"failed", SYNCHRONIZE);
    if (!finished || !failed) {
        console("M720 Mapper already stopped.");
        return 0;
    }
    winrt::check_bool(SetEvent(stop.get()));
    const std::array completions{finished.get(), failed.get()};
    const auto result = WaitForMultipleObjects(static_cast<DWORD>(completions.size()), completions.data(),
        FALSE, shutdown_timeout);
    if (result == WAIT_FAILED) winrt::throw_last_error();
    if (result == WAIT_OBJECT_0) {
        console("M720 Mapper stopped gracefully.");
        return 0;
    }
    if (result == WAIT_OBJECT_0 + 1) {
        console("M720 Mapper reported an error during shutdown. See Mapper.log.", STD_ERROR_HANDLE);
        return 2;
    }
    console("M720 Mapper has not acknowledged shutdown. See Mapper.log.", STD_ERROR_HANDLE);
    return 2;
}

int listen_seconds(const wchar_t* value) {
    const auto text = winrt::to_string(value);
    int seconds{};
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, seconds);
    if (result.ec != std::errc{} || result.ptr != end || seconds < 1 || seconds > 120)
        throw std::invalid_argument("Listen duration must be an integer from 1 to 120 seconds.");
    return seconds;
}

int cli(int argc, wchar_t** argv) {
    if (argc == 1) return resident(false);
    const std::wstring_view command(argv[1]);
    if (argc > (command == L"--listen" ? 3 : 2)) throw std::invalid_argument("Unexpected arguments. Use --help.");

    if (command == L"--help") {
        console("M720Mapper.exe            Run hidden. Use --stop to stop.");
        console("M720Mapper.exe --run      Run now and print log messages.");
        console("M720Mapper.exe --status   Show status for this user and session.");
        console("M720Mapper.exe --stop     Restore reporting and stop gracefully.");
        console("M720Mapper.exe --diagnose Enumerate and read the GATT service.");
        console("M720Mapper.exe --probe    Query HID++ and button reporting.");
        console("M720Mapper.exe --listen [seconds]  Listen for already-diverted buttons (1-120, default 30).");
        return 0;
    }
    if (command == L"--run") return resident(true);
    if (command == L"--status") return status_or_stop(false);
    if (command == L"--stop") return status_or_stop(true);
    const Log output = [](std::string_view message) { console(message); };
    if (command == L"--diagnose") {
        m720::diagnose(output);
        return 0;
    }
    if (command != L"--probe" && command != L"--listen") throw std::invalid_argument("Unknown option. Use --help.");
    auto mutex = open_transport_mutex();
    const MutexLease owner(mutex.get());
    if (!owner) throw std::runtime_error("M720 Mapper already owns the transport. Stop it before running this diagnostic.");
    if (command == L"--probe") m720::probe(output);
    else m720::listen(std::chrono::seconds(argc == 3 ? listen_seconds(argv[2]) : 30), output);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc{};
    auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const LocalMemory arguments{argv};
    if (!argv) return 2;
    if (argc > 1) {
        // The console inherits redirections when launched through PowerShell or a script.
        AttachConsole(ATTACH_PARENT_PROCESS);
    }
    try {
        return cli(argc, argv);
    } catch (const winrt::hresult_error& ex) {
        console(winrt::to_string(ex.message()), STD_ERROR_HANDLE);
    } catch (const std::exception& ex) {
        console(ex.what(), STD_ERROR_HANDLE);
    }
    return 2;
}
