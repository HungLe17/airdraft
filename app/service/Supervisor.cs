// SPDX-License-Identifier: GPL-2.0-only
using System.Diagnostics;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text.Json.Nodes;

namespace Airdraft.Service;

/// <summary>
/// Owns the card and the two daemons: the user-mode radio (airdraft-radio for
/// Intel, airdraft-radio-rtl for Realtek RTL8852CE) and awdld (winawdl's AWDL
/// daemon). Channel policy, after
/// ForbiddenItlwm 0.6.0: start on 6, where every country allows AWDL; when a
/// peer's channel sequence names 44 or 149 and its country is known, restart
/// there (the move waits until nothing is transferring); fall back to 6 if
/// the 5 GHz channel stays passive.
/// </summary>
public sealed class Supervisor : IDisposable
{
    public enum RadioState { Off, Starting, On, WifiProtected, Error }

    readonly string _root = AppContext.BaseDirectory;
    readonly object _lock = new();
    readonly ServiceConfig _cfg = ServiceConfig.Load();
    /// <summary>The card being driven, for the settings ("Intel AX200"), "" when none was found.</summary>
    string _cardLabel = "";
    Process? _radio, _awdld;
    int _channel;
    int _awdlPeerCount;
    /// <summary>The network last read from Windows; once the card is ours the WLAN service sees nothing.</summary>
    Wlan.Network? _net;
    string _wifi = "";
    /* Wi-Fi management (the list, connect, forget, auto-connect) */
    readonly KnownNetworks _known = KnownNetworks.Load();
    List<JsonObject> _scan = [], _scanPending = [];
    bool _scanBusy, _wantBootScan, _channelOpen;
    int _launchGen;
    DateTime _scanAt = DateTime.MinValue, _wifiDownSince = DateTime.UtcNow;
    bool _userChose, _userOff;
    string _staSsid = "", _wifiError = "";
    (string Ssid, string? Password, bool? Auto)? _pendingConnect;
    static readonly TimeSpan StaleScan = TimeSpan.FromSeconds(20);
    string _detail = "";
    volatile bool _wanted;
    WifiHandoff? _handoff;
    NativeWifiRecovery? _nativeWifiRecovery;
    ExperimentalNetwork? _experimentalNetwork;
    bool _offlineSession;
    Task? _stopWork;
    string _driverChange = "";
    bool _driverRestoreFailed;
    DateTime _lastUse = DateTime.UtcNow, _lastData = DateTime.UtcNow, _startedAt;
    long _rxToUs;
    int _restarts;
    CancellationTokenSource _cts = new();
    int _disposed;

    public RadioState State { get; private set; } = RadioState.Off;
    public event Action<JsonObject>? Changed;
    public event Action<string>? Log;

    public NetMode Mode => _cfg.Mode;

    /// <summary>
    /// What the card can do, read when the service starts and whenever the card
    /// is taken: the 2 s watchdog and the 15 s gateway probe ask it, and must
    /// not enumerate PCI devices each time. Before a card is seen (or with none)
    /// it is the Intel set, so those paths behave exactly as before the catalog.
    /// </summary>
    AdapterProfile _profile = CardCatalog.PreviewProfile;
    AdapterCapabilities _caps = CardCatalog.PreviewProfile.Capabilities;

    /// <summary>
    /// The mode the card can run: the user's choice, except Concurrent on a
    /// card whose radio has no station, which runs on demand.
    /// </summary>
    NetMode EffectiveMode() => _offlineSession ? NetMode.OnDemand : CardCatalog.EffectiveMode(_cfg.Mode, _caps);

    public JsonObject Snapshot()
    {
        lock (_lock)
        {
            return new JsonObject
            {
                ["event"] = "state",
                ["radio"] = State.ToString().ToLowerInvariant(),
                ["mode"] = EffectiveMode().ToString().ToLowerInvariant(),
                ["experimentalRadio"] = CardCatalog.ExperimentalBuild,
                ["offlineSession"] = _offlineSession,
                ["channel"] = _channel,
                ["awdlPeerCount"] = _awdlPeerCount,
                ["country"] = _cfg.Country,
                ["fastUnlock"] = _cfg.FastUnlock,
                ["coexPlan"] = _cfg.CoexPlan,
                ["btCoex"] = _cfg.BtCoexBalanced,
                ["card"] = _cardLabel,
                ["manualCard"] = _cfg.ManualCard,
                ["manualModel"] = _cfg.ManualModel,
                ["wifi"] = _wifi,
                ["wifiHandoffPending"] = _handoff?.Pending ?? false,
                ["wifiReturnPending"] = _nativeWifiRecovery is { TimedOut: false },
                ["wifiReturnIncomplete"] = _nativeWifiRecovery is { TimedOut: true },
                ["driverChange"] = _driverChange,
                ["stopPending"] = _stopWork is not null,
                ["detail"] = _detail,
                ["ssid"] = _staSsid,
                ["scanning"] = _scanBusy,
                ["wifiError"] = _wifiError,
                ["networks"] = new JsonArray(_scan.Select(n =>
                {
                    var c = (JsonObject)n.DeepClone();
                    var known = _known.Find((string?)n["ssid"] ?? "");
                    c["saved"] = known is { HasKey: true };
                    c["auto"] = known?.AutoConnect ?? false;
                    c["connected"] = (string?)n["ssid"] == _staSsid && _wifi == "connected";
                    return (JsonNode)c;
                }).ToArray()),
                ["known"] = new JsonArray(_known.All().Select(k =>
                    (JsonNode)new JsonObject { ["ssid"] = k.Ssid, ["auto"] = k.AutoConnect }).ToArray()),
            };
        }
    }

    void Publish() => Changed?.Invoke(Snapshot());

    static readonly object LogFileLock = new();
    /// <summary>service.log moves to service.log.1 at this size (the radio adds ~3 lines every 5 s).</summary>
    const long LogFileLimit = 8 * 1024 * 1024;

