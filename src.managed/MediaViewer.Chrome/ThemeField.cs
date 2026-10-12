// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The ground effect of a built-in theme. The Mac twin is ThemeField.swift:
// the counts, the speeds and the seed match. Marks only — the island already
// paints the canvas colour. Not a token, so an add-on theme has none, and
// the photo canvas never sees it.
//
// Motion rests. Ten frames a second, and only while this app is in front,
// Windows has animations on, and a pointer or a key moved in the last five
// seconds. Dots do not move. Stopping the timer leaves the last frame.

using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.UI;
using Windows.UI.ViewManagement;

namespace MediaViewer.Chrome;

internal static class ThemeMotion
{
    internal static double Time { get; private set; }
    internal static event Action? Tick;
    internal static event Action? Changed;

    private static int _listeners;
    private static bool _reduce;
    private static bool _running;
    private static long _lastInput;
    private static long _lastTick;
    private static DispatcherQueueTimer? _timer;
    private const long RestMs = 5000;

    internal static void Bump()
    {
        _lastInput = Environment.TickCount64;
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

    private static void Ensure()
    {
        bool live = _listeners > 0 && !_reduce && InFront()
                    && Environment.TickCount64 - _lastInput < RestMs;
        if (!live)
        {
            _timer?.Stop();
            _running = false;
            return;
        }
        if (_running) return;
        DispatcherQueue? queue = DispatcherQueue.GetForCurrentThread();
        if (queue is null) return;
        if (_timer is null)
        {
            _timer = queue.CreateTimer();
            _timer.Interval = TimeSpan.FromMilliseconds(100);
            _timer.IsRepeating = true;
            _timer.Tick += OnTick;
        }
        _lastTick = Environment.TickCount64;
        _running = true;
        _timer.Start();
    }

    private static void OnTick(DispatcherQueueTimer sender, object args)
    {
        if (_listeners == 0 || _reduce || !InFront()
            || Environment.TickCount64 - _lastInput >= RestMs)
        {
            sender.Stop();
            _running = false;
            return;
        }
        long now = Environment.TickCount64;
        Time += Math.Min(0.1, (now - _lastTick) / 1000.0);
        _lastTick = now;
        Tick?.Invoke();
    }

    private static bool InFront()
    {
        GetWindowThreadProcessId(GetForegroundWindow(), out uint pid);
        return pid == (uint)Environment.ProcessId;
    }

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
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
        _marks.Children.Clear();
        _specs.Clear();
        double area = w * h;
        uint rgb = item.EffectRgb;
        double gain = item.Intensity;
        switch (item.Effect)
        {
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
                AddDot(x, y, 1.2, rgb, alpha, null);
    }

    private void BuildRain(double w, double h, double area, uint rgb, double gain)
    {
        int n = Count(area, 9000, 64, 8);
        for (int i = 0; i < n; i++)
        {
            var rng = new Rng(Seed(i));
            double len = 14 + rng.Next() * 18;
            double speed = 32 + rng.Next() * 56;
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
            double radius = 0.45 + z * 1.15;
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
            double size = 3 + rng.Next() * 4;
            double vy = 16 + rng.Next() * 22;
            double amp = 10 + rng.Next() * 16;
            double spin = -0.5 + rng.Next();
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
            var shape = Dot(1.1 + depth * 1.5, rgb, 1);
            _specs.Add(new Spec(x0, y0, depth, phase, gain, 0, 0, shape, null, null));
            _marks.Children.Add(shape);
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
                    double speed = 12 + s.P2 * 48;
                    double y = Wrap(s.P1 + time * speed, _h);
                    Canvas.SetLeft(s.A, s.P0 - s.P3);
                    Canvas.SetTop(s.A, y - s.P3);
                }
                break;
            case "petals":
                foreach (Spec s in _specs)
                {
                    double x = s.P0 + Math.Sin(s.P6 + time * 0.7) * s.P4;
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
                    double freq = 0.6 + depth * 0.9;
                    double x = Wrap(s.P0 + Math.Sin(time * freq + s.P3) * (18 + depth * 36), _w);
                    double y = Wrap(s.P1 + Math.Cos(time * freq * 0.72 + s.P3) * (10 + depth * 22), _h);
                    double pulse = Math.Max(0, Math.Sin(time * (1.1 + depth) + s.P3));
                    double radius = 1.1 + depth * 1.5;
                    s.A.Opacity = pulse * (0.16 + depth * 0.34) * s.P4;
                    Canvas.SetLeft(s.A, x - radius);
                    Canvas.SetTop(s.A, y - radius);
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
        internal Spec(double p0, double p1, double p2, double p3, double p4, double p5, double p6,
            FrameworkElement a, FrameworkElement? b, RotateTransform? spin)
        {
            P0 = p0; P1 = p1; P2 = p2; P3 = p3; P4 = p4; P5 = p5; P6 = p6;
            A = a; B = b; Spin = spin;
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
