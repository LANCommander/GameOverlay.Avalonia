using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Windows.Input;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;

namespace GameOverlay.Avalonia.Sample;

/// <summary>
/// The demo control tree the sample projects into the overlay. Nothing here is
/// overlay-specific - it is an ordinary Avalonia <see cref="UserControl"/>; the
/// library composites it and draws the cursor. The named controls and the
/// counters exist only so the scripted verification tools can drive and assert
/// on real interaction.
/// </summary>
public partial class OverlayView : UserControl
{
    public OverlayView()
    {
        AvaloniaXamlLoader.Load(this);
        DataContext = new OverlayViewModel();
    }

    internal OverlayViewModel Model => (OverlayViewModel)DataContext!;

    /// <summary>
    /// Reports where the demo controls actually ended up, in device-independent
    /// pixels relative to this view, so the scripted interaction test can click
    /// real controls instead of hardcoded coordinates that rot when layout, font
    /// metrics or the UI scale change.
    /// </summary>
    internal IReadOnlyList<(string Name, Rect Bounds)> GetTestTargets()
    {
        var targets = new List<(string, Rect)>();

        foreach (string name in new[] { "ClickButton", "DemoTextBox", "DemoSlider" })
        {
            if (this.FindControl<Control>(name) is not { } control) continue;
            if (control.TranslatePoint(default, this) is not { } origin) continue;
            targets.Add((name, new Rect(origin, control.Bounds.Size)));
        }

        return targets;
    }
}

/// <summary>
/// Backs the demo UI. The counters and captions exist so a screenshot can prove
/// interaction actually happened, rather than merely that the overlay rendered.
/// </summary>
public sealed class OverlayViewModel : INotifyPropertyChanged
{
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private readonly DispatcherTimer _timer;
    private int _clicks;
    private string _typedText = string.Empty;
    private double _sliderValue = 25;
    private bool _isChecked = true;

    public OverlayViewModel()
    {
        ClickCommand = new RelayCommand(() =>
        {
            _clicks++;
            Raise(nameof(ClickCaption));
            Log($"click #{_clicks}");
        });

        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
        _timer.Tick += (_, _) => Raise(nameof(Uptime));
        _timer.Start();

        Log("overlay ready");
    }

    public ICommand ClickCommand { get; }

    public ObservableCollection<string> LogLines { get; } = new();

    public string ClickCaption => $"clicks: {_clicks}";

    public string Uptime => _clock.Elapsed.ToString(@"hh\:mm\:ss");

    public string SliderCaption => $"{_sliderValue:0}";

    public string TypedText
    {
        get => _typedText;
        set
        {
            if (_typedText == value) return;
            _typedText = value;
            Raise();
        }
    }

    public double SliderValue
    {
        get => _sliderValue;
        set
        {
            if (Math.Abs(_sliderValue - value) < 0.01) return;
            _sliderValue = value;
            Raise();
            Raise(nameof(SliderCaption));
        }
    }

    public bool IsChecked
    {
        get => _isChecked;
        set
        {
            if (_isChecked == value) return;
            _isChecked = value;
            Raise();
            Log($"checkbox {(value ? "on" : "off")}");
        }
    }

    private void Log(string line)
    {
        LogLines.Add($"{_clock.Elapsed:mm\\:ss\\.ff}  {line}");
        while (LogLines.Count > 40) LogLines.RemoveAt(0);
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private void Raise([CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}

internal sealed class RelayCommand(Action execute) : ICommand
{
    public event EventHandler? CanExecuteChanged { add { } remove { } }

    public bool CanExecute(object? parameter) => true;

    public void Execute(object? parameter) => execute();
}
