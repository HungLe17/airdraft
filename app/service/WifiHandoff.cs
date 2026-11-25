// Copyright (c) 2026 Airdraft
// SPDX-License-Identifier: GPL-2.0-only
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;

namespace Airdraft.Service;

[Flags]
internal enum WifiPaths { None = 0, IPv4 = 1, IPv6 = 2 }

/// <summary>Tracks how long Windows has been without the AX210's previous default paths.</summary>
internal sealed class WifiHandoff(WifiPaths required, long startedAt, WifiPaths reachableGateways = WifiPaths.None)
{
    internal const long RecoveryLimitMs = 25_000;
    long? _missingSince = startedAt;

    public bool Pending => _missingSince.HasValue;
    public bool Expired(long now) => _missingSince is { } since && now - since >= RecoveryLimitMs;

    public WifiPaths GatewaysToVerify(bool stationConnected, WifiPaths tapPaths) =>
        Pending && stationConnected && (tapPaths & required) == required ? reachableGateways : WifiPaths.None;

    public bool MustRestore(long now, bool stationConnected, WifiPaths tapPaths, WifiPaths gatewayReplies = WifiPaths.None)
    {
        if (stationConnected && (tapPaths & required) == required &&
            (!Pending || (gatewayReplies & reachableGateways) == reachableGateways))
        {
            _missingSince = null;
            return false;
        }
        _missingSince ??= now;
        return Expired(now);
    }

    public static bool WouldInterruptWifi(WifiPaths original, WifiPaths alternative, bool connected) =>
        (original & ~alternative) != WifiPaths.None || (connected && alternative == WifiPaths.None);

    public static bool HasAlternatePaths(WifiPaths required, WifiPaths alternative) =>
        alternative != WifiPaths.None && (required & ~alternative) == WifiPaths.None;

    const string TapName = "Airdraft";
    static bool IsAirdraftTap(NetworkInterface adapter) =>
        adapter.Name == TapName && adapter.Description.Contains("TAP-Windows", StringComparison.OrdinalIgnoreCase);

    public static NetworkInterface? TapAdapter() => NetworkInterface.GetAllNetworkInterfaces().FirstOrDefault(IsAirdraftTap);
    public static bool HasTap() => TapAdapter() is not null;

    internal static NetworkInterface? NativeAdapter(Guid interfaceId) => NetworkInterface.GetAllNetworkInterfaces()
        .FirstOrDefault(adapter => Guid.TryParse(adapter.Id, out var id) && id == interfaceId);

    public static WifiPaths NativePaths(Guid interfaceId) => Paths(NativeAdapter(interfaceId));

    public static (WifiPaths Original, WifiPaths Alternative) CurrentPaths(Guid wifiInterface)
    {
        var original = WifiPaths.None;
        var alternative = WifiPaths.None;
        foreach (var adapter in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (Guid.TryParse(adapter.Id, out var id) && id == wifiInterface)
                original = Paths(adapter);
            else if (IsPhysicalAlternate(adapter))
                alternative |= Paths(adapter);
        }
        return (original, alternative);
    }

    static readonly string[] VirtualAdapters = ["Virtual", "Wi-Fi Direct", "TAP", "TUN", "WireGuard", "VMware", "Loopback", "VPN", "WAN Miniport"];

    static bool IsPhysicalAlternate(NetworkInterface adapter) =>
        adapter.NetworkInterfaceType is NetworkInterfaceType.Wireless80211 or
            NetworkInterfaceType.Ethernet or NetworkInterfaceType.GigabitEthernet or
            NetworkInterfaceType.FastEthernetT or NetworkInterfaceType.FastEthernetFx &&
        !IsAirdraftTap(adapter) &&
        !VirtualAdapters.Any(name => adapter.Description.Contains(name, StringComparison.OrdinalIgnoreCase));

    public static Task<WifiPaths> ProbeAlternateGateways(Guid excludedInterface, WifiPaths paths) =>
        ProbeGateways(NetworkInterface.GetAllNetworkInterfaces().Where(adapter => IsPhysicalAlternate(adapter) &&
            (!Guid.TryParse(adapter.Id, out var id) || id != excludedInterface)), paths);

    public static Task<WifiPaths> ProbeGateways(NetworkInterface? adapter, WifiPaths paths) =>
        adapter is null ? Task.FromResult(WifiPaths.None) : ProbeGateways([adapter], paths);

    static async Task<WifiPaths> ProbeGateways(IEnumerable<NetworkInterface> adapters, WifiPaths paths)
    {
        var targets = new List<(IPAddress Source, IPAddress Gateway, WifiPaths Family)>();
        foreach (var adapter in adapters.Where(adapter => adapter.OperationalStatus == OperationalStatus.Up))
        {
            var ip = adapter.GetIPProperties();
            if (paths.HasFlag(WifiPaths.IPv4) && GatewayOn(ip, AddressFamily.InterNetwork) is { } ipv4)
                targets.Add((ipv4.Source, ipv4.Gateway, WifiPaths.IPv4));
            if (paths.HasFlag(WifiPaths.IPv6) && GatewayOn(ip, AddressFamily.InterNetworkV6) is { } ipv6)
                targets.Add((ipv6.Source, ipv6.Gateway, WifiPaths.IPv6));
        }
        var probes = targets.Select(target => Task.Run(() => Icmp.Ping(target.Source, target.Gateway, 1000) ? target.Family : WifiPaths.None));
        var reachable = WifiPaths.None;
        foreach (var reply in await Task.WhenAll(probes)) reachable |= reply;
        return reachable;
    }

