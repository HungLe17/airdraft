// SPDX-License-Identifier: GPL-2.0-only
namespace Airdraft.Service;

/// <summary>What the service knows when it starts a card's radio daemon.</summary>
/// <param name="Profile">the card (CardCatalog): IDs, capabilities, and RadioFamily when chosen by hand</param>
/// <param name="Channel">the AWDL channel to start on (6, 44 or 149)</param>
/// <param name="Kernel">nekodrop-wlan.sys owns the card (Intel only)</param>
/// <param name="Country">two letters learned from a peer or a network, "" if none</param>
/// <param name="StaConf">the Wi-Fi station's config file (Concurrent mode), or null</param>
/// <param name="Aggregate">A-MPDU sending (always true from the service since 0.1.35; the radio
/// skips it by itself without DMA room for the extra ring)</param>
/// <param name="FastUnlock">experimental fast passive-channel unlock is on in the settings</param>
/// <param name="CoexPlan">experimental coexistence plan (router entries in the AWDL schedule) is on</param>
/// <param name="BtCoexBalanced">experimental balanced Bluetooth coexistence is on in the settings</param>
public sealed record RadioStart(AdapterProfile Profile, int Channel, bool Kernel, string Country, string? StaConf,
                                bool Aggregate, bool FastUnlock, bool CoexPlan = false, bool BtCoexBalanced = false)
{
    /// <summary>20 MHz on 2.4 GHz, 80 MHz on 5 GHz.</summary>
    public int Width => Channel <= 14 ? 20 : 80;
}

/// <summary>
/// One user-mode radio daemon: how the service starts it. Every daemon speaks
/// winawdl's radio pipe (\\.\pipe\winawdl-radio, radio_link.h) to awdld, and
/// talks to the card through the register proxy (driver/regproxy), so the rest
/// of the service is the same for every card. Adding a card family:
/// docs/ADDING-A-CARD.md.
/// </summary>
public interface IRadioBackend
{
    RadioBackendKind Kind { get; }
    /// <summary>The daemon's file name in the install folder's radio\ directory.</summary>
    string ExeName { get; }
    /// <summary>Its command line. Pass only options this daemon accepts: unknown ones make winawdl's exit with status 2.</summary>
    IReadOnlyList<string> BuildArgs(RadioStart s);
}

/// <summary>The backends the service can start, by kind.</summary>
public static class RadioBackends
{
    static readonly Dictionary<RadioBackendKind, IRadioBackend> All = new IRadioBackend[]
    {
        new IwxRadioBackend(),
        new DevourerRadioBackend(),
        new Rtl8852beRadioBackend(),
    }.ToDictionary(b => b.Kind);

    public static IRadioBackend Get(RadioBackendKind kind) =>
        All.TryGetValue(kind, out var b) ? b : throw new NotSupportedException($"no radio backend for {kind}");

    public static IEnumerable<IRadioBackend> Registered => All.Values;
}

/// <summary>
/// Intel 22000 / AX210 families: airdraft-radio.exe (iwxradio, ported from
/// ForbiddenItlwm). Station, A-MPDU, kernel driver; faster sending (64 frames
/// in flight) always on since 0.1.25; --family when the model was chosen by hand.
/// </summary>
public sealed class IwxRadioBackend : IRadioBackend
{
    public RadioBackendKind Kind => RadioBackendKind.Iwx;
    public string ExeName => "airdraft-radio.exe";

    public IReadOnlyList<string> BuildArgs(RadioStart s)
    {
        var args = new List<string> { s.Channel.ToString(), "--width", s.Width.ToString() };
        if (s.Kernel) args.Add("--kernel");
        if (!string.IsNullOrEmpty(s.Country)) args.AddRange(["--country", s.Country]);
        if (s.StaConf is not null) args.AddRange(["--sta-config", s.StaConf]);
        var caps = s.Profile.Capabilities;
        if (!s.Kernel && caps.SupportsFastSend) args.AddRange(["--tx-window", "64"]);
        if (s.Aggregate && !s.Kernel && caps.SupportsAggregate) args.Add("--aggregate");
        // The firmware scan and country notice it needs are the user-mode Intel radio's,
        // the radio that also runs the Wi-Fi station; not with nekodrop-wlan.sys.
        if (s.FastUnlock && !s.Kernel && caps.SupportsConcurrentWifi) args.Add("--fast-unlock");
        // Only the user-mode Intel radio runs the station whose channel the plan names.
        if (s.CoexPlan && !s.Kernel && caps.SupportsConcurrentWifi) args.Add("--coex-plan");
        // Balanced Wi-Fi/Bluetooth arbitration in the firmware the user-mode radio boots;
        // nekodrop-wlan.sys keeps its own.
        if (s.BtCoexBalanced && !s.Kernel) args.AddRange(["--bt-coex", "1"]);
        if (!s.Kernel && s.Profile.RadioFamily is { Length: > 0 } family) args.AddRange(["--family", family]);
        return args;
    }
}

/// <summary>
/// Realtek RTL8852CE: winawdl's radio (winawdl_radio.exe on raph559/devourer's
/// Windows PCIe backend), shipped as airdraft-radio-rtl.exe. Monitor and
/// inject only, no station: CardCatalog runs it on demand.
/// </summary>
public sealed class DevourerRadioBackend : IRadioBackend
{
    public RadioBackendKind Kind => RadioBackendKind.Devourer;
    public string ExeName => "airdraft-radio-rtl.exe";

    public IReadOnlyList<string> BuildArgs(RadioStart s) => [s.Channel.ToString(), "--width", s.Width.ToString()];
}

internal sealed class Rtl8852beRadioBackend : IRadioBackend
{
    public RadioBackendKind Kind => RadioBackendKind.Devourer8852be;
    public string ExeName => "airdraft-radio-rtl8852be.exe";

    public IReadOnlyList<string> BuildArgs(RadioStart s) =>
        [s.Channel.ToString(), "--width", "20", "--no-block-ack", "--no-aggregate", "--no-quick-submit"];
}