    void Say(string line)
    {
        Log?.Invoke(line);
        try
        {
            ServiceConfig.EnsureSecureDirectory();
            var logPath = Path.Combine(ServiceConfig.Folder, "service.log");
            ServiceConfig.RejectReparsePoint(logPath);
            // Several threads log at once (radio, awdld, watchdog): one writer at a time.
            lock (LogFileLock)
            {
                if (File.Exists(logPath) && new FileInfo(logPath).Length > LogFileLimit)
                {
                    var old = logPath + ".1";
                    ServiceConfig.RejectReparsePoint(old);
                    File.Move(logPath, old, true);
                }
                File.AppendAllText(logPath,
                    $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {line}{Environment.NewLine}");
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    /// <summary>
    /// The first lines of every service start: which build, on which Windows,
    /// in which mode, with which card and driver, so a log says what it came
    /// from without asking the reporter.
    /// </summary>
    void SayHeader()
    {
        var asm = typeof(Supervisor).Assembly;
        var version = asm.GetCustomAttributes(typeof(System.Reflection.AssemblyInformationalVersionAttribute), false)
            .OfType<System.Reflection.AssemblyInformationalVersionAttribute>().FirstOrDefault()?.InformationalVersion
            ?? asm.GetName().Version?.ToString() ?? "?";
        string windows = "?";
        try
        {
            using var cv = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion");
            var build = cv?.GetValue("CurrentBuild") as string ?? "";
            var name = cv?.GetValue("ProductName") as string ?? "Windows";
            // ProductName still says "Windows 10" on Windows 11.
            if (int.TryParse(build, out var b) && b >= 22000) name = name.Replace("Windows 10", "Windows 11");
            windows = $"{name} {cv?.GetValue("DisplayVersion")} (build {build}.{cv?.GetValue("UBR")}, {cv?.GetValue("EditionID")})";
        }
        catch (Exception) { }
        Say($"===== Airdraft service {version}; {windows}; mode {_cfg.Mode}, country \"{_cfg.Country}\" =====");
        try
        {
            Card.Manual = (_cfg.ManualCard, _cfg.ManualModel);
            var card = Card.Find();
            if (card is null)
            {
                _cardLabel = !CardCatalog.ExperimentalBuild &&
                    Card.Candidates().Any(c => CardCatalog.Classify(c.HardwareId)?.Backend == RadioBackendKind.Devourer8852be)
                    ? "Realtek RTL8852BE requires the Realtek build"
                    : $"Exactly one {PreviewCardName} is required";
                Say("card: " + _cardLabel);
            }
            else
            {
                var p = card.Profile;
                _profile = p;
                _caps = p.Capabilities;
                _cardLabel = $"{p.VendorName} {p.FamilyName}";
                // Same line as before the catalog for Intel cards ("Intel INF on record").
                Say($"card: {card.InstanceId}; driver service {card.Service}, INF {Path.GetFileName(card.InfPath)} ({card.Provider}), problem {card.Problem}" +
                    $"; {p.VendorName} INF on record {Path.GetFileName(_cfg.VendorInf)}");
                var c = p.Capabilities;
                Say($"card profile: {p.VendorName} {p.FamilyName} ({p.VendorId}:{p.DeviceId}), radio {p.Backend}, " +
                    $"concurrent {c.SupportsConcurrentWifi}, fast send {c.SupportsFastSend}, A-MPDU {c.SupportsAggregate}, " +
                    $"kernel driver {c.SupportsKernelDriver}, register BAR >= 0x{c.MinBarBytes:x}");
                if (EffectiveMode() != _cfg.Mode)
                    Say($"mode: {p.VendorName} {p.FamilyName} cannot keep Wi-Fi beside AirDrop; runs {EffectiveMode()} instead of {_cfg.Mode}");
            }
        }
        catch (Exception e) { Say("card: " + e.Message); }
    }

    /// <summary>Called once when the service starts.</summary>
    public void Boot()
    {
        // A config the radio never read (it deletes it once read) holds the
        // Wi-Fi key: never leave one behind across a restart.
        try { File.Delete(Path.Combine(ServiceConfig.Folder, "run", "sta.conf")); } catch (Exception) { }
        SayHeader();
        if (!CardCatalog.ExperimentalBuild) _ = Task.Run(SetTapMetric);
        _ = RequestStop("checking the Windows driver at startup");
        _ = Task.Run(() => WlanBackup.Restore(Say, TimeSpan.FromSeconds(30), () => Card.VendorWlanInterface()));
        _ = Task.Run(() => Watchdog(_cts.Token));
    }

    public void SetMode(NetMode mode)
    {
        lock (_lock) { _cfg.Mode = mode; _cfg.Save(); }
        Say($"mode: {mode}");
        // Saved as chosen; a card without a station runs Concurrent on demand.
        if (EffectiveMode() == NetMode.Concurrent) Start("mode changed");
        else _ = RequestStop("mode changed");
        Publish();
    }

    /// <summary>Experimental fast unlock of a passive 5 GHz channel (#11); a running radio restarts to take it.</summary>
    public void SetFastUnlock(bool on)
    {
        lock (_lock)
        {
            if (_cfg.FastUnlock == on) return;
            _cfg.FastUnlock = on;
            _cfg.Save();
        }
        Say($"experimental fast passive-channel unlock: {(on ? "on" : "off")}");
        if (State is RadioState.On or RadioState.Starting)
            _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
        Publish();
    }

    /// <summary>Experimental coexistence plan (ForbiddenItlwm PR #30); a running radio restarts to take it.</summary>
    public void SetCoexPlan(bool on)
    {
        lock (_lock)
        {
            if (_cfg.CoexPlan == on) return;
            _cfg.CoexPlan = on;
            _cfg.Save();
        }
        Say($"experimental coexistence plan: {(on ? "on" : "off")}");
        if (State is RadioState.On or RadioState.Starting)
            _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
        Publish();
    }

    /// <summary>
    /// Experimental balanced Bluetooth coexistence (radio --bt-coex 1): the Bluetooth beacon
    /// that wakes Apple devices is no longer starved by AWDL; a running radio restarts to take it.
    /// </summary>
    public void SetBtCoex(bool on)
    {
        lock (_lock)
        {
            if (_cfg.BtCoexBalanced == on) return;
            _cfg.BtCoexBalanced = on;
            _cfg.Save();
        }
        Say($"experimental balanced Bluetooth coexistence: {(on ? "on" : "off")}");
        if (State is RadioState.On or RadioState.Starting)
            _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
        Publish();
    }

    /// <summary>
    /// The user's manual card choice (Settings): a PCI hardware ID and a
    /// CardCatalog.ManualModels key, or "" and "" for automatic. Used only for a
    /// card the catalog does not recognize by its ID. Returns "" or why not.
    /// </summary>
    public string SetManualCard(string hardwareId, string model)
    {
        hardwareId = hardwareId.Trim();
        if (hardwareId == "") model = "";
        else if (CardCatalog.ManualProfile(hardwareId, model) is null) return "this model does not fit this card";
        lock (_lock)
        {
            _cfg.ManualCard = hardwareId;
            _cfg.ManualModel = model;
            _cfg.Save();
            Card.Manual = (hardwareId, model);
        }
        Say(hardwareId == "" ? "card: automatic detection" : $"card: {hardwareId} chosen in settings as {model}");
        if (Card.Find() is { } c) { _profile = c.Profile; _caps = c.Profile.Capabilities; _cardLabel = $"{c.Profile.VendorName} {c.Profile.FamilyName}"; }
        else _cardLabel = "";
        if (_wanted) _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
        Publish();
        return "";
    }

    /// <summary>The client wants AirDrop now (opened the window, a phone nearby is sharing).</summary>
    public void Wake()
    {
        _lastUse = DateTime.UtcNow;
        if (EffectiveMode() == NetMode.Concurrent) Start("desktop requested concurrent operation");
    }

    public void StartExperimentalRadio()
    {
        if (!CardCatalog.ExperimentalBuild || EffectiveMode() != NetMode.OnDemand) return;
        _lastUse = DateTime.UtcNow;
        Start("desktop requested an RTL8852BE session");
    }

    public void StartOfflineRadio()
    {
        if (_cfg.Mode == NetMode.Off) return;
        Start($"desktop requested a temporary {PreviewCardName} AirDrop session", offlineSession: true);
    }

    public void Touch() => _lastUse = DateTime.UtcNow;

    public void Start(string why, bool offlineSession = false)
    {
        int channel;
        lock (_lock)
        {
            if (_wanted || _stopWork is not null || _driverRestoreFailed || _nativeWifiRecovery is not null ||
                Volatile.Read(ref _disposed) != 0) return;
            _wanted = true;
            _offlineSession = offlineSession;
            _restarts = 0;
            State = RadioState.Starting;
            _startedAt = DateTime.UtcNow;
            channel = CardCatalog.InitialRadioChannel(_profile, _cfg.StartChannel, _cfg.PeerChannel);
        }
        Say($"starting: {why}");
        _ = Task.Run(() => Launch(channel));
    }

    public Task RequestStop(string why)
    {
        Task stop;
        lock (_lock)
        {
            _wanted = false;
            if (_stopWork is not null) return _stopWork;
            _stopWork = stop = Task.Run(() => StopAndRestore(why));
        }
        Publish();
        return stop;
    }

    void StopAndRestore(string why)
    {
        Say($"stopping: {why}");
        // Serialized with launches (round 2 #7): a launch in progress sees
        // _wanted false at its next check and returns; then the card goes back.
        _launchGate.Wait();
        try
        {
            var hadSwitch = _cfg.VendorDriverReturnPending || _handoff is not null || _experimentalNetwork is not null || _radio is not null;
            var daemonsStopped = StopDaemons();
            hadSwitch |= Card.Find() is { } card && Card.BoundToProxy(card);
            var restored = _radio is null && EnsureVendorDriver();
            lock (_lock)
            {
                _driverRestoreFailed = (_driverRestoreFailed || hadSwitch) && !restored;
                _awdlPeerCount = 0;
                State = !daemonsStopped || _driverRestoreFailed ? RadioState.Error : RadioState.Off;
                _detail = !daemonsStopped ? "A radio helper is still running. Run Airdraft - restore Windows Wi-Fi."
                    : _driverRestoreFailed ? "Windows did not restore the Wi-Fi driver. Run Airdraft - restore Windows Wi-Fi." : "";
            }
        }
        catch (Exception e)
        {
            Say("could not stop and restore Windows Wi-Fi: " + e.Message);
            lock (_lock)
            {
                _driverRestoreFailed = true;
                State = RadioState.Error;
                _detail = "Windows Wi-Fi recovery failed. Run Airdraft - restore Windows Wi-Fi.";
            }
        }
        finally
        {
            BeginNativeWifiRecovery();
            lock (_lock)
            {
                _wifi = "";
                _channel = 0;
                _handoff = null;
                _experimentalNetwork = null;
                _offlineSession = false;
                _stopWork = null;
            }
            _launchGate.Release();
        }
        Publish();
    }

    /// <summary>One launch at a time: the watchdog, a wake, a mode change and a channel move
    /// used to start radios concurrently and kill each other's (hardware, 2026-11-17).</summary>
    readonly SemaphoreSlim _launchGate = new(1, 1);

    sealed class NativeWifiRecovery(NativeWifiReturn request)
    {
        public NativeWifiReturn Request { get; } = request;
        public long? StartedAt { get; set; }
        public Guid? InterfaceId { get; set; }
        public long InterfaceSeenAt { get; set; }
        public long NextCheckAt { get; set; }
        public bool ReconnectRequested { get; set; }
        public bool TimedOut { get; set; }
    }

    void BeginNativeWifiRecovery()
    {
        lock (_lock)
        {
            if (_nativeWifiRecovery is not null || _cfg.PendingWifiReturn is not { NeedsRecovery: true } request)
                return;
            _nativeWifiRecovery = new(request);
        }
        Say("Wi-Fi return: waiting for the native network connection");
    }

    void CheckNativeWifiRecovery()
    {
        if (!_launchGate.Wait(0)) return;
        try
        {
            var now = Environment.TickCount64;
            if (_nativeWifiRecovery is not { } recovery || now < recovery.NextCheckAt) return;
            recovery.StartedAt ??= now;
            var paths = WifiPaths.None;
            var request = recovery.Request;
            var adapter = Card.VendorWlanInterface(request.InstanceId);
            if (adapter is { } interfaceId)
            {
                try { paths = WifiHandoff.NativePaths(interfaceId); }
                catch (System.Net.NetworkInformation.NetworkInformationException) { }
                var connected = request.RequiredPaths == WifiPaths.None &&
                    Wlan.Current(out _, readKey: false, interfaceId: interfaceId) is not null;
                if (request.IsRestored(paths, connected))
                {
                    lock (_lock)
                    {
                        _nativeWifiRecovery = null;
                        _cfg.PendingWifiReturn = null;
                        try { _cfg.Save(); }
                        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                        { Say("Wi-Fi return: could not clear saved recovery state: " + e.Message); }
                    }
                    Say(request.RequiredPaths == WifiPaths.None ? "Wi-Fi return: the native adapter reconnected"
                        : $"Wi-Fi return: native adapter recovered its previous IP paths ({paths})");
                    Publish();
                    return;
                }
                if (recovery.InterfaceId != interfaceId)
                {
                    recovery.InterfaceId = interfaceId;
                    recovery.InterfaceSeenAt = now;
                }
                if (!recovery.TimedOut && !recovery.ReconnectRequested && now - recovery.InterfaceSeenAt >= 4000 &&
                    now - recovery.StartedAt < 30_000 && Wlan.RequestReconnect(interfaceId, request.ProfileName) is { } result)
                {
                    recovery.ReconnectRequested = true;
                    Say(result == 0 ? "Wi-Fi return: requested the previous saved connection on the native adapter"
                        : $"Wi-Fi return: Windows rejected the reconnect request (error {result})");
                }
            }
            else recovery.InterfaceId = null;
            if (!recovery.TimedOut && now - recovery.StartedAt >= 30_000)
            {
                lock (_lock) recovery.TimedOut = true;
                Say(request.RequiredPaths == WifiPaths.None ? "Wi-Fi return: the native adapter is still disconnected"
                    : $"Wi-Fi return: previous IP paths are still missing (need {request.RequiredPaths}, have {paths})");
                Publish();
            }
            if (recovery.TimedOut) recovery.NextCheckAt = now + 10_000;
        }
        finally { _launchGate.Release(); }
    }

    internal bool WaitForNativeWifiRecovery()
    {
        while (_nativeWifiRecovery is { TimedOut: false })
        {
            CheckNativeWifiRecovery();
            if (_nativeWifiRecovery is { TimedOut: false }) Thread.Sleep(1000);
        }
        return _nativeWifiRecovery is null;
    }

    void Launch(int channel)
    {
        _launchGate.Wait();
        try { LaunchLocked(channel); }
        finally { _launchGate.Release(); }
    }

    void LaunchLocked(int channel, (int Generation, int Move)? peerMove = null)
    {
        int generation;
        lock (_lock)
        {
            if (!_wanted || (peerMove is { } change &&
                (change.Generation != _launchGen || change.Move != _moveGen))) return;
            State = RadioState.Starting; _channel = channel; _detail = "";
            _awdlPeerCount = 0;
            _rxToUs = 0;
            if (_handoff is not null)
            {
                _handoff.MustRestore(Environment.TickCount64, false, WifiPaths.None);
                _wifi = "";
            }
            // A scan the old radio never finished must not block auto-connect forever (Codex review #29).
            _scanBusy = false;
            _wantBootScan = false;
            _channelOpen = false;
            generation = ++_launchGen;
            _scanPending = [];
        }
        if (peerMove is not null) Say($"peer uses channel {channel}: moving there (country {_cfg.Country})");
        Publish();
        try
        {
            if (!StopDaemons()) throw new InvalidOperationException("A previous radio helper is still running");
            string? staConf = null;
            NativeWifiReturn? wifiReturn = null;
            var card = Card.Find() ?? throw Preflight($"exactly one {PreviewCardName} is required");
            var kernel = Card.ByKernelDriver(card);
            if (CardCatalog.ExperimentalBuild && !_offlineSession) wifiReturn = PrepareExperimentalSession(card);
            if (_offlineSession && !kernel && !Card.BoundToProxy(card)) wifiReturn = PrepareOfflineSession(card);
            if (!kernel && EffectiveMode() == NetMode.Concurrent)
            {
                var why = "";
                var cardFree = !Card.BoundToProxy(card);
                Wlan.Network? fresh = null;
                var originalPaths = WifiPaths.None;
                if (cardFree)
                {
                    if (card.InterfaceId is not { } interfaceId)
                        throw Preflight("Windows did not report the AX210 network interface");
                    (originalPaths, var alternatives) = WifiHandoff.CurrentPaths(interfaceId);
                    if (WifiHandoff.WouldInterruptWifi(originalPaths, alternatives, connected: false)) throw new WifiInUseException();
                    fresh = Wlan.Current(out why, interfaceId: interfaceId);
                    if (why == "Wi-Fi is not connected" && Wlan.IsConnecting(interfaceId))
                        throw Preflight("Windows is still connecting to Wi-Fi. Retry once it connects");
                    if (why is not ("" or "Wi-Fi is not connected")) throw Preflight(why);
                    if (fresh is not null && (!HasStationConfig(fresh) || fresh.Channel <= 0))
                        throw Preflight("Airdraft could not read a usable network profile and channel for the AX210");
                    if (originalPaths != WifiPaths.None && fresh is null)
                        throw Preflight("The AX210 has active routes but Windows did not report a connected Wi-Fi profile");
                    if (WifiHandoff.WouldInterruptWifi(originalPaths, alternatives, fresh is not null)) throw new WifiInUseException();
                    if (fresh is not null && !WifiHandoff.HasTap())
                        throw Preflight("The Airdraft TAP adapter is missing. Run Airdraft Setup");

                    try
                    {
                        var imported = _known.ImportFromWindows();
                        if (imported > 0) Say($"Wi-Fi: imported {imported} saved network(s) from Windows");
                    }
                    catch (Exception e) { Say("Wi-Fi: could not read Windows' saved networks: " + e.Message); }
                    // On every new takeover use the connection Windows actually has now.
                    _net = fresh;
                    _userOff = _userChose = false;
                }
                if (fresh is not null)
                {
                    _known.Remember(fresh.Ssid, fresh.Passphrase, fresh.PskHex, fresh.Open);
                }
                var wired = fresh is null && _net is null && !_userChose && Wlan.WiredUp();
                var net = _userOff ? null : _net;
                lock (_lock) _staSsid = net?.Ssid ?? "";
                if (net is not null && HasStationConfig(net))
                {
                    staConf = Wlan.WriteStaConfig(net);
                    Say($"Wi-Fi: joining \"{net.Ssid}\" ({net.Security}, channel {net.Channel}) beside AirDrop");
                }
                else if (net is null && !wired && !_userOff && !_userChose &&
                         why is "Wi-Fi is not connected" or "no WLAN interface" or "" &&
                         _known.All().Any(k => k.HasKey && k.AutoConnect))
                {
                    // Saved networks were imported into _known: start AirDrop at once and
                    // let the radio scan and join the strongest one in range (#4).
                    Say("Wi-Fi: Windows is not connected; AirDrop starts now and Airdraft will scan for a saved network");
                    lock (_lock) { _wifi = ""; _wantBootScan = true; }
                }
                if (cardFree) wifiReturn = CaptureWifiReturn(card, originalPaths, fresh);
            }
            // With nekodrop-wlan.sys the card stays where it is: Windows runs
            // the station, the radio only drives AWDL through the driver.
            // Stop may have run meanwhile: never take the card or start a radio for it (Codex review #10).
            if (!_wanted) { lock (_lock) State = RadioState.Off; Publish(); return; }
            if (!kernel) TakeCard(card, wifiReturn);
            else if (Card.Find() is { } kc) { _profile = kc.Profile; _caps = kc.Profile.Capabilities; }
            if (!_wanted) { lock (_lock) State = RadioState.Off; Publish(); return; }
            if (CardCatalog.ExperimentalBuild && !_offlineSession) VerifyExperimentalPaths();
            // The AX210's Bluetooth beacon needs airtime to wake AirDrop
            // receivers while the Wi-Fi side is listening on AWDL.
            var launch = CardCatalog.BuildRadioLaunch(_profile, channel, kernel, _cfg.Country, staConf, true, _cfg.FastUnlock,
                                                      _cfg.CoexPlan, _offlineSession || _cfg.BtCoexBalanced);
            _radio = Spawn(Path.Combine(_root, "radio", launch.ExeName), launch.Args,
                line => OnRadioLine(line, generation), supervisedRadio: true);
            _startedAt = DateTime.UtcNow;
            // awdld connects to the radio's pipe; give the firmware time to boot.
            var readyBy = Environment.TickCount64 + 30_000;
            while (_wanted && State == RadioState.Starting && _radio is { HasExited: false } &&
                   Environment.TickCount64 < readyBy)
            {
                if (CardCatalog.ExperimentalBuild && !_offlineSession) VerifyExperimentalPaths();
                if (_handoff?.Expired(Environment.TickCount64) == true)
                    throw new TimeoutException("Wi-Fi handoff timed out waiting for the radio to start");
                Thread.Sleep(100);
            }
            if (!_wanted) return;
            if (_radio is null || _radio.HasExited) throw new InvalidOperationException("the radio exited during bring-up: " + _detail);
            if (State != RadioState.On) throw new TimeoutException("the radio did not become ready within 30 seconds");
            _awdld = Spawn(Path.Combine(_root, "awdl", "awdld.exe"), ["-c", channel.ToString()], line => OnAwdldLine(line, generation));
        }
        catch (Exception e)
        {
            try { File.Delete(Path.Combine(ServiceConfig.Folder, "run", "sta.conf")); } catch (Exception) { }
            if (e is WifiInUseException)
            {
                lock (_lock)
                {
                    _wanted = false;
                    State = RadioState.WifiProtected;
                    _detail = e.Message;
                }
                Say(e.Message);
                Publish();
                return;
            }
            var bind = e as CardBindException ?? e.InnerException as CardBindException;
            Say("start failed: " + (bind?.Message ?? e.Message));
            if (_handoff is not null || _experimentalNetwork is not null || (_offlineSession && _cfg.VendorDriverReturnPending))
            {
                RestoreAfterHandoffFailureLocked("Airdraft radio startup failed: " + (bind?.Message ?? e.Message));
                return;
            }
            lock (_lock)
            {
                // Windows refusing the driver is not cured by retrying: no restart loop
                // (it ran five times, each time leaving the card without a driver).
                if (bind is not null) _wanted = false;
                if (!_wanted) _offlineSession = false;
                State = _wanted || bind is not null ? RadioState.Error : RadioState.Off;
                _detail = bind?.Message ?? e.Message;
            }
            Publish();
        }
    }

    static bool HasStationConfig(Wlan.Network net) =>
        net.Open || !string.IsNullOrEmpty(net.Passphrase) || !string.IsNullOrEmpty(net.PskHex);

    static CardBindException Preflight(string why) =>
        new($"Wi-Fi preflight: {why}. The Windows Wi-Fi driver remains active.", new InvalidOperationException(why));

    sealed class WifiInUseException() : InvalidOperationException(
        "Windows is using the AX210 for Wi-Fi. Choose Start AirDrop session in the tray to use the card temporarily.");

    static string PreviewCardName => CardCatalog.ExperimentalBuild ? "Realtek RTL8852BE (10EC:B852 or 10EC:B85B)" : "Intel AX210 (8086:2725)";

    sealed class ExperimentalNetwork(Guid excludedInterface, WifiPaths required)
    {
        public Guid ExcludedInterface { get; } = excludedInterface;
        public WifiPaths Required { get; } = required;
        WifiPaths _answered;
        int _ipv4Misses, _ipv6Misses;

        public WifiPaths Record(WifiPaths replies)
        {
            _answered |= replies;
            var missing = _answered & ~replies;
            _ipv4Misses = missing.HasFlag(WifiPaths.IPv4) ? _ipv4Misses + 1 : 0;
            _ipv6Misses = missing.HasFlag(WifiPaths.IPv6) ? _ipv6Misses + 1 : 0;
            return (_ipv4Misses >= 3 ? WifiPaths.IPv4 : WifiPaths.None) |
                   (_ipv6Misses >= 3 ? WifiPaths.IPv6 : WifiPaths.None);
        }
    }

    static NativeWifiReturn? CaptureWifiReturn(Card.Info card, WifiPaths paths, Wlan.Network? network) =>
        paths == WifiPaths.None ? null : new(card.InstanceId, network?.ProfileName ?? "",
            paths.HasFlag(WifiPaths.IPv4), paths.HasFlag(WifiPaths.IPv6));

    static NativeWifiReturn? PrepareOfflineSession(Card.Info card)
    {
        if (card.InterfaceId is not { } wifi) throw Preflight("Windows did not report the Wi-Fi network interface");
        var paths = WifiHandoff.NativePaths(wifi);
        var network = Wlan.Current(out var why, readKey: false, interfaceId: wifi);
        if (why is not ("" or "Wi-Fi is not connected")) throw Preflight(why);
        if (paths != WifiPaths.None && string.IsNullOrEmpty(network?.ProfileName))
            throw Preflight("the saved Wi-Fi profile is needed to reconnect after AirDrop");
        if (paths == WifiPaths.None && network is { ProfileName.Length: > 0 })
            return new NativeWifiReturn(card.InstanceId, network.ProfileName, IPv4: false, IPv6: false);
        return CaptureWifiReturn(card, paths, network);
    }

    NativeWifiReturn? PrepareExperimentalSession(Card.Info card)
    {
        var radio = RadioBackends.Get(card.Profile.Backend).ExeName;
        if (!File.Exists(Path.Combine(_root, "radio", radio)) ||
            !File.Exists(Path.Combine(_root, "awdl", "awdld.exe")) ||
            !File.Exists(Path.Combine(_root, "driver", card.Profile.ProxyInfName)))
            throw Preflight("the experimental radio, AWDL daemon or register proxy is missing. Run the experimental Setup");
        if (Card.BoundToProxy(card))
        {
            if (_experimentalNetwork is null ||
                !string.Equals(_cfg.VendorInstanceId, card.InstanceId, StringComparison.OrdinalIgnoreCase))
                throw new CardBindException("The RTL8852BE was already bound to the proxy. Run Airdraft - restore Windows Wi-Fi.", new InvalidOperationException());
            VerifyExperimentalPaths();
            return null;
        }
        if (card.Problem != 0 || card.InterfaceId is not { } wifiInterface ||
            !TryGetVendorInf(card, card.InfPath, out _))
            throw Preflight("the RTL8852BE needs its working Windows driver and recovery INF before Airdraft can take it");
        var (original, alternative) = WifiHandoff.CurrentPaths(wifiInterface);
        if (!WifiHandoff.HasAlternatePaths(original, alternative))
            throw Preflight("connect another physical adapter that covers the active IPv4 and IPv6 paths first");
        var network = new ExperimentalNetwork(wifiInterface, alternative);
        try
        {
            var replies = WifiHandoff.ProbeAlternateGateways(wifiInterface, alternative).GetAwaiter().GetResult();
            network.Record(replies);
            Say($"RTL8852BE alternate adapters have {alternative}; gateway replies before takeover: {replies}");
        }
        catch (NetworkInformationException e) { Say("alternate network gateway snapshot: " + e.Message); }
        _experimentalNetwork = network;
        var native = original == WifiPaths.None ? null : Wlan.Current(out _, readKey: false, interfaceId: wifiInterface);
        return CaptureWifiReturn(card, original, native);
    }

    void VerifyExperimentalPaths()
    {
        if (_experimentalNetwork is not { } network ||
            !WifiHandoff.HasAlternatePaths(network.Required, WifiHandoff.CurrentPaths(network.ExcludedInterface).Alternative))
            throw new InvalidOperationException("The alternate network adapter lost a required IP path");
    }

    async Task ProbeExperimentalAlternates()
    {
        if (_experimentalNetwork is not { } network || State != RadioState.On || !_wanted) return;
        var generation = Volatile.Read(ref _launchGen);
        var replies = await WifiHandoff.ProbeAlternateGateways(network.ExcludedInterface, network.Required);
        if (!_launchGate.Wait(0)) return;
        try
        {
            if (!_wanted || _launchGen != generation || !ReferenceEquals(network, _experimentalNetwork)) return;
            var lost = network.Record(replies);
            if (lost != WifiPaths.None)
                RestoreAfterHandoffFailureLocked($"The alternate network stopped reaching its {lost} gateway");
        }
        finally { _launchGate.Release(); }
    }

    /* awdld warns for every frame it does not parse, several a second (issue #1):
       the first is logged, then one count a minute. */
    readonly object _noiseLock = new();
    DateTime _noiseAt = DateTime.MinValue;
    int _noiseCount;

    void OnAwdldLine(string line, int generation)
    {
        const string peerSummary = " : peers ";
        int start = line.IndexOf(peerSummary, StringComparison.Ordinal);
        if (start >= 0)
        {
            start += peerSummary.Length;
            int end = line.IndexOf(" |", start, StringComparison.Ordinal);
            if (end > start && int.TryParse(line.AsSpan(start, end - start), out var count))
            {
                bool changed;
                lock (_lock)
                {
                    changed = _wanted && generation == _launchGen && _awdlPeerCount != count;
                    if (changed) _awdlPeerCount = count;
                }
                if (changed) Publish();
            }
        }
        if (line.Contains("unhandled frame", StringComparison.Ordinal))
        {
            string? summary = null;
            lock (_noiseLock)
            {
                var now = DateTime.UtcNow;
                if (_noiseAt == DateTime.MinValue) { _noiseAt = now; summary = line + " (repeats are counted once a minute)"; }
                else
                {
                    _noiseCount++;
                    if (now - _noiseAt >= TimeSpan.FromMinutes(1))
                    {
                        summary = $"{_noiseCount} more unhandled frames";
                        _noiseCount = 0;
                        _noiseAt = now;
                    }
                }
            }
            if (summary is not null) Say("awdld: " + summary);
            return;
        }
        Say("awdld: " + line);
    }

    void OnRadioLine(string line, int generation)
    {
        Say("radio: " + line);
        if (!_wanted || generation != Volatile.Read(ref _launchGen)) return;
        var at = line.IndexOf("radio: ", StringComparison.Ordinal);
        var text = at >= 0 ? line[(at + 7)..] : line;
        if (text.StartsWith("ready on channel", StringComparison.Ordinal))
        {
            bool bootScan;
            lock (_lock)
            {
                if (!_wanted || generation != _launchGen) return;
                State = RadioState.On;
                bootScan = _wantBootScan;
                _wantBootScan = false;
                if (bootScan) _wifiDownSince = DateTime.MinValue;
            }
            Publish();
            if (bootScan) WifiScan();
        }
        else if (text.StartsWith("bring-up failed: ", StringComparison.Ordinal))
        {
            lock (_lock) _detail = text[17..];
        }
        else if (text.StartsWith("rx: ", StringComparison.Ordinal))
        {
            // "... N addressed to us": transfers keep a channel move waiting.
            var parts = text.Split(' ');
            var i = Array.IndexOf(parts, "addressed");
            if (i > 0 && long.TryParse(parts[i - 1], out var n))
            {
                lock (_lock)
                {
                    if (!_wanted || generation != _launchGen) return;
                    if (n > _rxToUs) { _lastData = DateTime.UtcNow; _lastUse = DateTime.UtcNow; }
                    _rxToUs = n;
                }
            }
        }
        else if (text.Contains("transmitting allowed here", StringComparison.Ordinal))
        {
            // A beacon or AWDL peer unlocked a passive 5 GHz channel (#11):
            // do not bounce back to channel 6 after 30 s.
            lock (_lock) _channelOpen = true;
        }
        else if (text.StartsWith("event: ", StringComparison.Ordinal))
        {
            OnEvent(text[7..].Split(' ', 2), generation);
        }
    }

    void OnEvent(string[] e, int generation)
    {
        var name = e[0];
        var value = e.Length > 1 ? e[1] : "";
        switch (name)
        {
            case "peer-country":
            case "country":
                if (value.Length == 2 && value != _cfg.Country)
                {
                    lock (_lock) { _cfg.Country = value; _cfg.Save(); }
                    Publish();
                }
                break;
            case "peer-channel":
                if (int.TryParse(value, out var ch) && (ch == 44 || ch == 149))
                {
                    int move;
                    lock (_lock)
                    {
                        if (!_wanted || generation != _launchGen) return;
                        move = ++_moveGen;
                        if (_profile.Backend != RadioBackendKind.Devourer8852be && _cfg.PeerChannel != ch)
                        {
                            _cfg.PeerChannel = ch;
                            _cfg.Save();
                        }
                        if (_channel == ch) return;
                    }
                    _ = Task.Run(() => MoveWhenIdle(ch, generation, move));
                }
                break;
            case "channel":
                // "44 passive": no transmitting until a beacon or AWDL peer is heard, or
                // the firmware unlocks the channel; if it stays passive, go back to 6 (#11).
                if (value.EndsWith("active", StringComparison.Ordinal))
                {
                    lock (_lock) _channelOpen = true;
                }
                else if (value.EndsWith("passive", StringComparison.Ordinal))
                {
                    int gen, passiveCh;
                    lock (_lock) { gen = _launchGen; passiveCh = _channel; }
                    _ = Task.Run(async () =>
                    {
                        await Task.Delay(TimeSpan.FromSeconds(30));
                        bool bounce;
                        lock (_lock)
                            bounce = State == RadioState.On && _launchGen == gen && _channel == passiveCh &&
                                     _channel > 14 && !_channelOpen && _rxToUs == 0 && _wifi != "connected";
                        if (bounce)
                        {
                            Say($"channel {passiveCh} stayed passive: back to {_cfg.StartChannel}");
                            lock (_lock) _cfg.PeerChannel = 0;
                            Relaunch(_cfg.StartChannel);
                        }
                    });
                }
                break;
            case "wifi":
                lock (_lock)
                {
                    if (value == "connected") { _wifiDownSince = DateTime.MaxValue; _wifiError = ""; }
                    else if (_wifi == "connected" || _wifiDownSince == DateTime.MaxValue) _wifiDownSince = DateTime.UtcNow;
                    _wifi = value;
                }
                Publish();
                // A failed join may be aimed at a BSSID that is gone (#27: the
                // radio retried the old one every 5 s). If the scan it came from
                // is old, scan for the name and rejoin with what answers now.
                if (value == "failed")
                {
                    (string Ssid, string? Password, bool? Auto)? again = null;
                    lock (_lock)
                        if (_net is { } n && !_userOff && !_scanBusy && _pendingConnect is null &&
                            DateTime.UtcNow - _scanAt > StaleScan)
                            again = (n.Ssid, null, null);
                    if (again is { } a)
                    {
                        Say($"Wi-Fi: joining \"{a.Ssid}\" failed; scanning for it again");
                        lock (_lock) _pendingConnect = a;
                        WifiScan(a.Ssid);
                    }
                }
                break;
            case "bss":
            {
                // "<bssid> <channel> <dBm> <security> <ssid hex>"
                var f = value.Split(' ');
                if (f.Length == 5 && int.TryParse(f[1], out var bch) && int.TryParse(f[2], out var dbm))
                {
                    string ssid;
                    try { ssid = System.Text.Encoding.UTF8.GetString(Convert.FromHexString(f[4])); }
                    catch (FormatException) { break; }
                    lock (_lock)
                        _scanPending.Add(new JsonObject
                        {
                            ["bssid"] = f[0], ["channel"] = bch, ["dbm"] = dbm, ["security"] = f[3], ["ssid"] = ssid,
                        });
                }
                break;
            }
            case "scan-done":
                lock (_lock)
                {
                    // One row per network name, its strongest access point.
                    _scan = _scanPending.GroupBy(n => (string?)n["ssid"] ?? "")
                        .Select(g => g.OrderByDescending(n => (int?)n["dbm"] ?? -127).First())
                        .OrderByDescending(n => (int?)n["dbm"] ?? -127).ToList();
                    _scanPending = [];
                    _scanBusy = false;
                    _scanAt = DateTime.UtcNow;
                }
                Publish();
                _ = Task.Run(AfterScan);
                break;
        }
    }

    int _moveGen;

    async Task MoveWhenIdle(int channel, int generation, int move)
    {
        // A newer channel event or launch supersedes an old move. Wait for
        // ten seconds of quiet, even if that means waiting for a transfer.
        while (true)
        {
            await _launchGate.WaitAsync();
            try
            {
                bool idle;
                lock (_lock)
                {
                    if (!_wanted || generation != _launchGen || move != _moveGen || _channel == channel) return;
                    idle = DateTime.UtcNow - _lastData > TimeSpan.FromSeconds(10);
                }
                if (idle)
                {
                    LaunchLocked(channel, (generation, move));
                    return;
                }
            }
            finally { _launchGate.Release(); }
            await Task.Delay(1000);
        }
    }

    void Relaunch(int channel)
    {
        _launchGate.Wait();
        try { LaunchLocked(channel); }
        finally { _launchGate.Release(); }
    }

    async Task Watchdog(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            await Task.Delay(2000, ct).ContinueWith(_ => { });
            if (ct.IsCancellationRequested) break;
            CheckNativeWifiRecovery();
            if (!_wanted) continue;
            if (await CheckWifiHandoff()) continue;
            // Snapshots: Relaunch swaps the processes on another thread (Codex review #11).
            var radio = _radio;
            var awdld = _awdld;
            var radioDown = DaemonExited(radio);
            var awdldDown = State is RadioState.On or RadioState.Error && DaemonExited(awdld);
            // A radio that never says "ready" would hold Starting for good (Codex review #12).
            var startStuck = State == RadioState.Starting && _launchGate.CurrentCount > 0 &&
                             DateTime.UtcNow - _startedAt > TimeSpan.FromSeconds(60);
            if (_scanBusy && DateTime.UtcNow - _scanStartedAt > TimeSpan.FromSeconds(30))
                lock (_lock) { _scanBusy = false; _scanPending = []; }
            if ((radioDown || awdldDown || startStuck) && (State != RadioState.Starting || startStuck) &&
                _launchGate.CurrentCount > 0)
            {
                if (RestoreAfterHandoffFailure("The radio or AWDL daemon stopped during the session")) continue;
                if (++_restarts > 5)
                {
                    Say("the radio keeps failing; giving the card back");
                    await RequestStop("too many failures");
                    lock (_lock) { State = RadioState.Error; _detail = "the radio keeps failing"; }
                    Publish();
                    continue;
                }
                Say(startStuck ? "the radio never became ready; restarting" : "a daemon stopped; restarting");
                Relaunch(_channel == 0 ? _cfg.StartChannel : _channel);
            }
            else if (State == RadioState.On && DateTime.UtcNow - _startedAt > TimeSpan.FromMinutes(2))
            {
                _restarts = 0;
            }
            // Wi-Fi down for a while: look for a known network to join.
            if (EffectiveMode() == NetMode.Concurrent && State == RadioState.On && !_userOff && _wifi != "connected" &&
                !_scanBusy && DateTime.UtcNow - _wifiDownSince > TimeSpan.FromSeconds(20) &&
                DateTime.UtcNow - _scanAt > TimeSpan.FromSeconds(30) && !Wlan.WiredUp())
                WifiScan();
            if (!_offlineSession && EffectiveMode() == NetMode.OnDemand && State == RadioState.On &&
                DateTime.UtcNow - _lastUse > TimeSpan.FromSeconds(_cfg.IdleSeconds))
                await RequestStop("idle");
            if (DateTime.UtcNow - _probeAt >= TimeSpan.FromSeconds(CardCatalog.ExperimentalBuild ? 5 : 15))
            {
                _probeAt = DateTime.UtcNow;
                try
                {
                    if (CardCatalog.ExperimentalBuild) await ProbeExperimentalAlternates();
                    else await ProbeGateway();
                }
                catch (Exception e) { Say("gateway probe: " + e.Message); }
            }
        }
    }

    async Task<bool> CheckWifiHandoff()
    {
        if ((_handoff is null && _experimentalNetwork is null) || !_launchGate.Wait(0)) return false;
        try
        {
            if (!_wanted) return false;
            if (_experimentalNetwork is not null)
            {
                try { VerifyExperimentalPaths(); }
                catch (Exception e) when (e is InvalidOperationException or System.Net.NetworkInformation.NetworkInformationException)
                {
                    RestoreAfterHandoffFailureLocked("The alternate network adapter lost a required IP path");
                    return true;
                }
            }
            if (_handoff is not { } handoff) return false;
            var paths = WifiPaths.None;
            var toVerify = WifiPaths.None;
            var replies = WifiPaths.None;
            try
            {
                var tap = WifiHandoff.TapAdapter();
                paths = WifiHandoff.Paths(tap);
                toVerify = handoff.GatewaysToVerify(_wifi == "connected", paths);
                if (toVerify != WifiPaths.None)
                    replies = await WifiHandoff.ProbeGateways(tap, toVerify);
            }
            catch (NetworkInformationException e) { Say("Wi-Fi handoff network check: " + e.Message); }
            if (!_wanted) return false;
            bool changed, expired;
            lock (_lock)
            {
                var pending = handoff.Pending;
                expired = handoff.MustRestore(Environment.TickCount64, _wifi == "connected", paths, replies);
                changed = pending != handoff.Pending;
            }
            if (expired)
            {
                var missingReplies = toVerify & ~replies;
                RestoreAfterHandoffFailureLocked(missingReplies != WifiPaths.None
                    ? $"Wi-Fi joined, but the {missingReplies} gateway did not answer through Airdraft"
                    : "Airdraft could not restore the AX210's previous network paths within 25 seconds");
                return true;
            }
            if (changed) Publish();
            return false;
        }
        finally { _launchGate.Release(); }
    }

    bool RestoreAfterHandoffFailure(string why)
    {
        if ((_handoff is null && _experimentalNetwork is null && !_offlineSession) || !_launchGate.Wait(0)) return false;
        try
        {
            if (!_wanted || (_handoff is null && _experimentalNetwork is null && !_offlineSession)) return false;
            RestoreAfterHandoffFailureLocked(why);
            return true;
        }
        finally { _launchGate.Release(); }
    }

    void RestoreAfterHandoffFailureLocked(string why)
    {
        lock (_lock)
        {
            _wanted = false;
            _handoff = null;
            _experimentalNetwork = null;
            _offlineSession = false;
            _wifi = "";
            _channel = 0;
        }
        Say("Wi-Fi rollback: " + why);
        StopDaemons();
        var restored = false;
        try { restored = _radio is null && EnsureVendorDriver(); }
        catch (Exception e) { Say("Wi-Fi rollback could not restore the driver: " + e.Message); }
        BeginNativeWifiRecovery();
        lock (_lock)
        {
            _driverRestoreFailed = !restored;
            State = RadioState.Error;
            _detail = why + (restored ? ". The Windows Wi-Fi driver is active again; wait for Windows to reconnect."
                : ". The driver could not be restored automatically. Run Airdraft - restore Windows Wi-Fi.");
        }
        Publish();
    }

    /* ----------------------------------------------------------- gateway probe */

    DateTime _probeAt = DateTime.MinValue, _connectedAt = DateTime.MaxValue, _scanStartedAt = DateTime.MinValue;
    readonly GatewayProbe[] _gatewayProbes = [new(AddressFamily.InterNetwork), new(AddressFamily.InterNetworkV6)];
    DateTime? _rejoinAt;
    int _probeGeneration = -1, _missingGatewayProbes;

    sealed class GatewayProbe(AddressFamily family)
    {
        public AddressFamily Family { get; } = family;
        public IPAddress? Gateway { get; private set; }
        public int Failures { get; private set; }
        bool _answered;

        public void Record(IPAddress? gateway, bool answered)
        {
            if (!Equals(Gateway, gateway))
            {
                Gateway = gateway;
                Failures = 0;
                _answered = false;
            }
            if (answered)
            {
                _answered = true;
                Failures = 0;
            }
            else if (_answered) Failures++;
        }
    }

    void ResetGatewayProbes()
    {
        _connectedAt = DateTime.MaxValue;
        _missingGatewayProbes = 0;
        _rejoinAt = null;
        foreach (var probe in _gatewayProbes) probe.Record(null, false);
    }

    /// <summary>
    /// The link can look connected while nothing comes back from the access
    /// point (hardware, 2026-11-17: Windows lost the internet with the station
    /// still "connected"). Probe IPv4 and IPv6 independently on the Airdraft
    /// TAP. Four misses trigger a rejoin and eight trigger recovery. Learn
    /// whether each gateway answers ICMP before treating missed replies as failure.
    /// </summary>
    async Task ProbeGateway()
    {
        bool connected, wanted;
        int generation;
        lock (_lock)
        {
            // Not judged when the user turned Wi-Fi off or no station is set up (round 2 #11).
            wanted = _wanted && EffectiveMode() == NetMode.Concurrent && State == RadioState.On && !_userOff && _staSsid != "";
            connected = wanted && _wifi == "connected";
            generation = _launchGen;
        }
        if (_probeGeneration != generation)
        {
            _probeGeneration = generation;
            ResetGatewayProbes();
        }
        if (!wanted)
        {
            ResetGatewayProbes();
            return;
        }
        var now = DateTime.UtcNow;
        if (!connected)
        {
            _connectedAt = DateTime.MaxValue;
            if (_rejoinAt is null) ResetGatewayProbes();
            else if (now - _rejoinAt > TimeSpan.FromSeconds(90) && _launchGate.Wait(0))
                try
                {
                    if (_wanted && _launchGen == generation && _wifi != "connected")
                        RecoverGatewayFailureLocked("Wi-Fi did not recover after rejoining");
                }
                finally { _launchGate.Release(); }
            return;
        }
        if (_connectedAt == DateTime.MaxValue) _connectedAt = now;
        if (now - _connectedAt < TimeSpan.FromSeconds(20)) return;
        var props = WifiHandoff.TapAdapter() is { OperationalStatus: OperationalStatus.Up } tap
            ? tap.GetIPProperties() : null;
        var targets = _gatewayProbes.Select(probe => WifiHandoff.GatewayOn(props, probe.Family)).ToArray();
        var replies = await Task.WhenAll(targets.Select(target => target is { } path ? Task.Run(() =>
        {
            for (var attempt = 0; attempt < 3; attempt++)
                if (Icmp.Ping(path.Source, path.Gateway, 1000)) return true;
            return false;
        }) : Task.FromResult(false)));

        if (!_launchGate.Wait(0)) return;
        try
        {
            if (!_wanted || _launchGen != generation || _wifi != "connected") return;
            _missingGatewayProbes = targets.All(t => t is null) && now - _connectedAt > TimeSpan.FromSeconds(90)
                ? _missingGatewayProbes + 1 : 0;
            for (var i = 0; i < _gatewayProbes.Length; i++)
            {
                var probe = _gatewayProbes[i];
                if (replies[i] && probe.Failures >= 4)
                    Say($"the {(probe.Family == AddressFamily.InterNetwork ? "IPv4" : "IPv6")} gateway answers again");
                probe.Record(targets[i]?.Gateway, replies[i]);
            }
            var failing = _gatewayProbes.MaxBy(probe => probe.Failures)!;
            var failures = Math.Max(_missingGatewayProbes, failing.Failures);
            var why = _missingGatewayProbes > 0 ? "the TAP adapter has no usable IP gateway"
                : $"the {(failing.Family == AddressFamily.InterNetwork ? "IPv4" : "IPv6")} gateway does not answer";
            if (failures >= 8) RecoverGatewayFailureLocked(why + " after rejoining");
            else if (failures >= 4 && _rejoinAt is null)
            {
                Say(why + "; joining Wi-Fi again");
                _rejoinAt = now;
                RadioCommand("rejoin");
            }
            else if (failures == 0) _rejoinAt = null;
        }
        finally { _launchGate.Release(); }
    }

    void RecoverGatewayFailureLocked(string why)
    {
        ResetGatewayProbes();
        if (_handoff is not null) RestoreAfterHandoffFailureLocked(why);
        else
        {
            Say(why + "; restarting the radio");
            LaunchLocked(_channel == 0 ? _cfg.StartChannel : _channel);
        }
    }

    void TakeCard(Card.Info card, NativeWifiReturn? wifiReturn)
    {
        _profile = card.Profile;
        _cardLabel = $"{card.Profile.VendorName} {card.Profile.FamilyName}";
        _caps = card.Profile.Capabilities;
        if (Card.TakenByProxy(card)) return;
        // Only the vendor's INF is the one to give the card back to, never the proxy's (issue #12).
        // Not checked against the vendor's provider names: OEM repackages of
        // Intel's driver carry other names, and refusing them would leave the
        // card with no driver to go back to.
        if (!Card.BoundToProxy(card))
        {
            if (card.Problem != 0 || !TryGetVendorInf(card, card.InfPath, out var vendorInf))
                throw Preflight("the Windows Wi-Fi driver and its recovery INF must be available before taking the adapter");
            // Windows can assign a different WLAN interface GUID when its driver returns.
            if (card.InterfaceId is { } wlanInterface)
            {
                WlanBackup.Restore(Say, TimeSpan.Zero, () => wlanInterface);
                if (WlanBackup.Save(Say, wlanInterface) < 0)
                    throw Preflight("the Windows WLAN profiles could not be backed up");
            }
            lock (_lock)
            {
                _cfg.VendorInf = vendorInf;
                _cfg.VendorInstanceId = card.InstanceId;
                _cfg.VendorDriverReturnPending = true;
                _cfg.PendingWifiReturn = wifiReturn;
                _cfg.Save();
            }
        }
        if (!_wanted) return;
        var inf = Path.Combine(_root, "driver", card.Profile.ProxyInfName);
        if (CardCatalog.ExperimentalBuild && !_offlineSession)
            VerifyExperimentalPaths();
        var reachable = WifiPaths.None;
        if (wifiReturn is not null && !_offlineSession && !CardCatalog.ExperimentalBuild && card.InterfaceId is { } sourceInterface)
        {
            try
            {
                reachable = WifiHandoff.ProbeGateways(WifiHandoff.NativeAdapter(sourceInterface), wifiReturn.RequiredPaths)
                    .GetAwaiter().GetResult();
            }
            catch (NetworkInformationException e) { Say("Native Wi-Fi gateway check: " + e.Message); }
            Say($"Wi-Fi handoff: native IP paths {wifiReturn.RequiredPaths}, reachable gateways {reachable}");
        }
        if (!CardCatalog.ExperimentalBuild && !_offlineSession && !Card.BoundToProxy(card) && card.InterfaceId is { } nativeInterface)
        {
            var (current, alternatives) = WifiHandoff.CurrentPaths(nativeInterface);
            if (WifiHandoff.WouldInterruptWifi(current, alternatives, Wlan.IsConnecting(nativeInterface, includeConnected: true)))
                throw new WifiInUseException();
        }
        if (!_wanted) return;
        if (wifiReturn is not null && !_offlineSession && !CardCatalog.ExperimentalBuild)
            lock (_lock) _handoff = new WifiHandoff(wifiReturn.RequiredPaths, Environment.TickCount64, reachable);
        Say($"taking the card from {Path.GetFileName(card.InfPath)}");
        var rebootRequired = false;
        try
        {
            rebootRequired = ChangeDriver(card, inf, restoring: false);
            if (rebootRequired)
                throw new CardBindException("Windows requested a restart during the radio driver switch. Run Airdraft - restore Windows Wi-Fi before restarting Windows.", new InvalidOperationException());
        }
        catch (CardBindException e)
        {
            // A failed bind can leave the card with no driver at all, and so
            // Windows without Wi-Fi (issue #3): give it straight back.
            Say("taking the card failed: " + e.Message);
            EnsureVendorDriver(force: rebootRequired);
            throw;
        }
        // The proxy's device interface appears once the UMDF host has started.
        for (int i = 0; i < 300; i++)
        {
            if (!_wanted) return;
            if (_handoff?.Expired(Environment.TickCount64) == true) break;
            var now = Card.Find();
            if (now is { Problem: 0 } && string.Equals(now.InstanceId, card.InstanceId, StringComparison.OrdinalIgnoreCase) &&
                Card.TakenByProxy(now)) return;
            if (CardCatalog.ExperimentalBuild && !_offlineSession)
                VerifyExperimentalPaths();
            Thread.Sleep(100);
        }
        // Bound but never started (issue #12: Windows 10, then "register proxy not
        // openable (error 259)" in a loop with the card held): say why, give it back.
        var after = Card.Find();
        var state = after is null ? "the card disappeared"
            : $"driver service '{after.Service}', provider '{after.Provider}', INF {Path.GetFileName(after.InfPath)}, " +
              $"Device Manager problem code {after.Problem}";
        Say("the register proxy did not start: " + state);
        EnsureVendorDriver();
        throw new CardBindException($"the register proxy was bound but did not start ({state})", new TimeoutException());
    }

    /* ------------------------------------------------------- Wi-Fi management */

    static readonly string[] Joinable = ["open", "owe", "wpa2-psk", "wpa2/3-psk", "wpa3-sae"];

    void RadioCommand(string line)
    {
        try
        {
            if (_wanted && _radio is { HasExited: false } r)
            {
                r.StandardInput.WriteLine(line);
                r.StandardInput.Flush();
            }
        }
        catch (Exception) { }
    }

    /// <summary>Refreshes the list; `ssid` makes it a directed scan (hidden networks).</summary>
    public void WifiScan(string? ssid = null)
    {
        if (State != RadioState.On) return;
        lock (_lock)
        {
            _scanPending = [];
            _scanBusy = true;
            _scanStartedAt = DateTime.UtcNow;
        }
        RadioCommand(string.IsNullOrEmpty(ssid) ? "scan" : "scan " + Convert.ToHexString(System.Text.Encoding.UTF8.GetBytes(ssid)));
        Publish();
    }

    /// <summary>Joins `ssid` (from the last scan). Returns "" or why not.</summary>
    public string WifiConnect(string ssid, string? password, bool? autoConnect, bool hidden)
    {
        // Only Concurrent runs a station beside AirDrop. In On demand / Off the
        // relaunch below joined nothing and only restarted the radio, which
        // reset discovery each time the user tried again (#11).
        if (EffectiveMode() != NetMode.Concurrent) return "needs concurrent mode";
        JsonObject? target;
        bool stale;
        lock (_lock)
        {
            target = _scan.FirstOrDefault(n => (string?)n["ssid"] == ssid);
            stale = DateTime.UtcNow - _scanAt > StaleScan;
        }
        // An iPhone hotspot comes back with a new BSSID: joining the address
        // from an old scan sent every authentication into the void until a later
        // scan happened to catch the new one (#27). Look by name again first.
        if (target is not null && stale && !_scanBusy && State == RadioState.On)
        {
            lock (_lock) _pendingConnect = (ssid, password, autoConnect);
            WifiScan(ssid);
            return "";
        }
        if (target is null)
        {
            if (!hidden && _scanBusy) return "still scanning; try again in a moment";
            // Not in the list (hidden, or out of the last scan): look for it by name first.
            lock (_lock) _pendingConnect = (ssid, password, autoConnect);
            WifiScan(ssid);
            return "";
        }
        var security = (string?)target["security"] ?? "";
        if (!Joinable.Contains(security))
            return security switch
            {
                "802.1x" => "enterprise (802.1X) networks are not supported yet",
                "wep" => "WEP networks are not supported",
                _ => "this network's security is not supported",
            };
        // Enhanced Open (OWE) has no password: the radio sees it in the beacon and encrypts.
        var open = security is "open" or "owe";
        string? pass = null, psk = null;
        if (!open)
        {
            if (!string.IsNullOrEmpty(password))
            {
                if (password.Length == 64 && password.All(Uri.IsHexDigit)) psk = password;
                else if (password.Length is >= 8 and <= 63) pass = password;
                else return "the password must be 8 to 63 characters";
            }
            else if (_known.Find(ssid) is { HasKey: true } k)
            {
                pass = k.Passphrase;
                psk = k.PskHex;
            }
            else return "password needed";
            // SAE works from the password itself; a stored 64-digit key cannot do WPA3.
            if (security == "wpa3-sae" && pass is null) return "WPA3 needs the Wi-Fi password itself, not a 64-digit key";
        }
        _known.Remember(ssid, pass, psk, open, autoConnect);
        var net = new Wlan.Network(ssid, (string?)target["bssid"] ?? "", (int?)target["channel"] ?? 0, security, pass, psk, open);
        lock (_lock)
        {
            _net = net;
            _userChose = true;
            _userOff = false;
            _wifiError = "";
            _staSsid = ssid;
        }
        Say($"Wi-Fi: joining \"{ssid}\" (chosen in Airdraft)");
        _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
        return "";
    }

    public void WifiDisconnect()
    {
        lock (_lock)
        {
            _userOff = true;
            _userChose = false;
            _net = null;
            _staSsid = "";
            _wifi = "";
        }
        Say("Wi-Fi: disconnected by the user");
        Publish();
        _ = Task.Run(() => Relaunch(_channel == 0 ? _cfg.StartChannel : _channel));
    }

    public void WifiForget(string ssid)
    {
        _known.Forget(ssid);
        if (ssid == _staSsid) WifiDisconnect();
        else Publish();
    }

    public void WifiAuto(string ssid, bool on)
    {
        _known.SetAutoConnect(ssid, on);
        Publish();
    }

    /// <summary>After a scan: a pending connect by name, or auto-connect when Wi-Fi is down.</summary>
    void AfterScan()
    {
        (string Ssid, string? Password, bool? Auto)? pending;
        lock (_lock)
        {
            pending = _pendingConnect;
            _pendingConnect = null;
        }
        if (pending is { } pc)
        {
            bool found;
            lock (_lock) found = _scan.Any(n => (string?)n["ssid"] == pc.Ssid);
            var why = found ? WifiConnect(pc.Ssid, pc.Password, pc.Auto, false) : $"\"{pc.Ssid}\" was not found";
            if (why != "") lock (_lock) _wifiError = why;
            Publish();
            return;
        }
        if (_userOff || EffectiveMode() != NetMode.Concurrent || _wifi == "connected") return;
        if (DateTime.UtcNow - _wifiDownSince < TimeSpan.FromSeconds(20)) return;
        // Beside a wired connection Wi-Fi is joined only from the Wi-Fi window (issue #1).
        if (Wlan.WiredUp()) return;
        JsonObject? best;
        lock (_lock)
            best = _scan.Where(n => Joinable.Contains((string?)n["security"] ?? "") &&
                                    _known.Find((string?)n["ssid"] ?? "") is { HasKey: true, AutoConnect: true })
                        .OrderByDescending(n => (int?)n["dbm"] ?? -127).FirstOrDefault();
        if (best is null) return;
        var ssid = (string?)best["ssid"] ?? "";
        Say($"Wi-Fi: auto-connecting to \"{ssid}\"");
        WifiConnect(ssid, null, null, false);
    }

    /// <summary>
    /// The "Airdraft" adapter after any wired one: with the automatic metric both
    /// got 25 and traffic could leave through Wi-Fi (issue #1).
    /// </summary>
    void SetTapMetric()
    {
        foreach (var family in new[] { "ipv4", "ipv6" })
        {
            try
            {
                var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "netsh.exe"))
                {
                    UseShellExecute = false, CreateNoWindow = true,
                    RedirectStandardOutput = true, RedirectStandardError = true,
                };
                foreach (var a in new[] { "interface", family, "set", "interface", "Airdraft", $"metric={TapMetric}" })
                    psi.ArgumentList.Add(a);
                using var p = Process.Start(psi);
                if (p is null) continue;
                // Read before waiting: a full pipe would block netsh.
                var output = p.StandardOutput.ReadToEndAsync();
                var errors = p.StandardError.ReadToEndAsync();
                p.WaitForExit(10000);
                if (p is { HasExited: true, ExitCode: not 0 })
                {
                    // What netsh said, so the log tells a missing adapter from a refusal (issue #3).
                    var why = string.Join(" ", (output.Wait(1000) ? output.Result : "") + " " + (errors.Wait(1000) ? errors.Result : ""))
                        .Replace("\r", " ").Replace("\n", " ").Trim();
                    Say($"could not set the Airdraft adapter's {family} metric (netsh exit {p.ExitCode}): {why}");
                }
            }
            catch (Exception e) { Say("could not set the Airdraft adapter's metric: " + e.Message); }
        }
    }

    /// <summary>
    /// High enough that wired Ethernet always wins the default route: Windows
    /// often assigns RouteMetric 256 + InterfaceMetric 25 = 281 to Ethernet,
    /// which lost to Airdraft's earlier 75 + 75 = 150 (issue #11).
    /// </summary>
    const int TapMetric = 500;

    // The launch gate remains held across the Windows call, including slow driver switches.
    bool ChangeDriver(Card.Info card, string inf, bool restoring)
    {
        if (Card.Find() is not { } current || !string.Equals(current.InstanceId, card.InstanceId, StringComparison.OrdinalIgnoreCase))
            throw new CardBindException("The expected Wi-Fi adapter changed before the driver switch", new InvalidOperationException());
        lock (_lock) _driverChange = restoring ? "restoring" : "taking";
        Publish();
        var timer = Stopwatch.StartNew();
        try { return Card.Bind(card.HardwareId, inf); }
        finally
        {
            Say($"Windows finished the {(restoring ? "restore" : "takeover")} driver request after {timer.Elapsed.TotalSeconds:F1} s");
            lock (_lock) _driverChange = "";
            Publish();
        }
    }

    bool EnsureVendorDriver(bool force = false)
    {
        var card = Card.Find();
        // Ours (running or not), or left with no driver by a failed bind: the vendor's driver goes back on.
        if (card is null) return false;
        if (!force && !Card.BoundToProxy(card) && card.InfPath != "")
            return card.Problem == 0 && ConfirmVendorDriverReturn(card);
        var vendor = card.Profile.VendorName;
        if ((_cfg.VendorInstanceId.Length != 0 || CardCatalog.ExperimentalBuild) &&
            !string.Equals(_cfg.VendorInstanceId, card.InstanceId, StringComparison.OrdinalIgnoreCase))
        {
            Say($"the recorded {vendor} driver belongs to another device instance; run restore.cmd");
            return false;
        }
        if (!TryGetVendorInf(card, _cfg.VendorInf, out var fullInf))
        {
            Say($"cannot give the card back: a usable {vendor} recovery INF is unavailable; run restore.cmd");
            return false;
        }
        try
        {
            Say($"giving the card back to {Path.GetFileName(fullInf)}");
            if (ChangeDriver(card, fullInf, restoring: true))
            {
                Say("Windows requested a restart to finish restoring the vendor driver; run restore.cmd to confirm the binding");
                return false;
            }
            // Once the vendor driver's WLAN interface is up, put back the saved networks it lacks.
            _ = Task.Run(() => WlanBackup.Restore(Say, TimeSpan.FromSeconds(60), () => Card.VendorWlanInterface(card.InstanceId)));
            for (var i = 0; i < 20; i++)
            {
                if (Card.Find() is { Problem: 0 } current &&
                    string.Equals(current.InstanceId, card.InstanceId, StringComparison.OrdinalIgnoreCase) &&
                    string.Equals(current.InfPath, fullInf, StringComparison.OrdinalIgnoreCase))
                    return !Card.BoundToProxy(current) && ConfirmVendorDriverReturn(current);
                Thread.Sleep(250);
            }
            Say("Windows did not report a working vendor driver after the bind");
        }
        catch (Exception e) { Say("giving the card back failed: " + e.Message); }
        return false;
    }

    bool ConfirmVendorDriverReturn(Card.Info card)
    {
        lock (_lock)
        {
            if (!_cfg.VendorDriverReturnPending) return true;
            if (!string.Equals(card.InstanceId, _cfg.VendorInstanceId, StringComparison.OrdinalIgnoreCase)) return false;
            _cfg.VendorDriverReturnPending = false;
            try { _cfg.Save(); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                _cfg.VendorDriverReturnPending = true;
                Say("could not save confirmation that the Wi-Fi driver returned: " + e.Message);
                return false;
            }
            return true;
        }
    }

    static bool TryGetVendorInf(Card.Info card, string path, out string fullInf)
    {
        fullInf = "";
        if (!File.Exists(path)) return false;
        fullInf = Path.GetFullPath(path);
        var infDir = Path.GetFullPath(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "INF"));
        var infRoot = infDir.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (!fullInf.StartsWith(infRoot, StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(Path.GetDirectoryName(fullInf), infDir.TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase) ||
            !System.Text.RegularExpressions.Regex.IsMatch(Path.GetFileName(fullInf), @"^oem\d+\.inf$", System.Text.RegularExpressions.RegexOptions.IgnoreCase) ||
            (File.GetAttributes(fullInf) & FileAttributes.ReparsePoint) != 0)
            return false;
        // An earlier build recorded the proxy's own INF as Intel's (issue #12):
        // binding it again would keep Windows without Wi-Fi.
        try
        {
            return !File.ReadAllText(fullInf).Contains(card.Profile.ProxyInfStem, StringComparison.OrdinalIgnoreCase);
        }
        catch (IOException) { return false; }
    }

    Process Spawn(string exe, IEnumerable<string> args, Action<string> onLine, bool supervisedRadio = false)
    {
        var psi = new ProcessStartInfo(exe)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
            RedirectStandardInput = true,
            WorkingDirectory = Path.GetDirectoryName(exe)!,
        };
        if (supervisedRadio && SupportsRadioEof) psi.Environment["AIRDRAFT_SUPERVISED_RADIO"] = "1";
        foreach (var a in args) psi.ArgumentList.Add(a);
        var p = Process.Start(psi) ?? throw new InvalidOperationException("cannot start " + exe);
        p.ErrorDataReceived += (_, e) => { if (e.Data is not null) onLine(e.Data); };
        p.OutputDataReceived += (_, e) => { if (e.Data is not null) onLine(e.Data); };
        p.BeginErrorReadLine();
        p.BeginOutputReadLine();
        return p;
    }

