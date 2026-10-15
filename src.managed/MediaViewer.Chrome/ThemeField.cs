// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The ground effect of a built-in theme. The Mac twin is ThemeField.swift:
// the counts, the speeds and the seed match. Marks only — the island already
// paints the canvas colour. Not a token, so an add-on theme has none, and
// the photo canvas never sees it.
//
// Motion rests (owner, 2026-10-15): thirty frames a second while this app is
// in front and a pointer or a key moved in the last five seconds; ten while
// the window is visible, for up to a minute after the last input; then none,
// and none while Windows has animations off. Dots do not move, and a theme with no ground does not either.
// Stopping the timer leaves the last frame. Speed scales velocities and size
// scales radii; 1 leaves a mark as written. Every position is a function of
// the clock, not of the last frame.

using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;
using Windows.UI.ViewManagement;

namespace MediaViewer.Chrome;

internal static class ThemeMotion
{
    internal static double Time { get; private set; }
    internal static event Action? Tick;
    internal static event Action? Changed;
    // The main window, for "visible"; and the native empty window's ear for
    // input over the islands (IslandHost sets both).
    internal static IntPtr Window;
    internal static Action? ForwardInput;

    private static int _listeners;
    private static bool _reduce;
    private static long _lastInput;
    private static long _lastTick;
    private static long _lastForward = long.MinValue / 2;
    private static int _intervalMs;  // 0 = stopped
    private static DispatcherQueueTimer? _timer;
    // The rule (owner, 2026-10-15; the native home runs the same, shell/home_theme.h
    // home_ground_clock): 30 a second in front with input in the last 5 seconds,
    // 10 a second while the window is visible for up to 60 seconds after the
    // last input, then rest.
    private const int FastMs = 33;
    private const int SlowMs = 100;
    private const long FastWindowMs = 5000;
    private const long SlowWindowMs = 60000;

    internal static void Bump()
    {
        _lastInput = Environment.TickCount64;
        // The native empty window hears its own HWND; the islands are separate
        // windows, so pass their input on (a few a second).
        if (_lastInput - _lastForward >= 500)
        {
            _lastForward = _lastInput;
            ForwardInput?.Invoke();
        }
        Ensure();
    }

    internal static void Retain()
    {
        _listeners++;
        Bump();
    }

    internal static void Release()
    {
        _listeners = Math.Max(0, _listeners - 1);
        Ensure();
    }

    internal static void Raise()
    {
        try { _reduce = !new UISettings().AnimationsEnabled; }
        catch (Exception) { _reduce = false; }
        Changed?.Invoke();
        Ensure();
    }

    // Milliseconds between ticks now, or 0 to rest.
    private static int Want()
    {
        if (_listeners == 0 || _reduce || !Visible()) return 0;
        long quiet = Environment.TickCount64 - _lastInput;
        if (InFront() && quiet < FastWindowMs) return FastMs;
        return quiet < SlowWindowMs ? SlowMs : 0;
    }

    private static void Ensure()
    {
        int want = Want();
        if (want == 0)
        {
            _timer?.Stop();
            _intervalMs = 0;
            return;
        }
        if (want == _intervalMs) return;
        DispatcherQueue? queue = DispatcherQueue.GetForCurrentThread();
        if (queue is null) return;
        if (_timer is null)
        {
            _timer = queue.CreateTimer();
            _timer.IsRepeating = true;
            _timer.Tick += OnTick;
        }
        _timer.Stop();
        _timer.Interval = TimeSpan.FromMilliseconds(want);
        if (_intervalMs == 0) _lastTick = Environment.TickCount64;
        _intervalMs = want;
        _timer.Start();
    }

    private static void OnTick(DispatcherQueueTimer sender, object args)
    {
        int want = Want();
        if (want == 0)
        {
            sender.Stop();
            _intervalMs = 0;
            return;
        }
        long now = Environment.TickCount64;
        Time += Math.Min(0.1, (now - _lastTick) / 1000.0);
        _lastTick = now;
        Tick?.Invoke();
        if (want != _intervalMs) Ensure();
    }

