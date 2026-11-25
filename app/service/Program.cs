// SPDX-License-Identifier: GPL-2.0-only
// AirdraftRadioService: runs as LocalSystem, so taking the Wi-Fi card, locking DMA
// memory, creating the AWDL adapter and reading the Wi-Fi profile need no UAC.
using Airdraft.Service;

// --selftest: checks the card catalog offline (no card, registry or administrator
// rights), so scripts\build-installer.ps1 runs it on the build machine.
if (args.Contains("--selftest"))
{
    var failures = CardCatalog.SelfTest().Concat(WifiHandoff.SelfTest()).ToArray();
    foreach (var f in failures) Console.Error.WriteLine("FAIL: " + f);
    Console.WriteLine(failures.Length == 0
        ? $"Card catalog and Wi-Fi handoff self-tests passed ({CardCatalog.AllProfiles.Count} profiles, {CardCatalog.ActiveProfiles.Count} active)."
        : $"Service self-test: {failures.Length} failure(s)");
    Environment.ExitCode = failures.Length == 0 ? 0 : 1;
    return;
}

// The card the user matched to a model by hand, for every path below that looks for the card.
{
    var manual = ServiceConfig.Load();
    Card.Manual = (manual.ManualCard, manual.ManualModel);
}

// --give-back: hand the card to the vendor's driver (Intel's today) and exit (uninstall, recovery).
if (args.Contains("--give-back"))
{
    using var s = new Supervisor();
    s.RequestStop("--give-back").GetAwaiter().GetResult();
    if (s.State == Supervisor.RadioState.Error)
    {
        Console.Error.WriteLine("Windows did not confirm that it has a working Wi-Fi driver. Run Airdraft - restore Windows Wi-Fi.");
        Environment.ExitCode = 1;
        return;
    }
    // The process ends here: wait for the vendor driver's interface and restore in line.
    WlanBackup.Restore(Console.WriteLine, TimeSpan.FromSeconds(60), () => Card.VendorWlanInterface());
    if (!s.WaitForNativeWifiRecovery())
    {
        Console.Error.WriteLine("The native Wi-Fi adapter is still missing its previous IP paths.");
        Environment.ExitCode = 1;
    }
    return;
}

// --status: what the service would work with (no keys are read or printed).
if (args.Contains("--status"))
{
    var card = Card.Find();
    var target = CardCatalog.ExperimentalBuild ? "RTL8852BE" : "AX210";
    Console.WriteLine(card is null ? $"card: exactly one {target} is required"
        : $"card: {card.InstanceId}{Environment.NewLine}  driver service {card.Service}, INF {card.InfPath}, taken by Airdraft: {Card.TakenByProxy(card)}");
    if (card is not null)
    {
        var p = card.Profile;
        var c = p.Capabilities;
        Console.WriteLine($"  profile: {p.VendorName} {p.FamilyName} ({p.VendorId}:{p.DeviceId}), radio {p.Backend}, " +
                          $"concurrent Wi-Fi {c.SupportsConcurrentWifi}, register BAR >= 0x{c.MinBarBytes:x}");
        // Whether the bound driver looks like the vendor's own (only informative:
        // the service records any non-proxy driver to give the card back to).
        Console.WriteLine($"  bound driver provider \"{card.Provider}\": " +
                          (Card.BoundToProxy(card) ? "Airdraft's register proxy"
                           : CardCatalog.IsVendorProviderMatch(p, card.Provider) ? $"{p.VendorName}'s driver" : "not recognised as the vendor's driver"));
        if (!Card.BoundToProxy(card) && card.InterfaceId is { } wifiInterface)
        {
            var paths = WifiHandoff.CurrentPaths(wifiInterface);
            Console.WriteLine($"  network paths: {target} {paths.Original}; other physical adapters {paths.Alternative}");
        }
    }
    var net = Wlan.Current(out var why, readKey: false, interfaceId: card?.InterfaceId);
    Console.WriteLine(net is null ? $"wifi: {why}"
        : $"wifi: \"{net.Ssid}\" {net.Bssid} channel {net.Channel}, {net.Security}{(why == "" ? "" : " - " + why)}");
    var cfg = ServiceConfig.Load();
    Console.WriteLine($"mode {cfg.Mode}, start channel {cfg.StartChannel}, country \"{cfg.Country}\", peer channel {cfg.PeerChannel}");
    return;
}

// The service owns the card and reads the Wi-Fi key: only as LocalSystem.
if (!System.Security.Principal.WindowsIdentity.GetCurrent().IsSystem)
{
    Console.Error.WriteLine("AirdraftRadioService runs as a Windows service (LocalSystem). Use --status, --selftest or --give-back here.");
    return;
}

var builder = Host.CreateApplicationBuilder(args);
builder.Services.AddWindowsService(o => o.ServiceName = "AirdraftRadioService");
builder.Services.AddSingleton<Supervisor>();
builder.Services.AddHostedService<Worker>();
builder.Build().Run();

sealed class Worker(Supervisor sup) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        sup.Boot();
        await new ControlPipe(sup).Run(stoppingToken);
    }

    public override Task StopAsync(CancellationToken cancellationToken)
    {
        sup.Dispose();
        return base.StopAsync(cancellationToken);
    }
}
