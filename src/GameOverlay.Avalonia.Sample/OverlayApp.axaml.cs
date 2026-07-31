using Avalonia;
using Avalonia.Markup.Xaml;

namespace GameOverlay.Avalonia.Sample;

public partial class OverlayApp : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);
}
