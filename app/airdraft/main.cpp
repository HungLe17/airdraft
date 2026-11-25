// Copyright (c) 2026 Airdraft
// SPDX-License-Identifier: GPL-2.0-only
#include "backend.h"
#include "resource.h"
#include "window_style.h"

#include <commctrl.h>
#include <bcrypt.h>
#include <netioapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <algorithm>
#include <array>
#include <climits>
#include <deque>
#include <map>
#include <set>
#include <type_traits>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Storage.Streams.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace airdraft {
namespace {
constexpr auto window_class = L"AirdraftTrayWindow";
constexpr auto scan_class = L"AirdraftScanWindow";
constexpr auto prefs = LR"(Software\Airdraft)";
constexpr UINT tray_message = WM_APP + 1, prompt_message = WM_APP + 3, open_window_message = WM_APP + 4;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
enum MenuId { send_files = 101, receive_files, open_downloads, show_status, about, exit_app, send_now, cancel_send,
    experimental_radio, offline_radio, scan_devices, scan_status_label, show_settings, review_files };

constexpr std::wstring_view receive_state(bool enabled, bool unlocked, bool recovering, bool wifi_protected,
                                          bool issue, bool listening)
{
    if (!enabled) return L"Paused";
    if (!unlocked) return L"Locked";
    if (recovering) return L"Recovering";
    if (wifi_protected) return L"Wi-Fi protected";
    if (issue) return L"Action needed";
    return listening ? L"Ready" : L"Recovering";
}

static_assert(receive_state(false, true, true, false, false, true) == L"Paused");
static_assert(receive_state(true, true, true, false, false, true) == L"Recovering");
static_assert(receive_state(true, true, false, false, false, false) == L"Recovering");
static_assert(receive_state(true, true, false, false, false, true) == L"Ready");
static_assert(receive_state(true, true, false, true, false, false) == L"Wi-Fi protected");

std::string beacon_error(winrt::Windows::Devices::Bluetooth::BluetoothError error)
{
    using enum winrt::Windows::Devices::Bluetooth::BluetoothError;
    switch (error) {
    case DisabledByUser: return "Bluetooth is off. Turn it on in Windows Settings.";
    case DisabledByPolicy: return "Windows policy blocks Bluetooth advertising.";
    case NotSupported: return "This Bluetooth adapter cannot advertise.";
    case RadioNotAvailable: return "The Bluetooth radio is unavailable.";
    case ResourceInUse: return "Windows says Bluetooth advertising is busy.";
    case ConsentRequired: return "Windows requires permission to advertise over Bluetooth.";
    default: return "Windows stopped Bluetooth advertising (error " + std::to_string(static_cast<int>(error)) + ").";
    }
}

winrt::fire_and_forget probe_bluetooth(std::weak_ptr<Mailbox> mailbox)
{
    co_await winrt::resume_background();
    std::string result;
    try {
        const auto adapter = co_await winrt::Windows::Devices::Bluetooth::BluetoothAdapter::GetDefaultAsync();
        if (!adapter || !adapter.IsLowEnergySupported()) result = "No Bluetooth LE adapter was found.";
        else {
            const auto radio = co_await adapter.GetRadioAsync();
            if (radio && radio.State() != winrt::Windows::Devices::Radios::RadioState::On)
                result = "Bluetooth is off. Turn it on in Windows Settings.";
        }
    } catch (const winrt::hresult_error&) {
        result = "Windows could not query Bluetooth. Check Bluetooth in Settings.";
    }
    if (const auto target = mailbox.lock()) target->put({Origin::bluetooth, 0, std::move(result)});
}

fs::path known_folder(REFKNOWNFOLDERID folder)
{
    PWSTR path = nullptr;
    winrt::check_hresult(SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &path));
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(path, CoTaskMemFree);
    return fs::path(path);
}

fs::path executable_folder()
{
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) throw std::runtime_error("Cannot locate the executable.");
    path.resize(length);
    return fs::path(path).parent_path();
}

DWORD receive_preference()
{
    DWORD enabled = 1, size = sizeof(enabled);
    if (RegGetValueW(HKEY_CURRENT_USER, prefs, L"Receiving", RRF_RT_REG_DWORD, nullptr, &enabled, &size) != ERROR_SUCCESS)
        return 1;
    return enabled;
}

std::wstring receiver_id()
{
    wchar_t saved[13]{};
    DWORD size = sizeof(saved);
    if (RegGetValueW(HKEY_CURRENT_USER, prefs, L"ReceiverId", RRF_RT_REG_SZ, nullptr, saved, &size) == ERROR_SUCCESS &&
        wcslen(saved) == 12 && std::all_of(saved, saved + 12, [](wchar_t c) { return iswxdigit(c) != 0; }))
        return saved;
    std::array<UCHAR, 6> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("Cannot create a receiver identifier.");
    constexpr auto hex = L"0123456789abcdef";
    for (size_t i = 0; i < bytes.size(); ++i) {
        saved[2 * i] = hex[bytes[i] >> 4];
        saved[2 * i + 1] = hex[bytes[i] & 15];
    }
    RegSetKeyValueW(HKEY_CURRENT_USER, prefs, L"ReceiverId", REG_SZ, saved, sizeof(saved));
    return saved;
}

bool session_unlocked()
{
    LPWSTR buffer = nullptr;
    DWORD length = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx,
                                     &buffer, &length)) return false;
    const auto info = reinterpret_cast<const WTSINFOEXW*>(buffer);
    const bool unlocked = length >= sizeof(*info) && info->Level == 1 &&
        info->Data.WTSInfoExLevel1.SessionState == WTSActive &&
        info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_UNLOCK;
    WTSFreeMemory(buffer);
    return unlocked;
}

NET_IFINDEX awdl_index()
{
    NET_LUID luid{};
    NET_IFINDEX index = 0;
    if (ConvertInterfaceAliasToLuid(L"awdl0", &luid) == NO_ERROR) ConvertInterfaceLuidToIndex(&luid, &index);
    return index;
}

std::wstring string(const JsonObject& object, std::wstring_view name)
{
    if (!object.HasKey(name)) return {};
    const auto value = object.GetNamedValue(name);
    return value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String
        ? std::wstring(value.GetString()) : L"";
}

double number(const JsonObject& object, std::wstring_view name, double fallback = 0)
{
    if (!object.HasKey(name)) return fallback;
    const auto value = object.GetNamedValue(name);
    return value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Number
        ? value.GetNumber() : fallback;
}

struct Incoming {
    std::wstring id, sender, summary;
};

struct NearbyDevice {
    std::wstring name, model;
};

struct ScanRow {
    std::wstring id, label;
    int icon;
};

constexpr size_t max_device_name = 63;

bool valid_device_name(std::wstring_view name)
{
    return !name.empty() && name.size() <= max_device_name && std::ranges::none_of(name, [](wchar_t ch) {
        return ch < 0x20 || (ch >= 0x7F && ch <= 0x9F) || (ch >= 0x200B && ch <= 0x200F) ||
            (ch >= 0x202A && ch <= 0x202E) || (ch >= 0x2066 && ch <= 0x2069) || ch == 0xFEFF ||
            std::wstring_view(L"/\\<>:\"|?*").find(ch) != std::wstring_view::npos;
    });
}

struct Preferences {
    std::wstring name;
    fs::path folder;
    bool show_pending = true;
    bool notifications = true;
};

Preferences default_preferences()
{
    wchar_t hostname[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD count = ARRAYSIZE(hostname);
    std::wstring name = GetComputerNameW(hostname, &count) ? hostname : L"Airdraft";
    if (!valid_device_name(name)) name = L"Airdraft";
    return {std::move(name), known_folder(FOLDERID_Downloads) / L"Airdraft"};
}

Preferences load_preferences()
{
    auto result = default_preferences();
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, prefs, L"Settings", RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        !bytes || bytes > 256 * 1024) return result;
    std::wstring saved(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, prefs, L"Settings", RRF_RT_REG_SZ, nullptr, saved.data(), &bytes) != ERROR_SUCCESS)
        return result;
    JsonObject object;
    if (!JsonObject::TryParse(saved.c_str(), object)) return result;
    const auto name = string(object, L"name");
    if (valid_device_name(name)) result.name = name;
    const fs::path folder = string(object, L"folder");
    if (folder.is_absolute()) result.folder = folder;
    const auto flag = [&](std::wstring_view key) {
        if (!object.HasKey(key)) return true;
        const auto value = object.GetNamedValue(key);
        return !value || value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Boolean || value.GetBoolean();
    };
    result.show_pending = flag(L"showPending");
    result.notifications = flag(L"notifications");
    return result;
}