    private static bool InFront()
    {
        GetWindowThreadProcessId(GetForegroundWindow(), out uint pid);
        return pid == (uint)Environment.ProcessId;
    }

    // On screen: shown and not minimised. Windows does not say cheaply whether
    // another window covers it; the native canvas stops on DXGI occlusion.
    private static bool Visible() =>
        Window == IntPtr.Zero || (IsWindowVisible(Window) && !IsIconic(Window));

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsIconic(IntPtr hWnd);
}

internal sealed class ThemeField : Grid
{
    private readonly Canvas _marks = new()
    {
        IsHitTestVisible = false,
        HorizontalAlignment = HorizontalAlignment.Stretch,
        VerticalAlignment = VerticalAlignment.Stretch,
    };
    private readonly List<Spec> _specs = new();
    private string _key = "";
    private string _effect = "";
    private double _w;
    private double _h;
    private double _speed = 1;
    private double _size = 1;
    private double _gain = 1;
    private bool _held;
    private bool _hooked;

    internal static void Lay(Grid root)
    {
        var field = new ThemeField();
        Grid.SetRowSpan(field, Math.Max(root.RowDefinitions.Count, 1));
        Grid.SetColumnSpan(field, Math.Max(root.ColumnDefinitions.Count, 1));
        root.Children.Insert(0, field);
        root.AddHandler(UIElement.PointerMovedEvent, new PointerEventHandler((_, _) => ThemeMotion.Bump()), true);
        root.AddHandler(UIElement.KeyDownEvent, new KeyEventHandler((_, _) => ThemeMotion.Bump()), true);
    }

    private ThemeField()
    {
        IsHitTestVisible = false;
        IsTabStop = false;
        AutomationProperties.SetAccessibilityView(this, AccessibilityView.Raw);
        Children.Add(_marks);
        SizeChanged += (_, _) => Rebuild();
        Loaded += (_, _) =>
        {
            if (_hooked) return;
            _hooked = true;
            ThemeMotion.Changed += Rebuild;
            Rebuild();
        };
        Unloaded += (_, _) =>
        {
            if (!_hooked) return;
            _hooked = false;
            ThemeMotion.Changed -= Rebuild;
            Hold(false);
        };
    }

    private void Rebuild()
    {
        if (!IslandHost.ChromeEffectVisible
            || BuiltinThemes.Find(IslandHost.ChromeThemeSelection) is not { } item)
        {
            _key = "";
            _marks.Children.Clear();
            _specs.Clear();
            Hold(false);
            Visibility = Visibility.Collapsed;
            return;
        }
        Visibility = Visibility.Visible;
        double w = ActualWidth;
        double h = ActualHeight;
        if (w < 8 || h < 8)
        {
            Hold(false);
            return;
        }
        string key = item.Id + " " + item.Effect + " " + (int)w + "x" + (int)h;
        if (key == _key) return;
        _key = key;
        _effect = item.Effect;
        _w = w;
        _h = h;
        _speed = item.Speed;
        _size = item.Size;
        _gain = item.Intensity;
        _marks.Children.Clear();
        _specs.Clear();
        double area = w * h;
        uint rgb = item.EffectRgb;
        double gain = item.Intensity;
        switch (item.Effect)
        {
            case "none":
                break;
            case "dots":
                BuildDots(w, h, area, rgb, gain);
                break;
            case "rain":
                BuildRain(w, h, area, rgb, gain);
                break;
            case "starfield":
                BuildStars(w, h, area, rgb, gain);
                break;
            case "petals":
                BuildPetals(w, h, area, rgb, gain);
                break;
            case "fireflies":
                BuildFlies(w, h, area, rgb, gain);
                break;
            case "synapse":
                BuildSynapse(w, h, area, rgb, gain);
                break;
            case "embers":
                BuildEmbers(w, h, area, rgb);
                break;
            case "sparkles":
                BuildSparkles(w, h, area, rgb, gain);
                break;
            case "perlin-flow":
                BuildFlow(w, h, area, rgb, gain);
                break;
        }
        Hold(BuiltinThemes.Moves(item.Effect));
        Place(ThemeMotion.Time);
    }

