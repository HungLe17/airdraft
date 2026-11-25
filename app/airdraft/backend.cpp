// Copyright (c) 2026 Airdraft
// SPDX-License-Identifier: GPL-2.0-only
#include "backend.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <winsvc.h>

namespace airdraft {
namespace {
constexpr DWORD max_line = 1024 * 1024;
constexpr auto service_name = L"AirdraftRadioService";
constexpr auto service_pipe = LR"(\\.\pipe\Airdraft)";

template<class Read>
void read_lines(Read read, Mailbox& mailbox, Origin origin, unsigned generation)
{
    std::string pending;
    std::array<char, 4096> buffer{};
    for (DWORD count = 0; read(buffer.data(), static_cast<DWORD>(buffer.size()), count) && count;) {
        pending.append(buffer.data(), count);
        size_t start = 0;
        for (size_t end; (end = pending.find('\n', start)) != std::string::npos; start = end + 1) {
            auto line = pending.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) mailbox.put({origin, generation, std::move(line)});
        }
        pending.erase(0, start);
        if (pending.size() > max_line) break;
    }
}

bool genuine_service(HANDLE pipe)
{
    ULONG server = 0;
    if (!GetNamedPipeServerProcessId(pipe, &server)) return false;
    const auto scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    const auto service = OpenServiceW(scm, service_name, SERVICE_QUERY_STATUS);
    CloseServiceHandle(scm);
    if (!service) return false;
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    const bool same = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes) && status.dwProcessId == server;
    CloseServiceHandle(service);
    return same;
}

std::string service_problem(std::optional<DWORD> pipe_error)
{
    const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return "Windows could not open Service Control Manager.";
    const auto service = OpenServiceW(manager, service_name, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    const auto open_error = GetLastError();
    CloseServiceHandle(manager);
    if (!service) {
        if (open_error == ERROR_SERVICE_DOES_NOT_EXIST)
            return "The Airdraft radio service is missing. Run Airdraft Setup.";
        return "Windows could not read the Airdraft radio service (error " + std::to_string(open_error) + ").";
    }
    DWORD needed = 0;
    QueryServiceConfigW(service, nullptr, 0, &needed);
    std::vector<BYTE> config(needed);
    auto settings = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config.data());
    const bool disabled = needed && QueryServiceConfigW(service, settings, needed, &needed)
        && settings->dwStartType == SERVICE_DISABLED;
    SERVICE_STATUS status{};
    const bool queried = QueryServiceStatus(service, &status) != FALSE;
    CloseServiceHandle(service);
    if (disabled) return "The Airdraft radio service is disabled. Run Airdraft Setup again to re-enable it.";
    if (queried && status.dwCurrentState == SERVICE_STOPPED)
        return "The Airdraft radio service is stopped (service exit code " + std::to_string(status.dwWin32ExitCode)
            + "). Run Airdraft Setup again or check the Airdraft service log.";
    if (queried && status.dwCurrentState == SERVICE_START_PENDING)
        return "The Airdraft radio service is starting.";
    if (!pipe_error) return "Connecting to the Airdraft radio service. Try Status again in a moment.";
    if (*pipe_error == ERROR_SUCCESS) return "Airdraft could not verify the radio service connection.";
    return "Airdraft could not connect to the radio service (Windows error " + std::to_string(*pipe_error) + ").";
}

std::wstring quote_argument(std::wstring_view value)
{
    std::wstring argument = L"\"";
    size_t slashes = 0;
    for (const auto ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        argument.append(ch == L'"' ? 2 * slashes + 1 : slashes, L'\\');
        argument.push_back(ch);
        slashes = 0;
    }
    argument.append(2 * slashes, L'\\');
    return argument + L'"';
}
}

JsonObject command(std::wstring_view operation)
{
    JsonObject object;
    object.Insert(L"do", JsonValue::CreateStringValue(operation));
    return object;
}

std::wstring initial_service_status()
{
    return winrt::to_hstring(service_problem(std::nullopt)).c_str();
}