bool save_preferences(const Preferences& settings)
{
    JsonObject object;
    object.Insert(L"name", JsonValue::CreateStringValue(settings.name));
    object.Insert(L"folder", JsonValue::CreateStringValue(settings.folder.native()));
    object.Insert(L"showPending", JsonValue::CreateBooleanValue(settings.show_pending));
    object.Insert(L"notifications", JsonValue::CreateBooleanValue(settings.notifications));
    const auto data = object.Stringify();
    return RegSetKeyValueW(HKEY_CURRENT_USER, prefs, L"Settings", REG_SZ, data.c_str(),
        static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

std::wstring edit_text(HWND dialog, int id)
{
    const auto edit = GetDlgItem(dialog, id);
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
    text.resize(GetWindowTextW(edit, text.data(), static_cast<int>(text.size())));
    return text;
}

void set_label(HWND control, const std::wstring& label)
{
    std::wstring current(label.size() + 1, L'\0');
    if (GetWindowTextLengthW(control) == static_cast<int>(label.size()) &&
        GetWindowTextW(control, current.data(), static_cast<int>(current.size())) == static_cast<int>(label.size()) &&
        std::wstring_view(current.data(), label.size()) == label) return;
    SetWindowTextW(control, label.c_str());
}

void set_visible(HWND control, bool visible)
{
    if (((GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0) != visible)
        SetWindowPos(control, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

class Tray {
public:
    explicit Tray(HINSTANCE instance) : instance_(instance), root_(executable_folder()),
        keys_(known_folder(FOLDERID_LocalAppData) / L"Airdraft" / L"keys"),
        preferences_(load_preferences())
    {}
    int run(bool background);

private:
    template<LRESULT (Tray::*method)(HWND, UINT, WPARAM, LPARAM)>
    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE) {
            auto info = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(info->lpCreateParams));
        }
        auto me = reinterpret_cast<Tray*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (me) {
            try { return (me->*method)(window, message, wparam, lparam); }
            catch (const winrt::hresult_error& e) { me->failure(std::wstring(e.message())); }
            catch (const std::exception& e) { me->failure(winrt::to_hstring(e.what()).c_str()); }
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT on_tray(HWND, UINT, WPARAM, LPARAM);
    LRESULT on_scan(HWND, UINT, WPARAM, LPARAM);
    void add_icon();
    void menu(POINT position);
    void act(int id);
    void update_status();
    void failure(std::wstring reason);
    void show_status_dialog();
    void open_settings();
    static INT_PTR CALLBACK settings_dialog(HWND, UINT, WPARAM, LPARAM);
    void apply_preferences(HWND dialog);
    void balloon(std::wstring_view text);
    void pick(std::wstring recipient = {});
    void drop_files(HWND window, HDROP drop);
    void queue_files(std::vector<std::wstring> files, std::wstring_view recipient, bool select_target);
    void review_queue();
    static INT_PTR CALLBACK review_dialog(HWND, UINT, WPARAM, LPARAM);
    void session_action(HWND owner);
    std::wstring scan_recipient(int item) const;
    void open_scan();
    void configure_scan(HWND window, UINT dpi);
    void layout_scan(HWND window);
    void paint_scan(HWND window, HDC dc) const;
    COLORREF status_surface() const;
    RECT recipient_tile(RECT icon) const;
    int scan_item_at(POINT point) const;
    LRESULT draw_recipient(NMLVCUSTOMDRAW& item) const;
    void toggle_offline(HWND owner);
    void send_selection();
    void update_send_controls();
    void refresh_scan_peers();
    void events();
    void on_service(const Event&);
    void on_engine(const Event&);
    void tick();
    void sync_discovery();
    std::wstring discovery_status() const;
    void update_discovery_hint();
    void start_beacon();
    void stop_beacon();
    void on_beacon(const Event&);
    void stop_engine();
    void change_intent();
    void ask(const JsonObject&);
    void prompt();
    void answer(std::wstring_view id, bool accept);
    void close_prompt(std::wstring_view id);
    void shutdown();

    HINSTANCE instance_;
    HWND window_{}, progress_{}, message_{}, prompt_{};
    HWND send_button_{}, cancel_button_{}, review_button_{}, review_window_{};
    HWND scan_{}, scan_heading_{}, scan_receive_{}, scan_send_{}, scan_downloads_{}, scan_details_{};
    HWND scan_peers_{}, scan_status_{}, scan_session_{}, scan_identity_{}, scan_result_{}, scan_queue_{}, scan_receive_status_{};
    HWND scan_settings_{}, settings_window_{};
    HWND scan_status_heading_{}, scan_devices_heading_{}, scan_transfer_heading_{}, scan_empty_{};
    HFONT scan_heading_font_{}, scan_section_font_{}, scan_body_font_{};
    RECT scan_band_{}, scan_devices_panel_{}, scan_transfer_panel_{}, scan_queue_panel_{};
    UINT scan_dpi_ = USER_DEFAULT_SCREEN_DPI;
    ui::Palette scan_colors_ = ui::palette();
    HIMAGELIST scan_icons_{};
    int scan_default_icon_ = I_IMAGENONE, scan_phone_icon_ = I_IMAGENONE, scan_computer_icon_ = I_IMAGENONE;
    UINT taskbar_created_{};
    fs::path root_, keys_;
    Preferences preferences_;
    NOTIFYICONDATAW icon_{};
    std::shared_ptr<Mailbox> mailbox_;
    HPOWERNOTIFY suspend_notifications_ = nullptr;
    std::unique_ptr<Service> service_;
    std::unique_ptr<Engine> engine_;
    BluetoothLEAdvertisementPublisher beacon_{nullptr};
    winrt::event_token beacon_token_{};
    bool enabled_ = receive_preference() != 0, unlocked_ = session_unlocked(), suspended_ = false;
    bool closing_ = false, listening_ = false, leased_ = false, choosing_files_ = false, cancelling_send_ = false;
    bool awake_recovery_ = false, wake_sent_ = false;
    bool stop_sent_ = false, wifi_handoff_pending_ = false;
    bool stop_pending_ = false;
    bool wifi_return_pending_ = false, wifi_return_incomplete_ = false;
    bool experimental_radio_ = false, experimental_session_ = false;
    bool offline_session_ = false;
    bool engine_link_ = false, look_requested_ = false, looking_ = false;
    unsigned attempts_ = 0, service_generation_ = 0;
    ULONGLONG retry_ = 0, bluetooth_after_ = 0, lease_retry_ = 0;
    ULONGLONG look_retry_ = 0, beacon_retry_ = 0, beacon_deadline_ = 0;
    unsigned look_attempts_ = 0, beacon_generation_ = 0;
    unsigned awdl_peer_count_ = 0;
    bool beacon_started_ = false, receiver_hidden_ = false;
    NET_IFINDEX engine_index_ = 0;
    std::wstring instance_id_ = receiver_id(), radio_, wifi_, mode_, card_, detail_, error_, state_, driver_change_;
    std::wstring bluetooth_;
    std::wstring look_error_, beacon_error_, shown_scan_status_;
    std::wstring service_detail_;
    std::wstring active_send_, prompt_id_;
    std::wstring transfer_message_, transfer_target_;
    unsigned transfer_percent_ = 0;
    std::deque<Incoming> requests_;
    std::set<std::wstring> pending_;
    std::map<std::wstring, NearbyDevice> devices_;
    std::map<std::wstring, std::wstring> discovery_candidates_;
    std::vector<std::wstring> selected_files_;
    std::vector<ScanRow> scan_rows_;
    bool can_receive() const { return enabled_ && unlocked_ && !suspended_ && !closing_ &&
        (!experimental_radio_ || experimental_session_ || offline_session_); }
    bool wants_radio() const { return experimental_radio_ && !offline_session_ ? experimental_session_ :
        (!offline_session_ || unlocked_) && (enabled_ || scan_ || !active_send_.empty()); }
    bool link_ready() const { return leased_ && engine_link_ && radio_ == L"on" && unlocked_ && !suspended_ &&
        !stop_sent_ && !stop_pending_ && !wifi_handoff_pending_ && !wifi_return_pending_ && !wifi_return_incomplete_; }
    bool radio_mode_available() const { return mode_ == L"concurrent" ||
        ((experimental_radio_ || offline_session_) && mode_ == L"ondemand"); }
    bool can_toggle_offline() const { return leased_ && unlocked_ && !suspended_ &&
        !stop_sent_ && !stop_pending_ && !wifi_return_pending_ && !wifi_return_incomplete_ &&
        (offline_session_ || ((enabled_ || scan_) && (radio_ == L"wifiprotected" ||
            (experimental_radio_ && mode_ == L"ondemand" && !experimental_session_ && (radio_ == L"off" || radio_ == L"error"))))); }
    bool can_toggle_experimental() const { return experimental_radio_ && !offline_session_ && leased_ && mode_ == L"ondemand" &&
        unlocked_ && !suspended_ && !stop_sent_ && !stop_pending_ && !wifi_return_pending_ && !wifi_return_incomplete_; }
    bool wants_engine() const { return !closing_ && !suspended_ && !awake_recovery_ &&
        !stop_pending_ && driver_change_.empty() && !wifi_return_pending_ && !wifi_return_incomplete_ && unlocked_ && wants_radio(); }
};

int Tray::run(bool background)
{
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    for (const auto& [name, proc] : {
             std::pair{window_class, &dispatch<&Tray::on_tray>},
             std::pair{scan_class, &dispatch<&Tray::on_scan>}}) {
        WNDCLASSEXW klass{sizeof(klass)};
        klass.hInstance = instance_;
        klass.lpszClassName = name;
        klass.lpfnWndProc = proc;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
        klass.hbrBackground = GetSysColorBrush(name == scan_class ? COLOR_BTNFACE : COLOR_WINDOW);
        if (!RegisterClassExW(&klass)) return 1;
    }
    window_ = CreateWindowExW(0, window_class, L"Airdraft", WS_OVERLAPPED,
        0, 0, 0, 0, nullptr, nullptr, instance_, this);
    if (!window_) return 1;
    WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION);
    suspend_notifications_ = RegisterSuspendResumeNotification(window_, DEVICE_NOTIFY_WINDOW_HANDLE);
    SetTimer(window_, 1, 2000, nullptr);
    mailbox_ = std::make_shared<Mailbox>(window_);
    engine_ = std::make_unique<Engine>(*mailbox_);
    service_ = std::make_unique<Service>(*mailbox_);
    add_icon();
    tick();
    if (!background) open_scan();
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (scan_ && IsDialogMessageW(scan_, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    shutdown();
    return static_cast<int>(msg.wParam);
}

void Tray::add_icon()
{
    icon_ = {};
    icon_.cbSize = sizeof(icon_);
    icon_.hWnd = window_;
    icon_.uID = 1;
    icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon_.uCallbackMessage = tray_message;
    icon_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
    wcscpy_s(icon_.szTip, L"Airdraft");
    if (Shell_NotifyIconW(NIM_ADD, &icon_)) {
        icon_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &icon_);
    }
}

void Tray::menu(POINT position)
{
    const auto menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, scan_devices, L"&Open Airdraft");
    SetMenuDefaultItem(menu, scan_devices, FALSE);
    AppendMenuW(menu, MF_STRING, send_files, active_send_.empty() ? L"&Choose files…" : L"&View transfer");
    AppendMenuW(menu, MF_STRING | (enabled_ ? MF_CHECKED : MF_UNCHECKED), receive_files, L"&Receive files");
    AppendMenuW(menu, MF_STRING, open_downloads, L"Received &files");
    if (can_toggle_offline())
        AppendMenuW(menu, MF_STRING, offline_radio, offline_session_ ? L"Stop AirDrop" : L"Start AirDrop…");
    if (can_toggle_experimental())
        AppendMenuW(menu, MF_STRING, experimental_radio,
            experimental_session_ ? L"Stop AirDrop with backup connection" : L"Start AirDrop with backup connection");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, show_settings, L"&Settings…");
    AppendMenuW(menu, MF_STRING, show_status, L"Connection &details…");
    AppendMenuW(menu, MF_STRING, about, L"About Airdraft");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, exit_app, L"&Quit Airdraft");
    SetForegroundWindow(window_);
    const auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        position.x, position.y, 0, window_, nullptr);
    DestroyMenu(menu);
    PostMessageW(window_, WM_NULL, 0, 0);
    if (choice) act(static_cast<int>(choice));
}