    private void OnTick()
    {
        if (ActualWidth < 8 || ActualHeight < 8) return;
        for (DependencyObject? node = this; node is not null; node = VisualTreeHelper.GetParent(node))
        {
            if (node is UIElement element && element.Visibility != Visibility.Visible) return;
        }
        Place(ThemeMotion.Time);
    }

    private void Hold(bool moves)
    {
        if (moves == _held) return;
        _held = moves;
        if (moves)
        {
            ThemeMotion.Tick += OnTick;
            ThemeMotion.Retain();
        }
        else
        {
            ThemeMotion.Tick -= OnTick;
            ThemeMotion.Release();
        }
    }

    private void BuildDots(double w, double h, double area, uint rgb, double gain)
    {
        double spacing = Math.Max(28, Math.Sqrt(area / 72));
        double alpha = 0.16 * gain;
        for (double y = spacing * 0.5; y < h; y += spacing)
            for (double x = spacing * 0.5; x < w; x += spacing)
                AddDot(x, y, 1.2 * _size, rgb, alpha, null);
    }

    private void BuildRain(double w, double h, double area, uint rgb, double gain)
    {
        int n = Count(area, 9000, 64, 8);
        for (int i = 0; i < n; i++)
        {
            var rng = new Rng(Seed(i));
            double len = (14 + rng.Next() * 18) * _size;
            double speed = (32 + rng.Next() * 56) * _speed;
            double alpha = (0.14 + rng.Next() * 0.14) * gain;
            double x = rng.Next() * w;
            double y0 = rng.Next() * (h + len);
            var shape = Bar(1, len, rgb, alpha);
            _specs.Add(new Spec(x, y0, len, speed, 0, 0, 0, shape, null, null));
            _marks.Children.Add(shape);
        }
    }

    private void BuildStars(double w, double h, double area, uint rgb, double gain)
    {
        int n = Count(area, 7000, 90, 10);
        for (int i = 0; i < n; i++)
        {
            var rng = new Rng(Seed(i));
            double x = rng.Next() * w;
            double y0 = rng.Next() * h;
            double z = 0.18 + rng.Next() * 0.82;
            double radius = (0.45 + z * 1.15) * _size;
            double alpha = (0.10 + z * 0.38) * gain;
            var shape = Dot(radius, rgb, alpha);
            _specs.Add(new Spec(x, y0, z, radius, 0, 0, 0, shape, null, null));
            _marks.Children.Add(shape);
        }
    }

    private void BuildPetals(double w, double h, double area, uint rgb, double gain)
    {
        int n = Count(area, 26000, 18, 6);
        for (int i = 0; i < n; i++)
        {
            var rng = new Rng(Seed(i));
            double x0 = rng.Next() * w;
            double y0 = rng.Next() * (h + 16);
            double size = (3 + rng.Next() * 4) * _size;
            double vy = (16 + rng.Next() * 22) * _speed;
            double amp = (10 + rng.Next() * 16) * _size;
            double spin = (-0.5 + rng.Next()) * _speed;
            double phase = rng.Next() * Math.PI * 2;
            var turn = new RotateTransform();
            var a = Petal(size, rgb, 0.20 * gain, turn);
            var b = Petal(size, rgb, 0.13 * gain, turn);
            _specs.Add(new Spec(x0, y0, size, vy, amp, spin, phase, a, b, turn));
            _marks.Children.Add(a);
            _marks.Children.Add(b);
        }
    }

    private void BuildFlies(double w, double h, double area, uint rgb, double gain)
    {
        int n = Count(area, 15000, 32, 7);
        for (int i = 0; i < n; i++)
        {
            var rng = new Rng(Seed(i));
            double x0 = rng.Next() * w;
            double y0 = rng.Next() * h;
            double depth = rng.Next();
            double phase = rng.Next() * Math.PI * 2;
            var shape = Dot((1.1 + depth * 1.5) * _size, rgb, 1);
            _specs.Add(new Spec(x0, y0, depth, phase, gain, 0, 0, shape, null, null));
            _marks.Children.Add(shape);
        }
    }