bool service_responding(DWORD timeout_ms)
{
    const auto deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        Handle pipe(CreateFileW(service_pipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr));
        if (!pipe || pipe.get() == INVALID_HANDLE_VALUE || !genuine_service(pipe.get())) {
            Sleep(50);
            continue;
        }
        std::array<char, 4096> bytes{};
        std::string pending;
        while (GetTickCount64() < deadline && pending.size() <= max_line) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe.get(), nullptr, 0, nullptr, &available, nullptr)) break;
            if (!available) {
                Sleep(50);
                continue;
            }
            DWORD read = 0;
            if (!ReadFile(pipe.get(), bytes.data(), std::min(available, static_cast<DWORD>(bytes.size())),
                          &read, nullptr) || !read) break;
            pending.append(bytes.data(), read);
            size_t start = 0;
            for (size_t end; (end = pending.find('\n', start)) != std::string::npos; start = end + 1) {
                JsonObject state;
                if (!JsonObject::TryParse(winrt::to_hstring(std::string_view(pending).substr(start, end - start)), state) ||
                    state.GetNamedString(L"event", L"") != L"state" || state.GetNamedBoolean(L"stopPending", true))
                    continue;
                const auto radio = state.GetNamedString(L"radio", L"");
                if (!radio.empty()) return radio != L"error";
            }
            pending.erase(0, start);
        }
        Sleep(50);
    }
    return false;
}

void Mailbox::put(Event event)
{
    {
        std::scoped_lock lock(mutex_);
        pending_.push_back(std::move(event));
    }
    PostMessageW(window_, backend_message, 0, 0);
}

std::vector<Event> Mailbox::drain()
{
    std::scoped_lock lock(mutex_);
    return std::exchange(pending_, {});
}

Service::Service(Mailbox& mailbox) :
    mailbox_(mailbox), stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
    worker_([this](std::stop_token stop) { run(stop); })
{
}

void Service::stop()
{
    if (!worker_.joinable()) return;
    worker_.request_stop();
    SetEvent(stop_event_.get());
    worker_.join();
}

