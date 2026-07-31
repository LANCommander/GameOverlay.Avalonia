using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Media;

namespace GameOverlay.Avalonia;

/// <summary>
/// The overlay's own mouse cursor, composited over the consumer's content.
/// </summary>
/// <remarks>
/// The overlay is not a real window, so there is no OS cursor to move - and in
/// exclusive fullscreen the game has usually hidden the system cursor entirely.
/// Drawing the pointer inside the overlay's own visual tree is therefore
/// functional, not decorative, and the library owns it so consumers get a
/// working pointer without adding anything.
/// </remarks>
internal sealed class OverlayCursor
{
    private readonly Canvas _canvas;
    private readonly Path _glyph;

    public OverlayCursor()
    {
        _glyph = new Path
        {
            Fill = Brushes.White,
            Stroke = new SolidColorBrush(Color.FromArgb(0xC0, 0, 0, 0)),
            StrokeThickness = 1,
            Data = Arrow(),
        };

        _canvas = new Canvas
        {
            // The cursor never intercepts input - it only shows where the
            // virtual pointer is; hit-testing happens against the real content.
            IsHitTestVisible = false,
            IsVisible = false,
        };
        _canvas.Children.Add(_glyph);
    }

    /// <summary>The visual to place on top of the consumer content.</summary>
    public Control Visual => _canvas;

    public void SetVisible(bool visible) => _canvas.IsVisible = visible;

    public void SetPosition(Point position)
    {
        Canvas.SetLeft(_glyph, position.X);
        Canvas.SetTop(_glyph, position.Y);
    }

    /// <summary>
    /// Swaps the glyph for the shape Avalonia asked for. Only the shapes that
    /// actually change how a control reads are distinguished.
    /// </summary>
    public void SetShape(StandardCursorType type)
    {
        _glyph.Data = type switch
        {
            StandardCursorType.Hand =>
                Geometry.Parse("M 5,0 L 5,9 L 3,7 L 1,9 L 4,15 L 12,15 L 13,7 L 11,6 L 10,7 L 9,5 L 8,6 L 7,4 Z"),
            StandardCursorType.Ibeam =>
                Geometry.Parse("M 2,0 L 8,0 L 8,2 L 6,2 L 6,14 L 8,14 L 8,16 L 2,16 L 2,14 L 4,14 L 4,2 L 2,2 Z"),
            StandardCursorType.SizeWestEast =>
                Geometry.Parse("M 0,8 L 5,3 L 5,6 L 13,6 L 13,3 L 18,8 L 13,13 L 13,10 L 5,10 L 5,13 Z"),
            _ => Arrow(),
        };
    }

    private static Geometry Arrow()
        => Geometry.Parse("M 0,0 L 0,17 L 4.2,13.2 L 6.8,19 L 9.6,17.8 L 7,12.2 L 12.4,12 Z");
}