    internal static WifiPaths Paths(NetworkInterface? adapter)
    {
        if (adapter is not { OperationalStatus: OperationalStatus.Up }) return WifiPaths.None;
        var ip = adapter.GetIPProperties();
        var paths = WifiPaths.None;
        foreach (var gateway in ip.GatewayAddresses.Select(g => g.Address))
        {
            if (gateway.Equals(IPAddress.Any) || gateway.Equals(IPAddress.IPv6Any)) continue;
            var hasAddress = ip.UnicastAddresses.Any(u => u.Address.AddressFamily == gateway.AddressFamily &&
                u.DuplicateAddressDetectionState == DuplicateAddressDetectionState.Preferred && UsableSource(u.Address));
            if (hasAddress)
                paths |= gateway.AddressFamily == AddressFamily.InterNetworkV6 ? WifiPaths.IPv6 : WifiPaths.IPv4;
        }
        return paths;
    }

    static bool UsableSource(IPAddress address)
    {
        if (IPAddress.IsLoopback(address) || address.Equals(IPAddress.Any) || address.Equals(IPAddress.IPv6Any))
            return false;
        if (address.AddressFamily == AddressFamily.InterNetworkV6) return !address.IsIPv6LinkLocal;
        var octets = address.GetAddressBytes();
        return octets[0] != 169 || octets[1] != 254;
    }

    public static (IPAddress Source, IPAddress Gateway)? GatewayOn(IPInterfaceProperties? ip, AddressFamily family)
    {
        if (ip is null) return null;
        var preferred = ip.UnicastAddresses.Where(u => u.Address.AddressFamily == family &&
            u.DuplicateAddressDetectionState == DuplicateAddressDetectionState.Preferred).Select(u => u.Address);
        var source = preferred.FirstOrDefault(UsableSource);
        var gateway = ip.GatewayAddresses.Select(g => g.Address).FirstOrDefault(a => a.AddressFamily == family &&
            !a.Equals(IPAddress.Any) && !a.Equals(IPAddress.IPv6Any));
        if (source is null || gateway is null) return null;
        if (!gateway.IsIPv6LinkLocal) return (source, gateway);

        var index = ip.GetIPv6Properties()?.Index ?? 0;
        if (index <= 0) return null;
        var local = preferred.FirstOrDefault(a => a.IsIPv6LinkLocal);
        return (local is null ? source : new IPAddress(local.GetAddressBytes(), index), new IPAddress(gateway.GetAddressBytes(), index));
    }

    public static IReadOnlyList<string> SelfTest()
    {
        var failures = new List<string>();
        var dualStack = WifiPaths.IPv4 | WifiPaths.IPv6;
        if (!WouldInterruptWifi(dualStack, WifiPaths.IPv4, true) ||
            WouldInterruptWifi(dualStack, dualStack, true) ||
            !WouldInterruptWifi(WifiPaths.IPv4, WifiPaths.None, false) ||
            !WouldInterruptWifi(WifiPaths.None, WifiPaths.None, true) ||
            WouldInterruptWifi(WifiPaths.None, WifiPaths.None, false))
            failures.Add("Wi-Fi handoff must preserve the connected station's existing network paths");
        if (HasAlternatePaths(WifiPaths.None, WifiPaths.None) || HasAlternatePaths(dualStack, WifiPaths.IPv4) ||
            !HasAlternatePaths(WifiPaths.IPv4, dualStack))
            failures.Add("An exclusive radio session must have an alternate adapter covering the required IP paths");

        var guard = new WifiHandoff(dualStack, 0);
        if (guard.MustRestore(RecoveryLimitMs - 1, true, WifiPaths.IPv4) || !guard.Pending ||
            !guard.MustRestore(RecoveryLimitMs, true, WifiPaths.IPv4))
            failures.Add("Association without both required IP paths must expire");
        guard = new WifiHandoff(WifiPaths.IPv4, 0);
        if (guard.MustRestore(5000, false, WifiPaths.IPv4) || !guard.Pending ||
            guard.MustRestore(6000, true, dualStack) || guard.Pending ||
            guard.MustRestore(7000, false, WifiPaths.None) ||
            !guard.MustRestore(7000 + RecoveryLimitMs, false, WifiPaths.None))
            failures.Add("A restored path must clear the timer and a later loss must start a fresh timer");
        guard = new WifiHandoff(dualStack, 0, WifiPaths.IPv4);
        if (guard.GatewaysToVerify(true, WifiPaths.IPv4) != WifiPaths.None ||
            guard.GatewaysToVerify(true, dualStack) != WifiPaths.IPv4 ||
            guard.MustRestore(5000, true, dualStack, WifiPaths.IPv6) || !guard.Pending ||
            guard.MustRestore(6000, true, dualStack, WifiPaths.IPv4) || guard.Pending ||
            guard.MustRestore(7000, true, dualStack) || guard.Pending)
            failures.Add("Handoff must verify only the gateways that answered on the native connection");
        guard = new WifiHandoff(dualStack, 0, dualStack);
        if (guard.MustRestore(RecoveryLimitMs - 1, true, dualStack, WifiPaths.IPv4) || !guard.Pending ||
            !guard.MustRestore(RecoveryLimitMs, true, dualStack, WifiPaths.IPv4))
            failures.Add("A previously reachable gateway must be verified before the handoff deadline");
        var local = new NativeWifiReturn("card", "Saved WLAN", false, false);
        if (!local.NeedsRecovery || local.IsRestored(WifiPaths.None, connected: false) ||
            !local.IsRestored(WifiPaths.None, connected: true) ||
            (local with { ProfileName = "" }).NeedsRecovery)
            failures.Add("A saved LAN-only Wi-Fi connection must reconnect even without a default route");
        return failures;
    }
}
