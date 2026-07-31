namespace GameOverlay.Avalonia;

/// <summary>
/// A snapshot of overlay counters, useful for diagnostics and health checks.
/// </summary>
public readonly record struct OverlayStatistics
{
    /// <summary>Frames the game has presented since attach.</summary>
    public uint GamePresents { get; init; }

    /// <summary>Overlay composites the payload has issued into the game.</summary>
    public uint PayloadDraws { get; init; }

    /// <summary>Frames the payload skipped because the shared texture was busy.</summary>
    public uint PayloadMutexTimeouts { get; init; }

    /// <summary>Overlay frames the host has published to the payload.</summary>
    public uint Published { get; init; }

    /// <summary>Publishes the host skipped because the payload was still reading.</summary>
    public uint Skipped { get; init; }

    /// <summary>Input messages the payload's window hook observed while capturing.</summary>
    public uint InputSeen { get; init; }

    /// <summary>Input events delivered to Avalonia.</summary>
    public uint InputDispatched { get; init; }

    /// <summary>Input events dropped because the transport ring was full.</summary>
    public uint InputDropped { get; init; }
}