    private void BuildSynapse(double w, double h, double area, uint rgb, double gain)
    {
        double spacing = Math.Max(24, (w + h) / 120);
        double line = 0.035 * gain;
        for (int row = 0; row * spacing < h; row++)
        {
            var bar = Bar(w, 1, rgb, line);
            Canvas.SetLeft(bar, 0);
            Canvas.SetTop(bar, row * spacing);
            _marks.Children.Add(bar);
        }
        for (int col = 0; col * spacing < w; col++)
        {
            var bar = Bar(1, h, rgb, line);
            Canvas.SetLeft(bar, col * spacing);
            Canvas.SetTop(bar, 0);
            _marks.Children.Add(bar);
        }
        int n = Count(area, 28000, 14, 4);
        for (int i = 0; i < n; i++)
        {
            var rng = new Rng(Seed(i));
            bool vertical = rng.Next() >= 0.5;
            double speed = (36 + rng.Next() * 74) * _speed;
            double trail = 14 * _size;
            double head = 1.3 * _size;
            double phase = rng.Next();
            int lanes = Math.Max(1, (int)((vertical ? w : h) / spacing));
            double lane = (int)(rng.Next() * lanes) * spacing;
            double span = vertical ? h : w;
            var trailShape = Bar(vertical ? 1 : trail, vertical ? trail : 1, rgb, 0.30 * gain);
            var headShape = Dot(head, rgb, 0.55 * gain);
            _specs.Add(new Spec(vertical ? 1 : 0, lane, phase, speed, trail, head, span,
                trailShape, headShape, null));
            _marks.Children.Add(trailShape);
            _marks.Children.Add(headShape);
        }
    }

    private void BuildEmbers(double w, double h, double area, uint rgb)
    {
        int n = Count(area, 14000, 36, 8);
        for (int i = 0; i < n; i++)
        {
            var rng = new Rng(Seed(i));
            double x0 = rng.Next() * w;
            double y0 = rng.Next() * (h + 24);
            double phase = rng.Next() * Math.PI * 2;
            double vy = (18 + rng.Next() * 36) * _speed;
            double amp = (6 + rng.Next() * 14) * _size;
            double radius = (1.1 + rng.Next() * 1.6) * _size;
            double rate = (0.18 + rng.Next() * 0.28) * _speed;
            var glow = Dot(radius * 3.2, rgb, 1);
            var core = Dot(radius * 0.65, rgb, 1);
            _specs.Add(new Spec(x0, y0, phase, vy, amp, rate, radius, glow, core, null));
            _marks.Children.Add(glow);
            _marks.Children.Add(core);
        }
    }

    private void BuildSparkles(double w, double h, double area, uint rgb, double gain)
    {
        int n = Count(area, 16000, 28, 6);
        for (int i = 0; i < n; i++)
        {
            var rng = new Rng(Seed(i));
            double x = rng.Next() * w;
            double y = rng.Next() * h;
            double radius = (2.4 + rng.Next() * 3.4) * _size;
            double phase = rng.Next() * Math.PI * 2;
            double freq = (0.9 + rng.Next() * 1.8) * _speed;
            var scale = new ScaleTransform { ScaleX = 1, ScaleY = 1 };
            Polygon shape = Star(radius, rgb);
            shape.RenderTransform = scale;
            shape.RenderTransformOrigin = new Point(0.5, 0.5);
            _specs.Add(new Spec(x, y, radius, phase, freq, gain, 0, shape, null, null, scale));
            _marks.Children.Add(shape);
        }
    }

