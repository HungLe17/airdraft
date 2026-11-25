// Copyright (c) 2026 Airdraft
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <windows.h>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

namespace airdraft {
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonArray;
namespace fs = std::filesystem;

inline constexpr UINT backend_message = WM_APP + 2;
bool service_responding(DWORD timeout_ms);

struct CloseHandleDeleter {
    void operator()(void* handle) const noexcept { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

enum class Origin { service, service_status, engine, bluetooth, beacon };
struct Event {
    Origin origin;
    unsigned generation;
    std::string line;
};

class Mailbox {
public:
    explicit Mailbox(HWND window) : window_(window) {}
    void put(Event event);
    std::vector<Event> drain();
private:
    HWND window_;
    std::mutex mutex_;
    std::vector<Event> pending_;
};

class Service {
public:
    explicit Service(Mailbox& mailbox);
    ~Service() { stop(); }
    bool send(const JsonObject& object);
    void stop();
private:
    void run(std::stop_token stop);
    Mailbox& mailbox_;
    Handle stop_event_;
    std::mutex mutex_;
    HANDLE pipe_ = nullptr;
    std::jthread worker_;
};

class Engine {
public:
    explicit Engine(Mailbox& mailbox) : mailbox_(mailbox) {}
    ~Engine() { stop(); }
    bool start(const fs::path& root, const fs::path& downloads, const fs::path& keys,
               std::wstring_view instance, std::wstring_view name, bool receive, std::wstring& error);
    void stop();
    bool running() const;
    bool send(const JsonObject& object);
    unsigned generation() const { return generation_; }
private:
    Mailbox& mailbox_;
    Handle process_, job_, input_;
    std::jthread reader_;
    unsigned generation_ = 0;
};

JsonObject command(std::wstring_view operation);
std::wstring initial_service_status();
bool ensure_identity(const fs::path& folder);
}