    bool SupportsRadioEof => _profile.Backend is RadioBackendKind.Iwx or RadioBackendKind.Devourer8852be;

    bool StopDaemons()
    {
        // Both radios close the AWDL link during teardown. Give awdld time to
        // exit on that disconnect and release its own adapter.
        var radioStopped = StopDaemon(ref _radio, "radio", SupportsRadioEof ? 4000 : 0, closeInput: SupportsRadioEof);
        var awdlStopped = StopDaemon(ref _awdld, "AWDL daemon", radioStopped ? 1000 : 0);
        return radioStopped && awdlStopped;
    }

    static bool DaemonExited(Process? daemon)
    {
        try { return daemon is null || daemon.HasExited; }
        catch (InvalidOperationException) { return true; } // Already detached from the service.
        catch (System.ComponentModel.Win32Exception) { return false; }
    }

    bool StopDaemon(ref Process? daemon, string name, int graceMs, bool closeInput = false)
    {
        if (daemon is not { } process) return true;
        try
        {
            if (!DaemonExited(process))
            {
                if (closeInput)
                {
                    try { process.StandardInput.BaseStream.Close(); }
                    catch (IOException e) { Say($"could not close {name} control pipe: {e.Message}"); }
                }
                if (graceMs > 0 && process.WaitForExit(graceMs))
                    Say($"{name} exited after shutdown request");
                else
                {
                    Say($"terminating {name} after shutdown wait");
                    process.Kill();
                    process.WaitForExit(5000);
                }
            }
        }
        catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            if (!DaemonExited(process)) Say($"could not stop {name}: {e.Message}");
        }
        if (!DaemonExited(process)) return false;
        daemon = null;
        process.Dispose();
        return true;
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        _cts.Cancel();
        RequestStop("service stopping").GetAwaiter().GetResult();
    }
}