void Tray::act(int id)
{
    switch (id) {
    case send_files:
        open_scan();
        if (scan_ && active_send_.empty()) pick();
        break;
    case scan_devices: open_scan(); break;
    case receive_files: {
        const DWORD next = enabled_ ? 0 : 1;
        if (RegSetKeyValueW(HKEY_CURRENT_USER, prefs, L"Receiving", REG_DWORD, &next, sizeof(next)) != ERROR_SUCCESS) {
            MessageBoxW(window_, L"Could not save the receiving preference.", L"Airdraft", MB_OK | MB_ICONERROR);
            break;
        }
        enabled_ = next != 0;
        change_intent();
        break;
    }
    case open_downloads: {
        std::error_code error;
        fs::create_directories(preferences_.folder, error);
        if (error || reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", preferences_.folder.c_str(),
                nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            MessageBoxW(scan_ ? scan_ : window_, L"Could not open the incoming files folder. Check the path in Settings.",
                        L"Airdraft", MB_OK | MB_ICONWARNING);
        break;
    }
    case show_settings:
        open_settings();
        break;
    case show_status: show_status_dialog(); break;
    case offline_radio: toggle_offline(window_); break;
    case experimental_radio:
        if (!can_toggle_experimental()) break;
        if (experimental_session_) {
            experimental_session_ = false;
        } else if (service_->send(command(L"start-experimental-radio"))) {
            experimental_session_ = true;
            wake_sent_ = true;
            error_.clear();
            detail_.clear();
            retry_ = 0;
            attempts_ = 0;
        }
        tick();
        break;
    case about: {
        const TASKDIALOG_BUTTON buttons[] = {{100, L"Credits"}, {IDCANCEL, L"Close"}};
        TASKDIALOGCONFIG config{sizeof(config)};
        config.hwndParent = window_;
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        config.pszWindowTitle = L"About Airdraft";
        config.pszMainInstruction = L"Airdraft";
        config.pszContent = L"0.1.6 Developer Preview";
        config.pButtons = buttons;
        config.cButtons = ARRAYSIZE(buttons);
        config.nDefaultButton = IDCANCEL;
        int button = 0;
        if (SUCCEEDED(TaskDialogIndirect(&config, &button, nullptr, nullptr)) && button == 100)
            ShellExecuteW(window_, L"open", L"notepad.exe", (L"\"" + (root_ / L"CREDITS.md").wstring() + L"\"").c_str(),
                          nullptr, SW_SHOWNORMAL);
        break;
    }
    case exit_app: DestroyWindow(window_); break;
    }
}

void Tray::toggle_offline(HWND owner)
{
    if (!can_toggle_offline()) return;
    if (offline_session_) {
        stop_sent_ = service_->send(command(L"stop"));
        if (stop_sent_) stop_engine();
        return;
    }
    if (MessageBoxW(owner, L"AirDrop will use the Wi-Fi adapter. Wi-Fi will disconnect until you stop the session. Start now?",
                    L"Start AirDrop session", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES &&
        can_toggle_offline() && !offline_session_ && service_->send(command(L"start-offline-radio")))
        wake_sent_ = true;
}

void Tray::session_action(HWND owner)
{
    if (can_toggle_offline() && can_toggle_experimental()) {
        const auto options = CreatePopupMenu();
        if (!options) return;
        AppendMenuW(options, MF_STRING, experimental_radio, L"Use a backup connection");
        AppendMenuW(options, MF_STRING, offline_radio, L"Temporarily use the Wi-Fi adapter…");
        RECT button{};
        GetWindowRect(scan_session_, &button);
        const auto action = TrackPopupMenu(options, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN,
                                          button.right, button.bottom, 0, owner, nullptr);
        DestroyMenu(options);
        if (action == offline_radio) toggle_offline(owner);
        if (action == experimental_radio) act(experimental_radio);
    } else if (can_toggle_offline()) {
        toggle_offline(owner);
    } else if (can_toggle_experimental()) {
        act(experimental_radio);
    }
}

void Tray::update_status()
{
    const bool issue = !leased_ || radio_ == L"error" || wifi_return_incomplete_ ||
        !radio_mode_available() || !error_.empty() || !bluetooth_.empty();
    const bool recovering = suspended_ || awake_recovery_ || stop_pending_ || !driver_change_.empty() ||
        wifi_handoff_pending_ || wifi_return_pending_;
    state_ = receive_state(enabled_, unlocked_, recovering, leased_ && radio_ == L"wifiprotected", issue,
        listening_ && radio_ == L"on" && engine_ && engine_->running());
    if (scan_receive_) SendMessageW(scan_receive_, BM_SETCHECK, enabled_ ? BST_CHECKED : BST_UNCHECKED, 0);
    if (scan_receive_status_)
        set_label(scan_receive_status_, !enabled_ ? L"Receiving off" : state_ == L"Ready" ? L"Ready to receive"
            : state_ == L"Locked" ? L"Unlock to receive" : L"Waiting to receive");
    icon_.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(icon_.szTip, (L"Airdraft · " + state_).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &icon_);
}

void Tray::failure(std::wstring reason)
{
    error_ = std::move(reason);
    listening_ = false;
    update_status();
}

void Tray::show_status_dialog()
{
    std::wstring text = enabled_ ? L"Receiving enabled\n" : L"Receiving paused\n";
    text += state_ + L"\n\n";
    if (!leased_) text += service_detail_.empty() ? initial_service_status() : service_detail_;
    else if (driver_change_ == L"taking") text += L"Waiting for Windows to switch the Wi-Fi adapter to Airdraft.";
    else if (driver_change_ == L"restoring") text += L"Waiting for Windows to restore the Wi-Fi driver.";
    else if (stop_pending_) text += L"Airdraft is releasing the radio.";
    else if (wifi_return_pending_) text += L"Waiting for the Windows Wi-Fi adapter to reconnect.";
    else if (wifi_return_incomplete_)
        text += L"Windows Wi-Fi has not fully reconnected. Check your connection or run Airdraft - restore Windows Wi-Fi.";
    else if (radio_ == L"wifiprotected") text += detail_;
    else if (!radio_mode_available()) text += L"The radio service is configured in an unavailable mode.";
    else if (!error_.empty()) text += error_;
    else if (radio_ == L"error" && !detail_.empty()) text += detail_;
    else if (offline_session_) text += L"AirDrop is using the Wi-Fi adapter. Stop the session to return it to Windows.";
    else if (experimental_radio_ && !experimental_session_)
        text += L"Start the experimental RTL8852BE radio from the tray. It needs another physical network adapter while it uses this card.";
    else if (!detail_.empty()) text += detail_;
    else if (wifi_handoff_pending_) text += L"Checking that Windows has its previous network paths through the Airdraft adapter.";
    else if (radio_ == L"on" && !engine_index_) text += L"AWDL interface is coming up.";
    if (!bluetooth_.empty()) text += L"\n\n" + bluetooth_;
    if (!card_.empty()) text += L"\n\nAdapter: " + card_;
    if (!wifi_.empty()) text += L"\nWi-Fi: " + wifi_;
    MessageBoxW(scan_ ? scan_ : window_, text.c_str(), L"Airdraft status", MB_OK | MB_ICONINFORMATION);
}

void Tray::open_settings()
{
    if (review_window_) {
        SetForegroundWindow(review_window_);
        return;
    }
    if (settings_window_) {
        SetForegroundWindow(settings_window_);
        return;
    }
    if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_PREFERENCES), scan_ ? scan_ : window_,
                        settings_dialog, reinterpret_cast<LPARAM>(this)) == -1)
        MessageBoxW(scan_ ? scan_ : window_, L"Could not open Airdraft Settings.", L"Airdraft", MB_OK | MB_ICONERROR);
}

INT_PTR CALLBACK Tray::settings_dialog(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC)
        return ui::dialog_surface(reinterpret_cast<HDC>(wparam));
    const auto populate = [&](const Preferences& options) {
        SetDlgItemTextW(dialog, IDC_DEVICE_NAME, options.name.c_str());
        SetDlgItemTextW(dialog, IDC_DOWNLOAD_FOLDER, options.folder.c_str());
        CheckDlgButton(dialog, IDC_SHOW_PENDING, options.show_pending ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_TRANSFER_NOTICES, options.notifications ? BST_CHECKED : BST_UNCHECKED);
    };
    auto* tray = reinterpret_cast<Tray*>(GetWindowLongPtrW(dialog, DWLP_USER));
    try {
        if (message == WM_INITDIALOG) {
            tray = reinterpret_cast<Tray*>(lparam);
            SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            tray->settings_window_ = dialog;
            SendDlgItemMessageW(dialog, IDC_DEVICE_NAME, EM_SETLIMITTEXT, max_device_name, 0);
            populate(tray->preferences_);
            return TRUE;
        }
        if (!tray) return FALSE;
        if (message == WM_DESTROY) {
            tray->settings_window_ = nullptr;
            return FALSE;
        }
        if (message != WM_COMMAND) return FALSE;
        switch (LOWORD(wparam)) {
        case IDC_BROWSE_FOLDER: {
            winrt::com_ptr<IFileOpenDialog> picker;
            winrt::check_hresult(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                                 IID_PPV_ARGS(picker.put())));
            winrt::check_hresult(picker->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST));
            winrt::check_hresult(picker->SetTitle(L"Choose where to save incoming files"));
            winrt::com_ptr<IShellItem> current;
            if (SUCCEEDED(SHCreateItemFromParsingName(edit_text(dialog, IDC_DOWNLOAD_FOLDER).c_str(), nullptr,
                                                      IID_PPV_ARGS(current.put()))))
                picker->SetFolder(current.get());
            const auto shown = picker->Show(dialog);
            if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return TRUE;
            winrt::check_hresult(shown);
            winrt::com_ptr<IShellItem> selected;
            winrt::check_hresult(picker->GetResult(selected.put()));
            PWSTR path = nullptr;
            winrt::check_hresult(selected->GetDisplayName(SIGDN_FILESYSPATH, &path));
            std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(path, CoTaskMemFree);
            SetDlgItemTextW(dialog, IDC_DOWNLOAD_FOLDER, path);
            return TRUE;
        }
        case IDC_RESTORE_DEFAULTS:
            populate(default_preferences());
            return TRUE;
        case IDOK:
            tray->apply_preferences(dialog);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
    } catch (const winrt::hresult_error& error) {
        MessageBoxW(dialog, error.message().c_str(), L"Airdraft Settings", MB_OK | MB_ICONERROR);
    } catch (const std::exception& error) {
        MessageBoxW(dialog, winrt::to_hstring(error.what()).c_str(), L"Airdraft Settings", MB_OK | MB_ICONERROR);
    }
    return FALSE;
}

void Tray::apply_preferences(HWND dialog)
{
    auto name = edit_text(dialog, IDC_DEVICE_NAME);
    const auto first = name.find_first_not_of(L" ");
    if (first != std::wstring::npos) name = name.substr(first, name.find_last_not_of(L" ") - first + 1);
    else name.clear();
    if (!valid_device_name(name)) {
        MessageBoxW(dialog, L"Enter a device name of up to 63 characters. Leave out control characters and \\ / : * ? \" < > |.",
                    L"Airdraft Settings", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, IDC_DEVICE_NAME));
        return;
    }
    Preferences next{std::move(name), edit_text(dialog, IDC_DOWNLOAD_FOLDER),
                     IsDlgButtonChecked(dialog, IDC_SHOW_PENDING) == BST_CHECKED,
                     IsDlgButtonChecked(dialog, IDC_TRANSFER_NOTICES) == BST_CHECKED};
    std::error_code folder_error;
    if (next.folder.is_absolute()) fs::create_directories(next.folder, folder_error);
    if (!next.folder.is_absolute() || folder_error) {
        MessageBoxW(dialog, L"Choose an available folder for incoming files.", L"Airdraft Settings", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!save_preferences(next)) {
        MessageBoxW(dialog, L"Could not save your Airdraft preferences.", L"Airdraft Settings", MB_OK | MB_ICONERROR);
        return;
    }
    if (engine_->running() && (next.name != preferences_.name || next.folder != preferences_.folder)) {
        auto update = command(L"set");
        update.Insert(L"name", JsonValue::CreateStringValue(next.name));
        update.Insert(L"folder", JsonValue::CreateStringValue(next.folder.native()));
        if (!engine_->send(update))
            MessageBoxW(dialog, L"Saved. The name and folder will be applied when the transfer engine restarts.",
                        L"Airdraft Settings", MB_OK | MB_ICONINFORMATION);
    }
    preferences_ = std::move(next);
    refresh_scan_peers();
    update_discovery_hint();
    EndDialog(dialog, IDOK);
}

void Tray::balloon(std::wstring_view text)
{
    if (!preferences_.notifications) return;
    icon_.uFlags = NIF_INFO;
    wcscpy_s(icon_.szInfoTitle, L"Airdraft");
    wcsncpy_s(icon_.szInfo, std::wstring(text).c_str(), _TRUNCATE);
    icon_.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &icon_);
}