void Service::run(std::stop_token stop)
{
    unsigned generation = 0;
    std::string previous_problem;
    while (!stop.stop_requested()) {
        // Request client rights individually. GENERIC_WRITE also requests permission
        // to create pipe instances, which the service grants only to SYSTEM.
        constexpr DWORD access = FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES;
        Handle pipe(CreateFileW(service_pipe, access, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        const auto pipe_error = pipe.get() == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (pipe.get() != INVALID_HANDLE_VALUE && genuine_service(pipe.get())) {
            previous_problem.clear();
            mailbox_.put({Origin::service_status, 0, "Waiting for the Airdraft radio service to accept this session."});
            {
                std::scoped_lock lock(mutex_);
                pipe_ = pipe.get();
            }
            ++generation;
            Handle signal(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            read_lines([&](char* data, DWORD size, DWORD& count) {
                OVERLAPPED io{};
                io.hEvent = signal.get();
                ResetEvent(signal.get());
                if (ReadFile(pipe.get(), data, size, &count, &io)) return true;
                if (GetLastError() != ERROR_IO_PENDING) return false;
                HANDLE waits[] = {stop_event_.get(), signal.get()};
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
                    return GetOverlappedResult(pipe.get(), &io, &count, FALSE) != FALSE;
                CancelIoEx(pipe.get(), &io);
                GetOverlappedResult(pipe.get(), &io, &count, TRUE);
                return false;
            }, mailbox_, Origin::service, generation);
            {
                std::scoped_lock lock(mutex_);
                pipe_ = nullptr;
            }
            mailbox_.put({Origin::service, generation, {}});
        } else {
            auto problem = service_problem(pipe_error);
            if (problem != previous_problem) {
                previous_problem = problem;
                mailbox_.put({Origin::service_status, 0, std::move(problem)});
            }
        }
        if (WaitForSingleObject(stop_event_.get(), 2000) != WAIT_TIMEOUT) return;
    }
}

bool Service::send(const JsonObject& object)
{
    auto text = winrt::to_string(object.Stringify()) + '\n';
    std::scoped_lock lock(mutex_);
    if (!pipe_ || text.size() > max_line) return false;
    Handle signal(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!signal) return false;
    OVERLAPPED io{};
    io.hEvent = signal.get();
    DWORD written = 0;
    const auto size = static_cast<DWORD>(text.size());
    if (WriteFile(pipe_, text.data(), size, &written, &io)) return written == size;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(signal.get(), 300) != WAIT_OBJECT_0) {
        CancelIoEx(pipe_, &io);
        GetOverlappedResult(pipe_, &io, &written, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe_, &io, &written, FALSE) && written == size;
}

bool Engine::start(const fs::path& root, const fs::path& downloads, const fs::path& keys,
                   std::wstring_view instance, std::wstring_view name, bool receive, std::wstring& error)
{
    stop();
    const auto python = root / L"python" / L"python.exe";
    const auto script = root / L"airdrop" / L"engine.py";
    if (!fs::is_regular_file(python) || !fs::is_regular_file(script)) {
        error = L"Transfer engine files are missing. Reinstall Airdraft.";
        return false;
    }
    std::error_code folder_error;
    fs::create_directories(downloads, folder_error);
    if (folder_error) {
        error = L"The incoming files folder is unavailable. Choose a folder in Settings.";
        return false;
    }
    if (!ensure_identity(keys)) {
        error = L"Consent and transfer identity could not be initialized.";
        return false;
    }
    auto args = quote_argument(python.native()) + L" -u " + quote_argument(script.native())
        + L" --outdir " + quote_argument(downloads.native()) + L" --keys " + quote_argument(keys.native())
        + L" --service-id " + std::wstring(instance) + L" --name=" + quote_argument(name) + L" --external-beacon";
    if (!receive) args += L" --send-only";

    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE parent_in{}, child_in{}, parent_out{}, child_out{};
    if (!CreatePipe(&child_in, &parent_in, &attributes, 0)) {
        error = L"Could not open the transfer engine input.";
        return false;
    }
    Handle input(parent_in), input_child(child_in);
    if (!CreatePipe(&parent_out, &child_out, &attributes, 0)) {
        error = L"Could not open the transfer engine output.";
        return false;
    }
    Handle output(parent_out), output_child(child_out);
    SetHandleInformation(input.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(output.get(), HANDLE_FLAG_INHERIT, 0);
    const auto log_file = keys.parent_path() / L"engine.log";
    std::error_code io_error;
    const auto log_size = fs::file_size(log_file, io_error);
    if (!io_error && log_size > 1024 * 1024)
        MoveFileExW(log_file.c_str(), (keys.parent_path() / L"engine.log.1").c_str(), MOVEFILE_REPLACE_EXISTING);
    Handle stderr_log(CreateFileW(log_file.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &attributes, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        error = L"Could not create the transfer engine job.";
        return false;
    }
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input_child.get();
    startup.hStdOutput = output_child.get();
    startup.hStdError = stderr_log.get() == INVALID_HANDLE_VALUE ? output_child.get() : stderr_log.get();
    PROCESS_INFORMATION launched{};
    if (!CreateProcessW(python.c_str(), args.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                        nullptr, script.parent_path().c_str(), &startup, &launched)) {
        error = L"Could not launch the transfer engine (" + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    Handle process(launched.hProcess), thread(launched.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get()) || ResumeThread(thread.get()) == DWORD(-1)) {
        TerminateProcess(process.get(), 1);
        error = L"Could not start the supervised transfer engine.";
        return false;
    }
    process_ = std::move(process);
    job_ = std::move(job);
    input_ = std::move(input);
    const auto generation = ++generation_;
    reader_ = std::jthread([this, pipe = std::move(output), generation] {
        read_lines([&](char* data, DWORD size, DWORD& count) {
            return ReadFile(pipe.get(), data, size, &count, nullptr) != FALSE;
        }, mailbox_, Origin::engine, generation);
        mailbox_.put({Origin::engine, generation, {}});
    });
    return true;
}

void Engine::stop()
{
    if (!process_) return;
    ++generation_;
    // EOF asks the engine to close its listener and unregister mDNS.
    input_.reset();
    WaitForSingleObject(process_.get(), 1200);
    job_.reset();
    if (reader_.joinable()) reader_.join();
    process_.reset();
}

bool Engine::running() const
{
    return process_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT;
}

bool Engine::send(const JsonObject& object)
{
    if (!running()) return false;
    auto text = winrt::to_string(object.Stringify()) + '\n';
    DWORD written = 0;
    return text.size() <= max_line &&
        WriteFile(input_.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
}
}
