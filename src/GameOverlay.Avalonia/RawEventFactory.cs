using System;
using System.Linq;
using System.Linq.Expressions;
using System.Reflection;
using Avalonia;
using Avalonia.Input;
using Avalonia.Input.Raw;

// Disambiguated from System.Reflection.Pointer, which this file also pulls in.
using AvaloniaPointer = Avalonia.Input.Pointer;

namespace GameOverlay.Avalonia;

/// <summary>
/// Builds Avalonia's raw input events, which cannot be constructed directly.
/// </summary>
/// <remarks>
/// <para>
/// Avalonia ships different accessibility in its reference and implementation
/// assemblies. In <c>ref/net8.0/Avalonia.Base.dll</c> these constructors are
/// <c>internal</c>, so the compiler refuses them; in <c>lib/net8.0</c> they are
/// <c>public</c>, so reflection reaches them with ordinary public binding. The
/// types themselves are public in both, which is why only the constructors need
/// this treatment.
/// </para>
/// <para>
/// Each constructor is resolved once and compiled into a delegate, so the
/// per-event cost is a delegate invocation rather than a reflective call.
/// Resolution happens eagerly at startup via <see cref="Validate"/>: an
/// Avalonia upgrade that changes a signature should fail immediately with a
/// clear message, not silently at the user's first click.
/// </para>
/// </remarks>
internal static class RawEventFactory
{
    private static readonly Lazy<Func<IInputDevice, ulong, IInputRoot, RawPointerEventType, Point, RawInputModifiers, RawInputEventArgs>>
        PointerFactory = new(() => Build<Func<IInputDevice, ulong, IInputRoot, RawPointerEventType, Point, RawInputModifiers, RawInputEventArgs>>(
            typeof(RawPointerEventArgs),
            typeof(IInputDevice), typeof(ulong), typeof(IInputRoot),
            typeof(RawPointerEventType), typeof(Point), typeof(RawInputModifiers)));

    private static readonly Lazy<Func<IInputDevice, ulong, IInputRoot, Point, Vector, RawInputModifiers, RawInputEventArgs>>
        WheelFactory = new(() => Build<Func<IInputDevice, ulong, IInputRoot, Point, Vector, RawInputModifiers, RawInputEventArgs>>(
            typeof(RawMouseWheelEventArgs),
            typeof(IInputDevice), typeof(ulong), typeof(IInputRoot),
            typeof(Point), typeof(Vector), typeof(RawInputModifiers)));

    private static readonly Lazy<Func<IKeyboardDevice, ulong, IInputRoot, RawKeyEventType, Key, RawInputModifiers, RawInputEventArgs>>
        KeyFactory = new(() => Build<Func<IKeyboardDevice, ulong, IInputRoot, RawKeyEventType, Key, RawInputModifiers, RawInputEventArgs>>(
            typeof(RawKeyEventArgs),
            typeof(IKeyboardDevice), typeof(ulong), typeof(IInputRoot),
            typeof(RawKeyEventType), typeof(Key), typeof(RawInputModifiers)));

    private static readonly Lazy<Func<IKeyboardDevice, ulong, IInputRoot, string, RawInputEventArgs>>
        TextFactory = new(() => Build<Func<IKeyboardDevice, ulong, IInputRoot, string, RawInputEventArgs>>(
            typeof(RawTextInputEventArgs),
            typeof(IKeyboardDevice), typeof(ulong), typeof(IInputRoot), typeof(string)));

    /// <summary>
    /// The primary keyboard device. <c>KeyboardDevice.Instance</c> is internal
    /// even at runtime, so unlike the constructors it needs non-public binding.
    /// The singleton must be used rather than a fresh instance, or Avalonia's
    /// focus tracking and ours disagree about who has focus.
    /// </summary>
    private static readonly Lazy<IKeyboardDevice> Keyboard = new(() =>
        typeof(KeyboardDevice)
            .GetProperty("Instance", BindingFlags.NonPublic | BindingFlags.Static)
            ?.GetValue(null) as IKeyboardDevice
        ?? throw new InvalidOperationException(
            "KeyboardDevice.Instance could not be resolved. Avalonia's internal input " +
            "API has changed; RawEventFactory needs updating."));