void Tray::open_scan()
{
    if (const auto dialog = review_window_ ? review_window_ : settings_window_) {
        SetForegroundWindow(dialog);
        return;
    }
    const bool opening = !scan_;
    if (opening) {
        scan_ = CreateWindowExW(WS_EX_CONTROLPARENT, scan_class, L"Nearby AirDrop devices", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 920, 640, nullptr, nullptr, instance_, this);
        if (!scan_) return;
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(scan_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const auto& room = monitor.rcWork;
            const int width = std::min<int>(ui::pixels(scan_dpi_, 920), room.right - room.left);
            const int height = std::min<int>(ui::pixels(scan_dpi_, 640), room.bottom - room.top);
            SetWindowPos(scan_, nullptr, (room.left + room.right - width) / 2,
                (room.top + room.bottom - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        layout_scan(scan_);
        refresh_scan_peers();
        tick();
    }
    if (IsIconic(scan_)) ShowWindow(scan_, SW_RESTORE);
    else if (!IsWindowVisible(scan_)) ShowWindow(scan_, SW_SHOWNORMAL);
    // A hidden tray launch can override the process's first ShowWindow call.
    if (!IsWindowVisible(scan_))
        SetWindowPos(scan_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(scan_);
    if (opening) SetFocus(scan_rows_.empty() ? scan_send_ : scan_peers_);
}

void Tray::configure_scan(HWND window, UINT dpi)
{
    scan_dpi_ = dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
    scan_colors_ = ui::palette();
    const auto old_heading = scan_heading_font_, old_section = scan_section_font_, old_body = scan_body_font_;
    scan_heading_font_ = ui::font(scan_dpi_, 32, FW_SEMIBOLD);
    scan_section_font_ = ui::font(scan_dpi_, 18, FW_SEMIBOLD);
    scan_body_font_ = ui::font(scan_dpi_, 15);
    for (auto control = GetWindow(window, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT)) {
        auto font = scan_body_font_;
        if (control == scan_heading_) font = scan_heading_font_;
        else if (control == scan_devices_heading_ || control == scan_transfer_heading_ || control == scan_status_heading_)
            font = scan_section_font_;
        SendMessageW(control, WM_SETFONT, font ? reinterpret_cast<WPARAM>(font)
            : reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
    }
    if (old_heading) DeleteObject(old_heading);
    if (old_section) DeleteObject(old_section);
    if (old_body) DeleteObject(old_body);
    ListView_SetBkColor(scan_peers_, scan_colors_.surface);
    ListView_SetTextColor(scan_peers_, scan_colors_.text);
    ListView_SetTextBkColor(scan_peers_, CLR_NONE);
    const auto icon_size = ui::pixels(scan_dpi_, 32);
    if (const auto icons = ImageList_Create(icon_size, icon_size, ILC_COLOR32 | ILC_MASK, 3, 0)) {
        const auto add_icon = [&](SHSTOCKICONID stock) {
            SHSTOCKICONINFO icon{sizeof(icon)};
            if (FAILED(SHGetStockIconInfo(stock, SHGSI_ICON | SHGSI_LARGEICON, &icon))) return I_IMAGENONE;
            const int index = ImageList_AddIcon(icons, icon.hIcon);
            DestroyIcon(icon.hIcon);
            return index < 0 ? I_IMAGENONE : index;
        };
        scan_default_icon_ = add_icon(SIID_MYNETWORK);
        scan_phone_icon_ = add_icon(SIID_DEVICECELLPHONE);
        scan_computer_icon_ = add_icon(SIID_DESKTOPPC);
        ListView_SetImageList(scan_peers_, icons, LVSIL_NORMAL);
        if (scan_icons_) ImageList_Destroy(scan_icons_);
        scan_icons_ = icons;
        refresh_scan_peers();
    }
    ListView_SetIconSpacing(scan_peers_, ui::pixels(scan_dpi_, 124), ui::pixels(scan_dpi_, 94));
    SendMessageW(progress_, PBM_SETBKCOLOR, 0, scan_colors_.outline);
    SendMessageW(progress_, PBM_SETBARCOLOR, 0, scan_colors_.accent);
    for (auto control = GetWindow(window, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT))
        InvalidateRect(control, nullptr, FALSE);
}

void Tray::layout_scan(HWND window)
{
    RECT client{};
    if (!GetClientRect(window, &client)) return;
    const auto scale = [&](int size) { return ui::pixels(scan_dpi_, size); };
    const auto place = [&](HWND control, int x, int y, int width, int height) {
        MoveWindow(control, x, y, std::max(0, width), std::max(0, height), FALSE);
    };
    const int width = client.right, height = client.bottom;
    const int margin = scale(22), gap = scale(14), pad = scale(18);
    const int end = width - margin, body_top = scale(182), body_end = height - margin;
    const int transfer = end - std::clamp(width / 3, scale(236), scale(282)), devices_end = transfer - gap;
    scan_band_ = {margin, scale(100), end, scale(172)};
    scan_devices_panel_ = {margin, body_top, devices_end, body_end};
    scan_transfer_panel_ = {transfer, body_top, end, body_end};
    scan_queue_panel_ = {transfer + pad, body_top + scale(50), end - pad, body_top + scale(126)};

    place(scan_heading_, margin + scale(57), scale(13), end - margin - scale(426), scale(41));
    place(scan_identity_, margin + scale(58), scale(54), end - margin - scale(425), scale(22));
    place(scan_settings_, end - scale(94), scale(26), scale(94), scale(34));
    place(scan_details_, end - scale(227), scale(26), scale(124), scale(34));
    place(scan_downloads_, end - scale(357), scale(26), scale(121), scale(34));

    place(scan_status_heading_, margin + pad, scale(109), end - margin - scale(218), scale(24));
    place(scan_status_, margin + pad, scale(134), end - margin - scale(216), scale(35));
    place(scan_session_, end - scale(182), scale(114), scale(164), scale(36));

    const int left_inner = margin + pad, left_width = devices_end - margin - 2 * pad;
    place(scan_devices_heading_, left_inner, body_top + scale(19), left_width - scale(125), scale(25));
    place(scan_result_, devices_end - pad - scale(123), body_top + scale(22), scale(123), scale(21));
    place(scan_peers_, left_inner, body_top + scale(55), left_width, body_end - body_top - scale(119));
    place(scan_empty_, left_inner, (body_top + body_end) / 2 + scale(7), left_width, scale(24));
    place(scan_receive_, left_inner, body_end - scale(48), scale(140), scale(29));
    place(scan_receive_status_, left_inner + scale(145), body_end - scale(43), left_width - scale(145), scale(23));

    const int right_inner = transfer + pad, right_width = end - transfer - 2 * pad;
    place(scan_transfer_heading_, right_inner, body_top + scale(19), right_width, scale(25));
    place(scan_queue_, right_inner + scale(11), body_top + scale(58), right_width - scale(22), scale(21));
    place(message_, right_inner + scale(11), body_top + scale(82), right_width - scale(22), scale(33));
    place(progress_, right_inner + scale(11), body_top + scale(118), right_width - scale(22), scale(5));
    if (body_end - body_top < scale(290)) {
        const int action_width = (right_width - scale(8)) / 2;
        place(scan_send_, right_inner, body_top + scale(138), action_width, scale(35));
        place(review_button_, end - pad - action_width, body_top + scale(138), action_width, scale(35));
    } else {
        place(scan_send_, right_inner, body_top + scale(141), right_width, scale(35));
        place(review_button_, right_inner, body_top + scale(184), right_width, scale(35));
    }
    place(cancel_button_, right_inner, body_end - scale(54), right_width - scale(108), scale(36));
    place(send_button_, end - pad - scale(98), body_end - scale(54), scale(98), scale(36));
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

COLORREF Tray::status_surface() const
{
    if (radio_ == L"wifiprotected" || radio_ == L"error" || wifi_return_incomplete_)
        return scan_colors_.caution;
    return link_ready() ? scan_colors_.status : scan_colors_.surface;
}

void Tray::paint_scan(HWND window, HDC dc) const
{
    RECT client{};
    GetClientRect(window, &client);
    const auto size = [&](int pixels) { return ui::pixels(scan_dpi_, pixels); };
    const auto radius = scan_colors_.contrast ? 0 : size(14);
    ui::fill(dc, client, scan_colors_.background);
    ui::fill(dc, {0, 0, client.right, size(86)}, scan_colors_.header);
    ui::fill(dc, {0, size(84), client.right, size(86)}, scan_colors_.accent);
    ui::panel(dc, {size(22), size(20), size(68), size(66)}, radius, scan_colors_.header_button, scan_colors_.header_outline);
    if (const auto icon = LoadIconW(instance_, MAKEINTRESOURCEW(1)))
        DrawIconEx(dc, size(29), size(27), icon, size(32), size(32), 0, nullptr, DI_NORMAL);
    ui::panel(dc, scan_band_, radius, status_surface(), scan_colors_.outline);
    ui::panel(dc, scan_devices_panel_, radius, scan_colors_.surface, scan_colors_.outline);
    ui::panel(dc, scan_transfer_panel_, radius, scan_colors_.surface, scan_colors_.outline);
    ui::panel(dc, scan_queue_panel_, radius, scan_colors_.inset, scan_colors_.outline);
    const int inset = size(18), bottom = scan_devices_panel_.bottom - size(58);
    ui::fill(dc, {scan_devices_panel_.left + inset, bottom, scan_devices_panel_.right - inset, bottom + 1},
             scan_colors_.outline);
    if (scan_rows_.empty() && scan_icons_ && scan_default_icon_ >= 0) {
        const int center = (scan_devices_panel_.left + scan_devices_panel_.right) / 2;
        const int middle = (scan_devices_panel_.top + scan_devices_panel_.bottom) / 2;
        ui::panel(dc, {center - size(25), middle - size(53), center + size(25), middle - size(3)},
                  radius, scan_colors_.inset, scan_colors_.outline);
        ImageList_Draw(scan_icons_, scan_default_icon_, dc, center - size(16), middle - size(44), ILD_TRANSPARENT);
    }
}

RECT Tray::recipient_tile(RECT icon) const
{
    const auto size = [&](int pixels) { return ui::pixels(scan_dpi_, pixels); };
    const int center = (icon.left + icon.right) / 2;
    const auto top = icon.top >= 0 ? std::max<LONG>(0, icon.top - size(4)) : icon.top - size(4);
    return {center - size(55), top, center + size(55), icon.bottom + size(33)};
}

int Tray::scan_item_at(POINT point) const
{
    RECT client{};
    if (!GetClientRect(scan_peers_, &client) || !PtInRect(&client, point)) return -1;
    LVHITTESTINFO hit{};
    hit.pt = point;
    const int item = ListView_HitTest(scan_peers_, &hit);
    if (item >= 0 || scan_colors_.contrast) return item;
    for (size_t row = 0; row < scan_rows_.size(); ++row) {
        RECT icon{};
        if (!ListView_GetItemRect(scan_peers_, static_cast<int>(row), &icon, LVIR_ICON)) continue;
        const auto tile = recipient_tile(icon);
        if (PtInRect(&tile, point)) return static_cast<int>(row);
    }
    return -1;
}

LRESULT Tray::draw_recipient(NMLVCUSTOMDRAW& item) const
{
    if (scan_colors_.contrast) return CDRF_DODEFAULT;
    if (item.nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (item.nmcd.dwDrawStage != CDDS_ITEMPREPAINT || item.nmcd.dwItemSpec >= scan_rows_.size())
        return CDRF_DODEFAULT;
    const auto index = static_cast<int>(item.nmcd.dwItemSpec);
    RECT icon{};
    if (!ListView_GetItemRect(scan_peers_, index, &icon, LVIR_ICON)) return CDRF_DODEFAULT;
    const auto size = [&](int pixels) { return ui::pixels(scan_dpi_, pixels); };
    const auto& row = scan_rows_[index];
    const bool selected = (ListView_GetItemState(scan_peers_, index, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    auto tile = recipient_tile(icon);
    const auto dc = item.nmcd.hdc;
    const int restore = SaveDC(dc);
    ui::panel(dc, tile, size(11), selected ? scan_colors_.status : scan_colors_.inset,
              selected ? scan_colors_.accent : scan_colors_.outline);
    if (scan_icons_ && row.icon >= 0)
        ImageList_Draw(scan_icons_, row.icon, dc, (tile.left + tile.right) / 2 - size(16), tile.top + size(5),
                       ILD_TRANSPARENT);
    if (scan_body_font_) SelectObject(dc, scan_body_font_);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, devices_.contains(row.id) ? scan_colors_.text : scan_colors_.secondary);
    RECT text{tile.left + size(5), icon.bottom + size(6), tile.right - size(5), tile.bottom - size(4)};
    DrawTextW(dc, row.label.c_str(), static_cast<int>(row.label.size()), &text,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (selected && (item.nmcd.uItemState & CDIS_FOCUS)) {
        InflateRect(&tile, -size(3), -size(3));
        DrawFocusRect(dc, &tile);
    }
    RestoreDC(dc, restore);
    return CDRF_SKIPDEFAULT;
}

LRESULT Tray::on_scan(HWND window, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CREATE: {
        const auto control = [&](LPCWSTR kind, LPCWSTR text, DWORD style, int id = 0) {
            return CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        };
        scan_heading_ = control(L"STATIC", L"Airdraft", SS_LEFT);
        scan_identity_ = control(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS);
        scan_receive_ = control(L"BUTTON", L"&Receive files", WS_TABSTOP | BS_AUTOCHECKBOX, receive_files);
        scan_receive_status_ = control(L"STATIC", L"", SS_RIGHT | SS_NOPREFIX);
        scan_downloads_ = control(L"BUTTON", L"Received &files", WS_TABSTOP | BS_OWNERDRAW, open_downloads);
        scan_details_ = control(L"BUTTON", L"Connection", WS_TABSTOP | BS_OWNERDRAW, show_status);
        scan_settings_ = control(L"BUTTON", L"Se&ttings", WS_TABSTOP | BS_OWNERDRAW, show_settings);
        scan_status_heading_ = control(L"STATIC", L"", SS_NOPREFIX);
        scan_status_ = control(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, scan_status_label);
        scan_session_ = control(L"BUTTON", L"Start Air&Drop…", WS_TABSTOP | BS_OWNERDRAW, offline_radio);
        scan_devices_heading_ = control(L"STATIC", L"Nearby devices", SS_LEFT);
        scan_result_ = control(L"STATIC", L"", SS_RIGHT | SS_NOPREFIX);
        scan_peers_ = control(WC_LISTVIEWW, nullptr,
            WS_TABSTOP | LVS_ICON | LVS_AUTOARRANGE | LVS_SINGLESEL | LVS_SHAREIMAGELISTS | LVS_SHOWSELALWAYS);
        scan_empty_ = control(L"STATIC", L"No nearby devices yet", SS_CENTER | SS_NOPREFIX);
        scan_transfer_heading_ = control(L"STATIC", L"Send files", SS_LEFT);
        scan_queue_ = control(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS);
        message_ = control(L"STATIC", L"", SS_NOPREFIX);
        progress_ = control(PROGRESS_CLASSW, L"", PBS_SMOOTH);
        scan_send_ = control(L"BUTTON", L"&Choose files…", WS_TABSTOP | BS_OWNERDRAW, send_files);
        review_button_ = control(L"BUTTON", L"Re&view files…", WS_TABSTOP | WS_DISABLED | BS_OWNERDRAW, review_files);
        cancel_button_ = control(L"BUTTON", L"C&lear files", WS_TABSTOP | WS_DISABLED | BS_OWNERDRAW, cancel_send);
        send_button_ = control(L"BUTTON", L"&Send", WS_TABSTOP | WS_DISABLED | BS_OWNERDRAW, send_now);
        if (!scan_heading_ || !scan_receive_ || !scan_send_ || !scan_downloads_ || !scan_details_ ||
            !scan_settings_ || !scan_peers_ || !scan_status_ || !scan_session_ || !scan_identity_ || !scan_result_ || !scan_receive_status_ ||
            !scan_queue_ || !message_ || !progress_ || !review_button_ || !cancel_button_ || !send_button_ ||
            !scan_status_heading_ || !scan_devices_heading_ || !scan_transfer_heading_ || !scan_empty_) return -1;
        for (const auto button : {scan_downloads_, scan_details_, scan_settings_, scan_session_,
                                  scan_send_, review_button_, cancel_button_, send_button_})
            ui::enable_button_hover(button);
        ListView_SetExtendedListViewStyle(scan_peers_, LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
        configure_scan(window, GetDpiForWindow(window));
        layout_scan(window);
        DragAcceptFiles(window, TRUE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto& info = *reinterpret_cast<MINMAXINFO*>(lparam);
        info.ptMinTrackSize = {ui::pixels(scan_dpi_, 760), ui::pixels(scan_dpi_, 535)};
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
            info.ptMinTrackSize.x = std::min(info.ptMinTrackSize.x, monitor.rcWork.right - monitor.rcWork.left);
            info.ptMinTrackSize.y = std::min(info.ptMinTrackSize.y, monitor.rcWork.bottom - monitor.rcWork.top);
        }
        return 0;
    }
    case WM_SIZE: {
        if (wparam != SIZE_MINIMIZED && scan_peers_) layout_scan(window);
        return 0;
    }
    case WM_DPICHANGED: {
        const auto frame = *reinterpret_cast<const RECT*>(lparam);
        configure_scan(window, LOWORD(wparam));
        SetWindowPos(window, nullptr, frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        layout_scan(window);
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        if (scan_peers_) {
            configure_scan(window, GetDpiForWindow(window));
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        paint_scan(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        paint_scan(window, reinterpret_cast<HDC>(wparam));
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        const auto dc = reinterpret_cast<HDC>(wparam);
        const auto child = reinterpret_cast<HWND>(lparam);
        auto color = scan_colors_.surface;
        auto text = scan_colors_.text;
        if (child == scan_heading_ || child == scan_identity_) {
            color = scan_colors_.header;
            text = child == scan_identity_ ? scan_colors_.header_secondary : scan_colors_.header_text;
        } else if (child == scan_status_heading_ || child == scan_status_) {
            color = status_surface();
            if (child == scan_status_) text = scan_colors_.secondary;
        } else if (child == scan_queue_ || child == message_) {
            color = scan_colors_.inset;
            if (child == message_) text = scan_colors_.secondary;
        } else if (child == scan_result_ || child == scan_empty_ || child == scan_receive_status_) {
            text = scan_colors_.secondary;
        }
        SetBkColor(dc, color);
        SetTextColor(dc, text);
        SetDCBrushColor(dc, color);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (item.CtlType != ODT_BUTTON) return FALSE;
        const bool header = item.CtlID == open_downloads || item.CtlID == show_status || item.CtlID == show_settings;
        const bool primary = item.CtlID == send_now ||
            (item.CtlID == offline_radio && !offline_session_ && !experimental_session_);
        auto colors = scan_colors_;
        if (item.CtlID == offline_radio) colors.surface = status_surface();
        ui::draw_button(item, scan_dpi_, header ? ui::Button::header : primary ? ui::Button::primary : ui::Button::secondary, colors);
        return TRUE;
    }
    case WM_DROPFILES:
        drop_files(window, reinterpret_cast<HDROP>(wparam));
        return 0;
    case WM_COMMAND: {
        const auto id = LOWORD(wparam);
        if (id == IDCANCEL) DestroyWindow(window);
        else if (id == offline_radio) session_action(window);
        else if (id == send_files) pick();
        else if (id == send_now) send_selection();
        else if (id == review_files) review_queue();
        else if (id == cancel_send) {
            if (active_send_.empty()) {
                selected_files_.clear();
                transfer_message_.clear();
                transfer_percent_ = 0;
            } else if (!cancelling_send_) {
                auto cancel = command(L"cancel");
                cancel.Insert(L"id", JsonValue::CreateStringValue(active_send_));
                cancelling_send_ = engine_->send(cancel);
                if (!cancelling_send_) transfer_message_ = L"Could not request cancellation. Try again.";
            }
            update_send_controls();
        }
        else act(id);
        return 0;
    }
    case WM_NOTIFY: {
        const auto& event = *reinterpret_cast<const NMHDR*>(lparam);
        if (event.hwndFrom == scan_peers_ && event.code == NM_CUSTOMDRAW)
            return draw_recipient(*reinterpret_cast<NMLVCUSTOMDRAW*>(lparam));
        if (event.hwndFrom == scan_peers_ && event.code == LVN_GETINFOTIPW) {
            auto& tip = *reinterpret_cast<NMLVGETINFOTIPW*>(lparam);
            if (tip.pszText && tip.iItem >= 0 && static_cast<size_t>(tip.iItem) < scan_rows_.size() && tip.cchTextMax > 0)
                wcsncpy_s(tip.pszText, static_cast<size_t>(tip.cchTextMax), scan_rows_[tip.iItem].label.c_str(), _TRUNCATE);
        }
        if (event.hwndFrom == scan_peers_ && event.code == LVN_ITEMCHANGED &&
            (reinterpret_cast<const NMLISTVIEW*>(lparam)->uChanged & LVIF_STATE)) update_send_controls();
        if (event.hwndFrom == scan_peers_ && event.code == NM_CLICK) {
            const auto& click = *reinterpret_cast<const NMITEMACTIVATE*>(lparam);
            if (click.iItem < 0) {
                const auto index = scan_item_at(click.ptAction);
                if (index >= 0)
                    ListView_SetItemState(scan_peers_, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
        }
        if (event.hwndFrom == scan_peers_ && (event.code == NM_DBLCLK || event.code == NM_RETURN)) {
            const auto index = event.code == NM_DBLCLK ? scan_item_at(reinterpret_cast<const NMITEMACTIVATE*>(lparam)->ptAction)
                : ListView_GetNextItem(scan_peers_, -1, LVNI_SELECTED);
            auto recipient = scan_recipient(index);
            if (!recipient.empty() && active_send_.empty()) {
                if (selected_files_.empty()) pick(std::move(recipient));
                else if (IsWindowEnabled(send_button_)) SetFocus(send_button_);
            }
        }
        return 0;
    }
    case WM_DESTROY:
        if (scan_icons_) ImageList_Destroy(scan_icons_);
        if (scan_heading_font_) DeleteObject(scan_heading_font_);
        if (scan_section_font_) DeleteObject(scan_section_font_);
        if (scan_body_font_) DeleteObject(scan_body_font_);
        scan_icons_ = nullptr;
        scan_heading_font_ = scan_section_font_ = scan_body_font_ = nullptr;
        scan_heading_ = scan_receive_ = scan_send_ = scan_downloads_ = scan_details_ = nullptr;
        scan_settings_ = nullptr;
        scan_ = scan_peers_ = scan_status_ = scan_session_ = scan_identity_ = scan_result_ = scan_queue_ = nullptr;
        scan_receive_status_ = nullptr;
        scan_status_heading_ = scan_devices_heading_ = scan_transfer_heading_ = scan_empty_ = nullptr;
        send_button_ = cancel_button_ = review_button_ = message_ = progress_ = nullptr;
        scan_rows_.clear();
        shown_scan_status_.clear();
        tick();
        return 0;
    }
    return DefWindowProcW(window, msg, wparam, lparam);
}

void Tray::pick(std::wstring recipient)
{
    if (!scan_ || choosing_files_ || review_window_ || settings_window_ || !active_send_.empty()) return;
    auto dialog = winrt::create_instance<IFileOpenDialog>(CLSID_FileOpenDialog);
    FILEOPENDIALOGOPTIONS flags = 0;
    winrt::check_hresult(dialog->GetOptions(&flags));
    winrt::check_hresult(dialog->SetOptions(flags | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST));
    winrt::check_hresult(dialog->SetTitle(L"Choose files for Airdraft"));
    choosing_files_ = true;
    update_send_controls();
    const auto result = dialog->Show(scan_);
    choosing_files_ = false;
    update_send_controls();
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    winrt::check_hresult(result);
    winrt::com_ptr<IShellItemArray> items;
    winrt::check_hresult(dialog->GetResults(items.put()));
    DWORD count = 0;
    winrt::check_hresult(items->GetCount(&count));
    std::vector<std::wstring> paths;
    for (DWORD i = 0; i < count; ++i) {
        winrt::com_ptr<IShellItem> item;
        PWSTR path = nullptr;
        winrt::check_hresult(items->GetItemAt(i, item.put()));
        winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &path));
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(path, CoTaskMemFree);
        paths.emplace_back(path);
    }
    if (paths.empty()) return;
    queue_files(std::move(paths), recipient, !recipient.empty());
}

void Tray::drop_files(HWND window, HDROP drop)
{
    const std::unique_ptr<std::remove_pointer_t<HDROP>, decltype(&DragFinish)> files(drop, DragFinish);
    if (!files || choosing_files_ || review_window_ || settings_window_ || !active_send_.empty()) return;
    std::wstring recipient;
    POINT point{};
    if (DragQueryPoint(drop, &point)) {
        MapWindowPoints(window, scan_peers_, &point, 1);
        recipient = scan_recipient(scan_item_at(point));
    }
    std::vector<std::wstring> paths;
    const auto count = DragQueryFileW(drop, UINT_MAX, nullptr, 0);
    for (UINT item = 0; item < count; ++item) {
        std::wstring path(DragQueryFileW(drop, item, nullptr, 0) + 1, L'\0');
        path.resize(DragQueryFileW(drop, item, path.data(), static_cast<UINT>(path.size())));
        const auto attributes = GetFileAttributesW(path.c_str());
        if (path.empty() || attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            MessageBoxW(window, L"Drop individual files from File Explorer.", L"Airdraft", MB_OK | MB_ICONINFORMATION);
            return;
        }
        paths.push_back(std::move(path));
    }
    if (paths.empty()) return;
    queue_files(std::move(paths), recipient, true);
}

void Tray::queue_files(std::vector<std::wstring> files, std::wstring_view recipient, bool select_target)
{
    if (!scan_ || files.empty() || !active_send_.empty()) return;
    selected_files_ = std::move(files);
    transfer_message_.clear();
    transfer_percent_ = 0;
    if (select_target) {
        ListView_SetItemState(scan_peers_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        const auto row = std::ranges::find(scan_rows_, recipient, &ScanRow::id);
        if (row != scan_rows_.end() && devices_.contains(row->id)) {
            const auto index = static_cast<int>(row - scan_rows_.begin());
            ListView_SetItemState(scan_peers_, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(scan_peers_, index, FALSE);
        }
    }
    update_send_controls();
}

void Tray::review_queue()
{
    if (!scan_ || choosing_files_ || review_window_ || settings_window_ || selected_files_.empty() || !active_send_.empty()) return;
    if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_FILE_REVIEW), scan_, review_dialog,
                        reinterpret_cast<LPARAM>(this)) == -1)
        MessageBoxW(scan_, L"Could not open the file queue.", L"Airdraft", MB_OK | MB_ICONERROR);
}

INT_PTR CALLBACK Tray::review_dialog(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC)
        return ui::dialog_surface(reinterpret_cast<HDC>(wparam));
    auto* tray = reinterpret_cast<Tray*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        tray = reinterpret_cast<Tray*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        tray->review_window_ = dialog;
        const auto list = GetDlgItem(dialog, IDC_QUEUED_FILES);
        const auto dc = GetDC(list);
        const auto font = reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0));
        const auto old_font = dc && font ? SelectObject(dc, font) : nullptr;
        LONG width = 0;
        bool populated = true;
        for (const auto& path : tray->selected_files_) {
            if (SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str())) < 0) {
                populated = false;
                break;
            }
            SIZE size{};
            if (dc && GetTextExtentPoint32W(dc, path.c_str(), static_cast<int>(path.size()), &size))
                width = std::max(width, size.cx);
        }
        if (old_font) SelectObject(dc, old_font);
        if (dc) ReleaseDC(list, dc);
        if (!populated) EndDialog(dialog, -1);
        else SendMessageW(list, LB_SETHORIZONTALEXTENT, width + 10, 0);
        return TRUE;
    }
    if (!tray) return FALSE;
    if (message == WM_DESTROY) {
        tray->review_window_ = nullptr;
        return FALSE;
    }
    if (message != WM_COMMAND) return FALSE;
    if (LOWORD(wparam) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    const auto list = GetDlgItem(dialog, IDC_QUEUED_FILES);
    const auto remove = GetDlgItem(dialog, IDC_REMOVE_QUEUED);
    if (LOWORD(wparam) == IDC_QUEUED_FILES && HIWORD(wparam) == LBN_SELCHANGE) {
        EnableWindow(remove, SendMessageW(list, LB_GETSELCOUNT, 0, 0) > 0);
        return TRUE;
    }
    if (LOWORD(wparam) == IDC_REMOVE_QUEUED && tray->active_send_.empty()) {
        for (size_t index = tray->selected_files_.size(); index > 0;) {
            --index;
            if (SendMessageW(list, LB_GETSEL, index, 0) > 0 && SendMessageW(list, LB_DELETESTRING, index, 0) >= 0)
                tray->selected_files_.erase(tray->selected_files_.begin() + index);
        }
        tray->transfer_message_.clear();
        tray->transfer_percent_ = 0;
        tray->update_send_controls();
        EnableWindow(remove, FALSE);
        return TRUE;
    }
    return FALSE;
}

void Tray::send_selection()
{
    if (!scan_ || choosing_files_ || review_window_ || !link_ready() || selected_files_.empty() || !active_send_.empty()) return;
    const auto recipient = scan_recipient(ListView_GetNextItem(scan_peers_, -1, LVNI_SELECTED));
    if (recipient.empty()) return;
    const auto id = std::to_wstring(GetTickCount64());
    auto send = command(L"send");
    send.Insert(L"id", JsonValue::CreateStringValue(id));
    send.Insert(L"peer", JsonValue::CreateStringValue(recipient));
    JsonArray paths;
    for (const auto& path : selected_files_) paths.Append(JsonValue::CreateStringValue(path));
    send.Insert(L"files", paths);
    transfer_percent_ = 0;
    if (engine_->send(send)) {
        active_send_ = id;
        transfer_target_ = devices_.at(recipient).name;
        transfer_message_ = L"Waiting for the recipient to accept…";
        cancelling_send_ = false;
        service_->send(command(L"touch"));
    } else {
        transfer_message_ = L"Could not start the transfer. Try again.";
    }
    update_send_controls();
}

void Tray::update_send_controls()
{
    if (!scan_ || !scan_queue_) return;
    const auto recipient = scan_recipient(ListView_GetNextItem(scan_peers_, -1, LVNI_SELECTED));
    const bool active = !active_send_.empty(), has_files = !selected_files_.empty();
    const bool ready = link_ready();
    EnableWindow(scan_send_, !active && !choosing_files_);
    EnableWindow(review_button_, !active && !choosing_files_ && has_files);
    EnableWindow(send_button_, !active && !choosing_files_ && has_files && ready && !recipient.empty());
    EnableWindow(cancel_button_, !choosing_files_ && (active ? !cancelling_send_ : has_files));
    set_label(scan_send_, has_files ? L"&Change files…" : L"&Choose files…");
    set_label(cancel_button_, active ? L"C&ancel transfer" : L"C&lear files");

    std::wstring queue = L"No files selected";
    if (has_files)
        queue = selected_files_.size() == 1 ? fs::path(selected_files_[0]).filename().wstring()
            : std::to_wstring(selected_files_.size()) + L" files selected";
    if (active) queue = L"To " + transfer_target_;
    set_label(scan_queue_, queue);

    std::wstring instruction = transfer_message_;
    if (active && cancelling_send_) instruction = L"Cancelling transfer…";
    else if (instruction.empty()) {
        if (!has_files) instruction = L"Choose files or drop them here.";
        else if (!ready) instruction = L"Files are ready. Waiting for AirDrop.";
        else if (recipient.empty()) instruction = L"Select a nearby device to send these files.";
        else instruction = L"Send to " + devices_.at(recipient).name;
    }
    set_label(message_, instruction);
    SendMessageW(progress_, PBM_SETPOS, transfer_percent_, 0);
    set_visible(progress_, active);
}

void Tray::refresh_scan_peers()
{
    if (!scan_peers_) return;
    for (size_t index = scan_rows_.size(); index > 0;) {
        --index;
        const auto& id = scan_rows_[index].id;
        if (!devices_.contains(id) && !(preferences_.show_pending && discovery_candidates_.contains(id)) &&
            ListView_DeleteItem(scan_peers_, static_cast<int>(index)))
            scan_rows_.erase(scan_rows_.begin() + index);
    }
    const auto update = [&](const std::wstring& id, const std::wstring& label, int icon) {
        if (icon == I_IMAGENONE) icon = scan_default_icon_;
        const auto existing = std::ranges::find(scan_rows_, id, &ScanRow::id);
        if (existing != scan_rows_.end() && existing->label == label && existing->icon == icon) return;
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = static_cast<int>(existing - scan_rows_.begin());
        item.pszText = const_cast<wchar_t*>(label.c_str());
        item.iImage = icon;
        if (existing != scan_rows_.end()) {
            if (ListView_SetItem(scan_peers_, &item)) {
                existing->label = label;
                existing->icon = icon;
            }
        } else if (const auto inserted = ListView_InsertItem(scan_peers_, &item); inserted >= 0) {
            scan_rows_.insert(scan_rows_.begin() + inserted, {id, label, icon});
        }
    };
    for (const auto& [key, device] : devices_) {
        const int icon = (device.model.starts_with(L"iPhone") || device.model.starts_with(L"iPad")) ? scan_phone_icon_
            : device.model.starts_with(L"Mac") ? scan_computer_icon_ : scan_default_icon_;
        update(key, device.name, icon);
    }
    if (!preferences_.show_pending) return;
    for (const auto& [key, state] : discovery_candidates_) {
        update(key, state == L"restricted" ? L"Name hidden"
            : state == L"retrying" ? L"Reconnecting…" : L"Checking…", scan_default_icon_);
    }
}

std::wstring Tray::scan_recipient(int item) const
{
    if (item < 0 || static_cast<size_t>(item) >= scan_rows_.size()) return {};
    const auto& id = scan_rows_[static_cast<size_t>(item)].id;
    return devices_.contains(id) ? id : L"";
}

void Tray::events()
{
    for (const auto& event : mailbox_->drain()) {
        if (closing_) return;
        if (event.origin == Origin::service) on_service(event);
        else if (event.origin == Origin::service_status) service_detail_ = winrt::to_hstring(event.line);
        else if (event.origin == Origin::bluetooth) {
            bluetooth_ = winrt::to_hstring(event.line);
            update_status();
        } else if (event.origin == Origin::beacon) on_beacon(event);
        else if (event.generation == engine_->generation()) on_engine(event);
    }
}

void Tray::on_service(const Event& event)
{
    if (event.generation < service_generation_) return;
    if (event.line.empty()) {
        leased_ = false;
        experimental_session_ = false;
        offline_session_ = false;
        wake_sent_ = false;
        stop_sent_ = wifi_handoff_pending_ = false;
        stop_pending_ = false;
        driver_change_.clear();
        wifi_return_pending_ = wifi_return_incomplete_ = false;
        awdl_peer_count_ = 0;
        radio_.clear();
        stop_engine();
        return;
    }
    JsonObject object;
    if (!JsonObject::TryParse(winrt::to_hstring(event.line), object)) return;
    const auto type = string(object, L"event");
    if (type == L"desktop") {
        leased_ = object.GetNamedBoolean(L"granted", false);
        if (!leased_) service_detail_ = L"The Airdraft radio service is reserved for the active console session.";
    }
    if (type != L"state" && type != L"desktop") return;
    if (type == L"state") {
        if (service_generation_ != event.generation) {
            service_generation_ = event.generation;
            lease_retry_ = 0;
        }
        const auto next = string(object, L"radio");
        awdl_peer_count_ = static_cast<unsigned>(number(object, L"awdlPeerCount"));
        if (next != L"on" && radio_ == L"on") stop_engine();
        experimental_radio_ = object.GetNamedBoolean(L"experimentalRadio", false);
        offline_session_ = object.GetNamedBoolean(L"offlineSession", false);
        if (experimental_radio_ && (next == L"error" || (next == L"off" && (radio_ == L"on" || radio_ == L"starting")))) {
            experimental_session_ = false;
            wake_sent_ = false;
            if (next == L"error") stop_sent_ = awake_recovery_ = false;
        }
        radio_ = next;
        mode_ = string(object, L"mode");
        card_ = string(object, L"card");
        wifi_ = string(object, L"wifi");
        detail_ = string(object, L"detail");
        stop_pending_ = object.GetNamedBoolean(L"stopPending", false);
        driver_change_ = string(object, L"driverChange");
        wifi_handoff_pending_ = object.GetNamedBoolean(L"wifiHandoffPending", false);
        const bool returning = wifi_return_pending_ || wifi_return_incomplete_;
        wifi_return_pending_ = object.GetNamedBoolean(L"wifiReturnPending", false);
        wifi_return_incomplete_ = object.GetNamedBoolean(L"wifiReturnIncomplete", false);
        if (returning && !wifi_return_pending_ && !wifi_return_incomplete_) wake_sent_ = radio_ == L"error";
        if (radio_ == L"off") {
            awake_recovery_ = false;
            if (stop_sent_) wake_sent_ = stop_sent_ = false;
        }
    }
    tick();
}

void Tray::on_engine(const Event& event)
{
    if (event.line.empty()) {
        stop_engine();
        failure(L"Transfer engine exited. Airdraft will retry.");
        return;
    }
    JsonObject object;
    if (!JsonObject::TryParse(winrt::to_hstring(event.line), object)) return;
    const auto type = string(object, L"event"), id = string(object, L"id");
    if (type == L"receiving") {
        listening_ = can_receive() && object.GetNamedBoolean(L"on", false);
        if (listening_) {
            attempts_ = 0;
            retry_ = 0;
            error_.clear();
        } else if (const auto reason = string(object, L"reason"); !reason.empty() && can_receive()) {
            failure(L"Receiver discovery failed: " + reason);
        }
    } else if (type == L"link") {
        engine_link_ = object.GetNamedBoolean(L"up", false);
        if (!engine_link_) {
            listening_ = false;
            devices_.clear();
            discovery_candidates_.clear();
            refresh_scan_peers();
        }
    } else if (type == L"looking") {
        looking_ = look_requested_ && object.GetNamedBoolean(L"on", false);
        if (looking_) {
            look_error_.clear();
            look_attempts_ = 0;
        } else if (const auto why = string(object, L"why"); !why.empty() && look_requested_) {
            look_error_ = L"AWDL discovery failed: " + why;
            auto close = command(L"look");
            close.Insert(L"on", JsonValue::CreateBooleanValue(false));
            engine_->send(close);
            look_requested_ = false;
            look_retry_ = GetTickCount64() + std::min<ULONGLONG>(30'000, 2000ULL << std::min(look_attempts_++, 4U));
        }
    } else if (type == L"unwilling") {
        receiver_hidden_ = number(object, L"count") > 0;
    } else if (type == L"peer_candidate" && !id.empty()) {
        discovery_candidates_[id] = string(object, L"state");
        devices_.erase(id);
        refresh_scan_peers();
    } else if (type == L"peer" && !id.empty()) {
        auto name = string(object, L"name");
        discovery_candidates_.erase(id);
        devices_[id] = {name.empty() ? L"Apple device" : std::move(name), string(object, L"model")};
        refresh_scan_peers();
    } else if (type == L"peer_gone") {
        discovery_candidates_.erase(id);
        devices_.erase(id);
        refresh_scan_peers();
    } else if (type == L"asked") {
        ask(object);
    } else if (type == L"answered" && string(object, L"how") != L"accepted") {
        close_prompt(id);
    } else if (type == L"stored" || type == L"lost") {
        close_prompt(id);
        balloon(type == L"stored" ? L"Received files saved." :
            L"Incoming transfer failed: " + string(object, L"reason"));
    } else if (type == L"send" && id == active_send_) {
        const auto state = string(object, L"state");
        const auto total = number(object, L"total");
        const auto done = number(object, L"done", number(object, L"sent"));
        if (total > 0) transfer_percent_ = static_cast<unsigned>(std::clamp(done * 100 / total, 0.0, 100.0));
        const auto reason = string(object, L"reason");
        const auto outcome = state == L"sent" || state == L"done" ? L"Received by the recipient"
            : state == L"cancelled" ? L"Transfer cancelled"
            : state == L"declined" ? L"The recipient declined"
            : state == L"refused" ? L"The recipient could not save the files"
            : state == L"failed" ? (reason == L"gone" ? L"The selected device left. Choose it again when it returns."
                : L"Transfer failed: " + reason)
            : state == L"sending" ? L"Sending…"
            : state == L"finishing" ? L"Waiting for the recipient to finish saving…" : L"Waiting for the recipient…";
        transfer_message_ = outcome;
        if (state == L"sent" || state == L"done" || state == L"cancelled" || state == L"declined" ||
            state == L"refused" || state == L"failed") {
            active_send_.clear();
            cancelling_send_ = false;
            if (state == L"sent" || state == L"done") selected_files_.clear();
            balloon(outcome);
        }
    } else if (type == L"fatal") {
        failure(L"Transfer engine failed: " + string(object, L"reason") + string(object, L"detail"));
    }
    sync_discovery();
    update_status();
}

void Tray::start_beacon()
{
    const auto generation = ++beacon_generation_;
    beacon_error_.clear();
    try {
        // Same AirDrop discovery packet as the standalone engine's beacon.
        std::array<uint8_t, 20> payload{};
        payload[0] = 0x05;
        payload[1] = 0x12;
        payload[10] = 0x01;
        if (BCryptGenRandom(nullptr, payload.data() + 11, 8, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("Could not create the Bluetooth discovery payload.");
        winrt::Windows::Storage::Streams::DataWriter writer;
        writer.WriteBytes(payload);
        BluetoothLEAdvertisement advertisement;
        advertisement.ManufacturerData().Append(BluetoothLEManufacturerData(0x004c, writer.DetachBuffer()));
        beacon_ = BluetoothLEAdvertisementPublisher(advertisement);
        beacon_token_ = beacon_.StatusChanged([target = std::weak_ptr<Mailbox>(mailbox_), generation](
            const BluetoothLEAdvertisementPublisher&, const BluetoothLEAdvertisementPublisherStatusChangedEventArgs& args) {
            using enum BluetoothLEAdvertisementPublisherStatus;
            const auto status = args.Status();
            if (status != Started && status != Aborted && status != Stopped) return;
            if (const auto mailbox = target.lock()) {
                auto reason = status == Aborted ? beacon_error(args.Error()) :
                    status == Stopped ? std::string("Windows stopped Bluetooth advertising.") : std::string{};
                mailbox->put({Origin::beacon, generation, std::move(reason)});
            }
        });
        beacon_deadline_ = GetTickCount64() + 5000;
        beacon_.Start();
        return;
    } catch (const winrt::hresult_error& error) {
        beacon_error_ = L"Bluetooth advertising failed: " + std::wstring(error.message());
    } catch (const std::exception& error) {
        beacon_error_ = winrt::to_hstring(error.what());
    }
    stop_beacon();
    beacon_retry_ = GetTickCount64() + 15'000;
}

void Tray::stop_beacon()
{
    beacon_started_ = false;
    beacon_deadline_ = 0;
    if (!beacon_) return;
    ++beacon_generation_;
    if (beacon_token_.value) beacon_.StatusChanged(beacon_token_);
    try { beacon_.Stop(); } catch (const winrt::hresult_error&) { }
    beacon_token_ = {};
    beacon_ = nullptr;
}

void Tray::on_beacon(const Event& event)
{
    if (!beacon_ || event.generation != beacon_generation_) return;
    if (event.line.empty()) {
        beacon_started_ = true;
        beacon_deadline_ = 0;
    } else {
        beacon_error_ = winrt::to_hstring(event.line);
        stop_beacon();
        beacon_retry_ = GetTickCount64() + 15'000;
    }
    update_discovery_hint();
}

std::wstring Tray::discovery_status() const
{
    if (!leased_) return service_detail_.empty() ? L"Waiting for the Airdraft radio service." : service_detail_;
    if (wifi_return_incomplete_) return L"Windows Wi-Fi has not fully reconnected.";
    if (stop_sent_ || stop_pending_ || wifi_return_pending_) return L"Waiting for Windows Wi-Fi to reconnect…";
    if (driver_change_ == L"taking" || radio_ == L"starting") return L"Starting AirDrop…";
    if (radio_ == L"wifiprotected")
        return L"Wi-Fi is in use. Start AirDrop to scan. Wi-Fi will pause for the session.";
    if (radio_ == L"error" && !detail_.empty()) return detail_;
    if (experimental_radio_ && !experimental_session_ && !offline_session_) return L"Start AirDrop to scan for nearby devices.";
    if (!radio_mode_available()) return L"The Airdraft radio service is configured in an unavailable mode.";
    if (!error_.empty() && !engine_link_) return error_;
    if (!engine_link_) return L"Waiting for the AirDrop link…";
    if (!look_error_.empty()) return look_error_;
    if (!beacon_error_.empty()) return beacon_error_;
    if (!looking_) return L"Starting nearby discovery…";
    if (!beacon_started_) return L"Starting Bluetooth advertising…";
    if (!devices_.empty()) return {};
    if (std::ranges::any_of(discovery_candidates_, [](const auto& item) { return item.second == L"checking"; }))
        return L"AirDrop devices found. Checking which devices can receive files…";
    if (receiver_hidden_ ||
        std::ranges::any_of(discovery_candidates_, [](const auto& item) { return item.second == L"restricted"; }))
        return L"A nearby device withheld its name. Set AirDrop to Everyone for 10 Minutes on that device.";
    if (!discovery_candidates_.empty())
        return L"AirDrop devices found. Retrying the connection. Keep the receiving device awake.";
    return L"Scanning for devices… Set AirDrop to Everyone for 10 Minutes on the device you want to find.";
}

void Tray::update_discovery_hint()
{
    if (!scan_) return;
    set_label(scan_status_heading_, wifi_return_incomplete_ ? L"Wi-Fi recovery needs attention"
        : stop_sent_ || stop_pending_ || wifi_return_pending_ ? L"Reconnecting Wi-Fi"
        : radio_ == L"wifiprotected" ? L"Wi-Fi protected"
        : radio_ == L"error" || !error_.empty() ? L"Connection needs attention"
        : radio_ == L"starting" || driver_change_ == L"taking" ? L"Starting AirDrop"
        : link_ready() ? L"AirDrop active" : L"AirDrop connection");
    auto status = discovery_status();
    if (status.empty())
        status = offline_session_ ? L"AirDrop is ready. Stop AirDrop to reconnect Wi-Fi."
                                 : L"Select a device or drop files on it.";
    if (looking_ && beacon_started_ && beacon_error_.empty() && devices_.empty() &&
        discovery_candidates_.empty() && !receiver_hidden_ && look_error_.empty() && awdl_peer_count_ > 0) {
        status = L"Waiting for nearby Apple devices to advertise AirDrop. Set the receiving device to Everyone for 10 Minutes.";
    }
    if (status != shown_scan_status_) {
        SetWindowTextW(scan_status_, status.c_str());
        shown_scan_status_ = std::move(status);
        InvalidateRect(scan_, &scan_band_, FALSE);
        InvalidateRect(scan_status_heading_, nullptr, FALSE);
        InvalidateRect(scan_session_, nullptr, FALSE);
    }
    const auto available = devices_.size();
    set_label(scan_result_, available == 1 ? L"1 ready" : std::to_wstring(available) + L" ready");
    const bool grid_has_items = !scan_rows_.empty();
    if (((GetWindowLongPtrW(scan_peers_, GWL_STYLE) & WS_VISIBLE) != 0) != grid_has_items) {
        set_visible(scan_peers_, grid_has_items);
        InvalidateRect(scan_, &scan_devices_panel_, FALSE);
    }
    set_visible(scan_result_, grid_has_items);
    set_visible(scan_empty_, !grid_has_items);
    set_label(scan_identity_, L"AirDrop name: " + preferences_.name);
    set_label(scan_session_, (offline_session_ || experimental_session_) ? L"Stop Air&Drop" : L"Start Air&Drop…");
    set_visible(scan_session_, can_toggle_offline() || can_toggle_experimental());
    update_send_controls();
}

void Tray::sync_discovery()
{
    const auto now = GetTickCount64();
    const bool desired = (scan_ || !active_send_.empty()) && engine_link_ &&
        wants_engine() && leased_ && radio_ == L"on" && engine_->running();
    if (!desired) {
        if (look_requested_) {
            auto close = command(L"look");
            close.Insert(L"on", JsonValue::CreateBooleanValue(false));
            engine_->send(close);
        }
        look_requested_ = looking_ = receiver_hidden_ = false;
        look_retry_ = beacon_retry_ = 0;
        look_attempts_ = 0;
        look_error_.clear();
        beacon_error_.clear();
    } else if (!look_requested_ && now >= look_retry_) {
        auto look = command(L"look");
        look.Insert(L"on", JsonValue::CreateBooleanValue(true));
        look_requested_ = engine_->send(look);
    }
    if (!looking_) {
        stop_beacon();
    } else if (beacon_deadline_ && now >= beacon_deadline_) {
        beacon_error_ = L"Windows has not started Bluetooth advertising. Check Bluetooth in Settings.";
        stop_beacon();
        beacon_retry_ = now + 15'000;
    } else if (!beacon_ && now >= beacon_retry_) {
        start_beacon();
    }
    update_discovery_hint();
}

void Tray::tick()
{
    if (closing_) return;
    const auto now = GetTickCount64();
    if (!leased_ && !radio_.empty() && !suspended_ && now >= lease_retry_) {
        lease_retry_ = now + 2'000;
        service_->send(command(L"desktop-open"));
    }
    if (!suspended_ && now >= bluetooth_after_) {
        bluetooth_after_ = now + 30'000;
        probe_bluetooth(mailbox_);
    }
    if (!wants_radio() && leased_ && !stop_sent_ && !stop_pending_ &&
        (radio_ == L"on" || radio_ == L"starting" || (wake_sent_ && radio_ != L"error"))) {
        stop_sent_ = service_->send(command(L"stop"));
        if (stop_sent_) stop_engine();
    }
    if (wants_engine() && leased_ && radio_mode_available() && !stop_sent_) {
        if (!experimental_radio_ && !wake_sent_ && radio_ != L"on" && radio_ != L"starting")
            wake_sent_ = service_->send(command(L"wake"));
        if (radio_ == L"on" && !wifi_handoff_pending_) {
            if (experimental_radio_ && !active_send_.empty()) service_->send(command(L"touch"));
            const auto index = awdl_index();
            if (engine_index_ != index) stop_engine();
            if (index && !engine_->running() && GetTickCount64() >= retry_) {
                retry_ = GetTickCount64() + std::min<ULONGLONG>(60000, 2000ULL << std::min(attempts_++, 5U));
                std::wstring why;
                if (engine_->start(root_, preferences_.folder, keys_, instance_id_, preferences_.name, can_receive(), why)) {
                    engine_index_ = index;
                    error_.clear();
                } else failure(std::move(why));
            }
        }
    } else if (!wants_engine() && engine_->running()) {
        stop_engine();
    }
    sync_discovery();
    update_status();
}

void Tray::stop_engine()
{
    listening_ = false;
    engine_index_ = 0;
    engine_link_ = false;
    look_requested_ = false;
    sync_discovery();
    update_status();
    requests_.clear();
    pending_.clear();
    if (prompt_) SendMessageW(prompt_, TDM_CLICK_BUTTON, 102, 0);
    engine_->stop();
    devices_.clear();
    discovery_candidates_.clear();
    refresh_scan_peers();
    if (!active_send_.empty()) {
        active_send_.clear();
        cancelling_send_ = false;
        transfer_message_ = L"Transfer interrupted";
        balloon(L"Transfer interrupted");
    }
    update_send_controls();
}

void Tray::change_intent()
{
    listening_ = false;
    attempts_ = 0;
    retry_ = 0;
    if (!experimental_radio_) wake_sent_ = false;
    if (!unlocked_ || (!enabled_ && !scan_ && active_send_.empty())) experimental_session_ = false;
    error_.clear();
    if (!can_receive()) {
        const auto pending = pending_;
        for (const auto& id : pending) answer(id, false);
        requests_.clear();
        if (prompt_) SendMessageW(prompt_, TDM_CLICK_BUTTON, 102, 0);
    }
    auto receive = command(L"receive");
    receive.Insert(L"on", JsonValue::CreateBooleanValue(can_receive()));
    engine_->send(receive);
    tick();
}

void Tray::ask(const JsonObject& object)
{
    const auto id = string(object, L"id");
    if (id.empty() || !pending_.insert(id).second) return;
    if (!can_receive() || pending_.size() > 8) {
        answer(id, false);
        return;
    }
    auto name = string(object, L"sender");
    const auto files = object.GetNamedArray(L"files", JsonArray{});
    std::wstring summary = std::to_wstring(files.Size()) + L" file(s)";
    const auto total = number(object, L"total");
    if (total > 0) summary += L" · " + std::to_wstring(static_cast<ULONGLONG>(total)) + L" bytes";
    const auto shown = std::min(6U, files.Size());
    for (unsigned i = 0; i < shown; ++i) summary += L"\n" + string(files.GetObjectAt(i), L"name").substr(0, 130);
    if (shown < files.Size()) summary += L"\n…";
    std::erase_if(name, [](wchar_t ch) { return iswcntrl(ch) || (ch >= 0x202A && ch <= 0x202E) || (ch >= 0x2066 && ch <= 0x2069); });
    if (name.empty()) name = L"Unknown Apple device";
    requests_.push_back({id, name.substr(0, 100), std::move(summary)});
    PostMessageW(window_, prompt_message, 0, 0);
}

void Tray::prompt()
{
    if (prompt_ || requests_.empty() || !can_receive()) return;
    auto next = std::move(requests_.front());
    requests_.pop_front();
    if (!pending_.contains(next.id)) {
        PostMessageW(window_, prompt_message, 0, 0);
        return;
    }
    prompt_id_ = next.id;
    const auto title = next.sender + L" wants to share files";
    const TASKDIALOG_BUTTON options[] = {{101, L"Save files"}, {102, L"Decline"}};
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = settings_window_ ? settings_window_ : review_window_ ? review_window_ : scan_ ? scan_ : window_;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_CALLBACK_TIMER;
    config.pszWindowTitle = L"Incoming AirDrop · Airdraft";
    config.pszMainInstruction = title.c_str();
    config.pszContent = next.summary.c_str();
    config.pszFooter = L"AirDrop could not verify the sender's name.";
    config.cButtons = ARRAYSIZE(options);
    config.pButtons = options;
    config.nDefaultButton = 102;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(this);
    config.pfCallback = [](HWND dialog, UINT notice, WPARAM time, LPARAM, LONG_PTR data) -> HRESULT {
        auto& tray = *reinterpret_cast<Tray*>(data);
        if (notice == TDN_CREATED) tray.prompt_ = dialog;
        if (notice == TDN_TIMER && time > 45000) SendMessageW(dialog, TDM_CLICK_BUTTON, 102, 0);
        if (notice == TDN_DESTROYED) tray.prompt_ = nullptr;
        return S_OK;
    };
    int button = 0;
    const auto opened = TaskDialogIndirect(&config, &button, nullptr, nullptr);
    prompt_id_.clear();
    answer(next.id, SUCCEEDED(opened) && button == 101 && session_unlocked());
    if (!requests_.empty()) PostMessageW(window_, prompt_message, 0, 0);
}

void Tray::answer(std::wstring_view id, bool accept)
{
    if (!pending_.erase(std::wstring(id))) return;
    auto answer = command(L"answer");
    answer.Insert(L"id", JsonValue::CreateStringValue(id));
    answer.Insert(L"accept", JsonValue::CreateBooleanValue(accept && can_receive()));
    engine_->send(answer);
}

void Tray::close_prompt(std::wstring_view id)
{
    pending_.erase(std::wstring(id));
    std::erase_if(requests_, [&](const Incoming& request) { return request.id == id; });
    if (prompt_ && id == prompt_id_) SendMessageW(prompt_, TDM_CLICK_BUTTON, 102, 0);
}

void Tray::shutdown()
{
    if (closing_) return;
    closing_ = true;
    stop_beacon();
    if (prompt_) SendMessageW(prompt_, TDM_CLICK_BUTTON, 102, 0);
    if (engine_) engine_->stop();
    if (service_) {
        if (leased_) service_->send(command(L"desktop-close"));
        service_->stop();
    }
    if (settings_window_) EndDialog(settings_window_, IDCANCEL);
    if (review_window_) EndDialog(review_window_, IDCANCEL);
    if (scan_) DestroyWindow(scan_);
    WTSUnRegisterSessionNotification(window_);
    if (suspend_notifications_) UnregisterSuspendResumeNotification(suspend_notifications_);
    KillTimer(window_, 1);
    Shell_NotifyIconW(NIM_DELETE, &icon_);
}

LRESULT Tray::on_tray(HWND window, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (taskbar_created_ && msg == taskbar_created_) { add_icon(); update_status(); return 0; }
    switch (msg) {
    case tray_message: {
        const auto event = LOWORD(lparam);
        if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
            POINT position{};
            if (event == WM_CONTEXTMENU) {
                position.x = static_cast<short>(LOWORD(wparam));
                position.y = static_cast<short>(HIWORD(wparam));
            }
            if (event == WM_RBUTTONUP || (position.x == -1 && position.y == -1)) GetCursorPos(&position);
            menu(position);
        } else if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONUP) {
            open_scan();
        }
        return 0;
    }
    case backend_message: if (mailbox_) events(); return 0;
    case prompt_message: prompt(); return 0;
    case open_window_message: open_scan(); return 0;
    case WM_TIMER: tick(); return 0;
    case WM_WTSSESSION_CHANGE:
        if (wparam == WTS_SESSION_LOCK || wparam == WTS_CONSOLE_DISCONNECT || wparam == WTS_SESSION_LOGOFF) {
            unlocked_ = false;
            change_intent();
        } else if (wparam == WTS_SESSION_UNLOCK || wparam == WTS_CONSOLE_CONNECT) {
            unlocked_ = session_unlocked();
            change_intent();
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wparam == PBT_APMSUSPEND && !suspended_) {
            suspended_ = true;
            experimental_session_ = false;
            wake_sent_ = false;
            awake_recovery_ = leased_ && service_->send(command(L"stop"));
            stop_engine();
        } else if ((wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) && suspended_) {
            suspended_ = false;
            bluetooth_after_ = 0;
            unlocked_ = session_unlocked();
            change_intent();
        }
        return TRUE;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION: if (wparam) shutdown(); return 0;
    case WM_DESTROY: shutdown(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, msg, wparam, lparam);
}
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR args, int)
{
    int count = 0;
    const std::unique_ptr<LPWSTR, decltype(&LocalFree)> parsed(
        CommandLineToArgvW(GetCommandLineW(), &count), LocalFree);
    const std::wstring_view option = parsed && count > 1 ? parsed.get()[1] : L"";
    const bool check_service = option == L"--check-service";
    try {
        if (!check_service) SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        if (check_service) return count == 2 && airdraft::service_responding(15'000) ? 0 : 1;
        if (option == L"--selftest") {
            const bool passed = count == 3 && airdraft::ensure_identity(parsed.get()[2]) &&
                airdraft::ensure_identity(parsed.get()[2]) &&
                airdraft::command(L"status").GetNamedString(L"do") == L"status";
            return passed ? 0 : 1;
        }
        const bool background = std::wstring_view(args).find(L"--background") != std::wstring_view::npos;
        airdraft::Handle single(CreateMutexW(nullptr, TRUE, LR"(Local\AirdraftTray)"));
        if (!single) return 1;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (!background) {
                if (const auto tray = FindWindowW(airdraft::window_class, nullptr))
                    PostMessageW(tray, airdraft::open_window_message, 0, 0);
            }
            return 0;
        }
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&controls);
        return airdraft::Tray(instance).run(background);
    } catch (const winrt::hresult_error& e) {
        if (check_service) return 1;
        MessageBoxW(nullptr, e.message().c_str(), L"Airdraft", MB_OK | MB_ICONERROR);
    } catch (const std::exception& e) {
        if (check_service) return 1;
        MessageBoxW(nullptr, winrt::to_hstring(e.what()).c_str(), L"Airdraft", MB_OK | MB_ICONERROR);
    }
    return 1;
}