    private void BuildFlow(double w, double h, double area, uint rgb, double gain)
    {
        int n = Count(area, 18000, 28, 6);
        double[] lags = { 0, 0.16, 0.32 };
        double[] alphas = { 0.22, 0.11, 0.05 };
        double[] radii = { 1.15, 0.85, 0.60 };
        for (int i = 0; i < n; i++)
        {
            var rng = new Rng(Seed(i));
            double x0 = rng.Next() * w;
            double y0 = rng.Next() * h;
            double phase = rng.Next() * Math.PI * 2;
            double freq = (0.12 + SmoothNoise(x0 * 0.01, y0 * 0.01) * 0.22) * _speed;
            double amp = (26 + SmoothNoise(x0 * 0.008 + 3, y0 * 0.008) * 46) * _size;
            for (int sample = 0; sample < 3; sample++)
            {
                var shape = Dot(radii[sample] * _size, rgb, 1);
                _specs.Add(new Spec(x0, y0, phase, freq, amp, lags[sample], alphas[sample] * gain,
                    shape, null, null));
                _marks.Children.Add(shape);
            }
        }
    }

    private void Place(double time)
    {
        switch (_effect)
        {
            case "rain":
                foreach (Spec s in _specs)
                {
                    double y = Wrap(s.P1 + time * s.P3, _h + s.P2) - s.P2;
                    Canvas.SetLeft(s.A, s.P0);
                    Canvas.SetTop(s.A, y);
                }
                break;
            case "starfield":
                foreach (Spec s in _specs)
                {
                    double speed = (12 + s.P2 * 48) * _speed;
                    double y = Wrap(s.P1 + time * speed, _h);
                    Canvas.SetLeft(s.A, s.P0 - s.P3);
                    Canvas.SetTop(s.A, y - s.P3);
                }
                break;
            case "petals":
                foreach (Spec s in _specs)
                {
                    double x = s.P0 + Math.Sin(s.P6 + time * 0.7 * _speed) * s.P4;
                    double y = Wrap(s.P1 + time * s.P3, _h + 16) - 8;
                    double width = s.P2 * 1.4;
                    double height = s.P2 * 0.64;
                    if (s.Spin is not null) s.Spin.Angle = (s.P6 + time * s.P5) * 180 / Math.PI;
                    Canvas.SetLeft(s.A, x - width / 2 - s.P2 * 0.15);
                    Canvas.SetTop(s.A, y - height / 2);
                    if (s.B is not null)
                    {
                        Canvas.SetLeft(s.B, x - width / 2 + s.P2 * 0.15);
                        Canvas.SetTop(s.B, y - height / 2);
                    }
                }
                break;
            case "fireflies":
                foreach (Spec s in _specs)
                {
                    double depth = s.P2;
                    double freq = (0.6 + depth * 0.9) * _speed;
                    double x = Wrap(s.P0 + Math.Sin(time * freq + s.P3) * (18 + depth * 36) * _size, _w);
                    double y = Wrap(s.P1 + Math.Cos(time * freq * 0.72 + s.P3) * (10 + depth * 22) * _size, _h);
                    double pulse = Math.Max(0, Math.Sin(time * (1.1 + depth) * _speed + s.P3));
                    double radius = (1.1 + depth * 1.5) * _size;
                    s.A.Opacity = pulse * (0.16 + depth * 0.34) * s.P4;
                    Canvas.SetLeft(s.A, x - radius);
                    Canvas.SetTop(s.A, y - radius);
                }
                break;
            case "synapse":
                foreach (Spec s in _specs)
                {
                    double span = s.P6 + s.P4;
                    double pos = Wrap(s.P2 * span + time * s.P3, span) - s.P4;
                    if (s.P0 < 0.5)
                    {
                        Canvas.SetLeft(s.A, pos);
                        Canvas.SetTop(s.A, s.P1);
                        if (s.B is not null)
                        {
                            Canvas.SetLeft(s.B, pos + s.P4 - s.P5);
                            Canvas.SetTop(s.B, s.P1 + 0.5 - s.P5);
                        }
                    }
                    else
                    {
                        Canvas.SetLeft(s.A, s.P1);
                        Canvas.SetTop(s.A, pos);
                        if (s.B is not null)
                        {
                            Canvas.SetLeft(s.B, s.P1 + 0.5 - s.P5);
                            Canvas.SetTop(s.B, pos + s.P4 - s.P5);
                        }
                    }
                }
                break;
            case "embers":
                foreach (Spec s in _specs)
                {
                    double life = Wrap(time * s.P5 + s.P2 / (Math.PI * 2), 1);
                    double fade = Math.Sin(life * Math.PI);
                    double x = Wrap(s.P0 + Math.Sin(s.P2 + time * 0.8 * _speed) * s.P4, _w);
                    double y = Wrap(s.P1 - time * s.P3, _h + 24);
                    double glow = s.P6 * 3.2;
                    double core = s.P6 * 0.65;
                    s.A.Opacity = 0.14 * fade * _gain;
                    Canvas.SetLeft(s.A, x - glow);
                    Canvas.SetTop(s.A, y - glow);
                    if (s.B is not null)
                    {
                        s.B.Opacity = 0.62 * fade * _gain;
                        Canvas.SetLeft(s.B, x - core);
                        Canvas.SetTop(s.B, y - core);
                    }
                }
                break;
            case "sparkles":
                foreach (Spec s in _specs)
                {
                    double twinkle = Math.Max(0, Math.Sin(time * s.P4 + s.P3));
                    s.A.Opacity = twinkle * 0.42 * s.P5;
                    if (s.Scale is not null)
                    {
                        double sc = 0.45 + twinkle * 0.55;
                        s.Scale.ScaleX = sc;
                        s.Scale.ScaleY = sc;
                    }
                    Canvas.SetLeft(s.A, s.P0 - s.P2);
                    Canvas.SetTop(s.A, s.P1 - s.P2);
                }
                break;
            case "perlin-flow":
                foreach (Spec s in _specs)
                {
                    double t = time - s.P5;
                    var at = Flow(s.P0, s.P1, s.P2, s.P3, s.P4, t, _speed, _w, _h);
                    s.A.Opacity = s.P6;
                    Canvas.SetLeft(s.A, at.X - s.A.Width / 2);
                    Canvas.SetTop(s.A, at.Y - s.A.Height / 2);
                }
                break;
        }
    }