    public static IKeyboardDevice KeyboardDevice => Keyboard.Value;

    private static readonly Lazy<Func<int, PointerType, bool, AvaloniaPointer>> PointerCtor =
        new(() => Build<Func<int, PointerType, bool, AvaloniaPointer>>(
            typeof(AvaloniaPointer), typeof(int), typeof(PointerType), typeof(bool)));

    private static readonly Lazy<Func<AvaloniaPointer, MouseDevice>> MouseDeviceCtor =
        new(() => Build<Func<AvaloniaPointer, MouseDevice>>(typeof(MouseDevice), typeof(AvaloniaPointer)));

    /// <summary>
    /// Creates the pointer device the offscreen top level needs.
    /// </summary>
    /// <remarks>
    /// The obvious-looking <c>MouseDevice.GetOrCreatePrimary&lt;T&gt;()</c> is a
    /// trap: it is constrained <c>where T : MouseDevice, new()</c>, and
    /// <c>MouseDevice</c> has no parameterless constructor, so it can only ever
    /// be closed over a platform-specific subclass. Constructing one directly
    /// is both simpler and correct - and, like the raw event constructors,
    /// <c>MouseDevice(Pointer)</c> is public at runtime even though the
    /// reference assembly hides it.
    /// </remarks>
    public static IMouseDevice CreateMouseDevice()
    {
        int id = 1;
        try
        {
            if (typeof(AvaloniaPointer).GetMethod("GetNextFreeId",
                    BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public)
                    ?.Invoke(null, null) is int next)
            {
                id = next;
            }
        }
        catch
        {
            // A colliding id is harmless for a single pointer; fall back to 1.
        }

        return MouseDeviceCtor.Value(PointerCtor.Value(id, PointerType.Mouse, true));
    }

    /// <summary>
    /// Forces every factory to resolve, so signature drift surfaces at startup.
    /// </summary>
    public static void Validate()
    {
        _ = PointerFactory.Value;
        _ = WheelFactory.Value;
        _ = KeyFactory.Value;
        _ = TextFactory.Value;
        _ = Keyboard.Value;
        _ = PointerCtor.Value;
        _ = MouseDeviceCtor.Value;
    }

    public static RawInputEventArgs Pointer(IInputDevice device, IInputRoot root,
        RawPointerEventType type, Point position, RawInputModifiers modifiers)
        => PointerFactory.Value(device, Timestamp, root, type, position, modifiers);

    public static RawInputEventArgs Wheel(IInputDevice device, IInputRoot root,
        Point position, Vector delta, RawInputModifiers modifiers)
        => WheelFactory.Value(device, Timestamp, root, position, delta, modifiers);

    public static RawInputEventArgs Key(IInputRoot root, RawKeyEventType type,
        Key key, RawInputModifiers modifiers)
        => KeyFactory.Value(KeyboardDevice, Timestamp, root, type, key, modifiers);

    public static RawInputEventArgs Text(IInputRoot root, string text)
        => TextFactory.Value(KeyboardDevice, Timestamp, root, text);

    private static ulong Timestamp => (ulong)Environment.TickCount64;

    private static TDelegate Build<TDelegate>(Type target, params Type[] signature)
        where TDelegate : Delegate
    {
        ConstructorInfo ctor = target.GetConstructor(
                                   BindingFlags.Public | BindingFlags.Instance,
                                   binder: null, signature, modifiers: null)
            ?? throw new InvalidOperationException(
                $"{target.Name} has no constructor ({string.Join(", ", signature.Select(t => t.Name))}). " +
                "This build of Avalonia is not compatible with RawEventFactory; the raw input " +
                "signatures it binds to have changed.");

        ParameterExpression[] parameters = signature.Select(Expression.Parameter).ToArray();
        return Expression.Lambda<TDelegate>(Expression.New(ctor, parameters), parameters).Compile();
    }
}
