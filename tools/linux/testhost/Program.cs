using System;
using System.Diagnostics;
using System.Threading;
using GameOverlay.Avalonia;

// Linux end-to-end transport test. Drives the real managed pipeline -
// LinuxProcessInjector (LD_PRELOAD launch), OverlaySharedState + CpuFrameProducer
// over LinuxSharedMemory (POSIX shm), and OverlaySession's handshake + pump -
// against a real GLX game and the injected payload. Uses TestPatternFrameSource
// so no Avalonia/Skia is involved: a PASS proves the transport and the payload
// interoperate, isolating that from the UI layer.

if (args.Length < 1)
{
    Console.Error.WriteLine("usage: TestHost <game-exe> [seconds]");
    return 2;
}

string gameExe = args[0];
int seconds = args.Length > 1 ? int.Parse(args[1]) : 4;
// Optional: a path to xtest_inject switches this into input-capture mode.
string? injector = args.Length > 2 ? args[2] : null;

void Log(string m) => Console.WriteLine(m);

IProcessInjector game = PlatformServices.LaunchSuspended(gameExe, null, Log);
using var source = new TestPatternFrameSource();
using var session = new OverlaySession(game, PayloadPath(), source, Log);

Console.WriteLine("[test] waiting for payload handshake...");
session.Attach(TimeSpan.FromSeconds(15));
var (w, h) = session.GameSize;
Console.WriteLine($"[test] attached: api={session.GraphicsApi} size={w}x{h}");
session.Visible = true;

var cts = new CancellationTokenSource();
var pump = new Thread(() => session.Run(cts.Token, 60)) { IsBackground = true, Name = "pump" };
pump.Start();

bool pass;
if (injector is not null)
{
    // Input-capture test: turn capture on (the payload grabs on its next swap),
    // fire synthetic input, and confirm the payload observed it on the ring.
    session.Interactive = true;
    Thread.Sleep(TimeSpan.FromMilliseconds(700));   // let the grab take effect
    uint before = session.Statistics.InputSeen;

    var inj = Process.Start(new ProcessStartInfo { FileName = injector, UseShellExecute = false });
    inj!.WaitForExit();
    Thread.Sleep(TimeSpan.FromMilliseconds(500));

    uint after = session.Statistics.InputSeen;
    session.Interactive = false;
    Console.WriteLine($"[test] inputSeen before={before} after={after} (delta={after - before})");
    pass = after > before;
}
else
{
    Thread.Sleep(TimeSpan.FromSeconds(seconds));
    var s = session.Statistics;
    Console.WriteLine($"[test] stats: published={s.Published} skipped={s.Skipped} " +
                      $"gamePresents={s.GamePresents} payloadDraws={s.PayloadDraws}");
    pass = s.Published > 0 && s.PayloadDraws > 0;
}

cts.Cancel();
pump.Join(TimeSpan.FromSeconds(2));

Console.WriteLine(pass ? "RESULT: PASS" : "RESULT: FAIL");
return pass ? 0 : 1;

// The injector sets LD_PRELOAD itself; OverlaySession only needs a path for its
// (no-op on Linux) inject/detach calls.
static string PayloadPath() => "/work/build-linux/bin/GameOverlay.Native.so";