    private ulong Seed(int i)
    {
        var item = BuiltinThemes.Find(IslandHost.ChromeThemeSelection);
        string text = (item?.Id ?? "") + ":" + _effect;
        return Rng.Hash(text) ^ ((ulong)i * 0x9E3779B97F4A7C15UL);
    }

    private static int Count(double area, double per, int cap, int min)
    {
        if (area < 800) return 0;
        return Math.Min(cap, Math.Max(min, (int)(area / per)));
    }

    private static double Wrap(double value, double span)
    {
        if (span <= 1) return 0;
        double m = value % span;
        return m < 0 ? m + span : m;
    }

    private static double Noise2d(double x, double y)
    {
        double n = Math.Sin(x * 12.9898 + y * 78.233) * 43758.5453;
        return n - Math.Floor(n);
    }

    private static double SmoothNoise(double x, double y)
    {
        double ix = Math.Floor(x);
        double iy = Math.Floor(y);
        double fx = x - ix;
        double fy = y - iy;
        double a = Noise2d(ix, iy);
        double b = Noise2d(ix + 1, iy);
        double c = Noise2d(ix, iy + 1);
        double d = Noise2d(ix + 1, iy + 1);
        double ux = fx * fx * (3 - 2 * fx);
        double uy = fy * fy * (3 - 2 * fy);
        return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
    }

    private readonly record struct FlowAt(double X, double Y);

    private static FlowAt Flow(double x0, double y0, double phase, double freq, double amp,
        double t, double pace, double w, double h)
    {
        double angle = SmoothNoise(x0 * 0.004 + t * 0.05 * pace, y0 * 0.004) * Math.PI * 6;
        double x = Wrap(x0 + Math.Cos(angle) * amp * 0.35 + Math.Sin(t * freq + phase) * amp, w);
        double y = Wrap(y0 + Math.Sin(angle) * amp * 0.35 + Math.Cos(t * freq * 0.72 + phase) * amp * 0.62, h);
        return new FlowAt(x, y);
    }

    private static Polygon Star(double radius, uint rgb)
    {
        double waist = radius * 0.325;
        var points = new PointCollection
        {
            new Point(radius, 0),
            new Point(radius + waist, radius - waist),
            new Point(radius * 2, radius),
            new Point(radius + waist, radius + waist),
            new Point(radius, radius * 2),
            new Point(radius - waist, radius + waist),
            new Point(0, radius),
            new Point(radius - waist, radius - waist),
        };
        return new Polygon
        {
            Points = points,
            Fill = new SolidColorBrush(Rgb(rgb, 255)),
            IsHitTestVisible = false,
        };
    }

    private static Color Rgb(uint rgb, byte alpha) =>
        Color.FromArgb(alpha, (byte)((rgb >> 16) & 255), (byte)((rgb >> 8) & 255), (byte)(rgb & 255));

    private void AddDot(double x, double y, double radius, uint rgb, double alpha, RotateTransform? spin)
    {
        Ellipse shape = Dot(radius, rgb, alpha);
        if (spin is not null)
        {
            shape.RenderTransform = spin;
            shape.RenderTransformOrigin = new Point(0.5, 0.5);
        }
        Canvas.SetLeft(shape, x - radius);
        Canvas.SetTop(shape, y - radius);
        _marks.Children.Add(shape);
    }

    private static Ellipse Dot(double radius, uint rgb, double alpha)
    {
        byte a = (byte)Math.Clamp((int)Math.Round(alpha * 255), 0, 255);
        return new Ellipse
        {
            Width = radius * 2,
            Height = radius * 2,
            Fill = new SolidColorBrush(Rgb(rgb, a)),
            IsHitTestVisible = false,
        };
    }

    private static Rectangle Bar(double width, double height, uint rgb, double alpha)
    {
        byte a = (byte)Math.Clamp((int)Math.Round(alpha * 255), 0, 255);
        return new Rectangle
        {
            Width = width,
            Height = height,
            Fill = new SolidColorBrush(Rgb(rgb, a)),
            IsHitTestVisible = false,
        };
    }

    private static Ellipse Petal(double size, uint rgb, double alpha, RotateTransform spin)
    {
        Ellipse shape = Dot(1, rgb, alpha);
        shape.Width = size * 1.4;
        shape.Height = size * 0.64;
        shape.RenderTransform = spin;
        shape.RenderTransformOrigin = new Point(0.5, 0.5);
        return shape;
    }

    private sealed class Spec
    {
        internal readonly double P0, P1, P2, P3, P4, P5, P6;
        internal readonly FrameworkElement A;
        internal readonly FrameworkElement? B;
        internal readonly RotateTransform? Spin;
        internal readonly ScaleTransform? Scale;
        internal Spec(double p0, double p1, double p2, double p3, double p4, double p5, double p6,
            FrameworkElement a, FrameworkElement? b, RotateTransform? spin, ScaleTransform? scale = null)
        {
            P0 = p0; P1 = p1; P2 = p2; P3 = p3; P4 = p4; P5 = p5; P6 = p6;
            A = a; B = b; Spin = spin; Scale = scale;
        }
    }

    private struct Rng
    {
        private ulong _state;
        internal Rng(ulong seed) { _state = seed; }
        internal double Next()
        {
            _state += 0x9E3779B97F4A7C15UL;
            ulong z = _state;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9UL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBUL;
            z ^= z >> 31;
            return (z >> 11) / (double)(1UL << 53);
        }
        internal static ulong Hash(string text)
        {
            ulong h = 0x6a09e667f3bcc909UL;
            foreach (char c in text)
                h = (h ^ (byte)c) * 0x100000001b3UL;
            return h;
        }
    }
}
