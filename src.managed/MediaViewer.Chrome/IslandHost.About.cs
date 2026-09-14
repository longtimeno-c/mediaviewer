// SPDX-License-Identifier: GPL-2.0-or-later
using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace MediaViewer.Chrome;

// PR 8 About (plan/13 "About", plan/11): the mark, the running version, the
// GPL, GitHub, THIRD-PARTY.md and the LGPL source offer for *this* build.
// No command-table entry (plan/16 PR 8 row); the flyout is reached from the
// bar and every link is a tab stop.
public static partial class IslandHost
{
    private const string RepoUrl = "https://github.com/longtimeno-c/mediaviewer";
    private const string AboutMarkFile = "mediaviewer-256.png";
    private const string LicenceFile = "LICENSE";
    private const string ThirdPartyFile = "THIRD-PARTY.md";

    private static string ExeDirectory()
    {
        char[] buf = new char[32768];
        uint n = GetModuleFileNameW(IntPtr.Zero, buf, (uint)buf.Length);
        if (n > 0 && n < buf.Length)
        {
            string? dir = Path.GetDirectoryName(new string(buf, 0, (int)n));
            if (dir is not null) return dir;
        }
        return AppContext.BaseDirectory;
    }

    private static string ExePath()
    {
        char[] buf = new char[32768];
        uint n = GetModuleFileNameW(IntPtr.Zero, buf, (uint)buf.Length);
        return n > 0 && n < buf.Length ? new string(buf, 0, (int)n) : string.Empty;
    }

    /// The version comes from the host exe's VERSIONINFO, which CMake fills
    /// from project(VERSION) — the one version number (no second source).
    private static string? ReadHostVersion()
    {
        try
        {
            string exe = ExePath();
            if (exe.Length == 0) return null;
            FileVersionInfo info = FileVersionInfo.GetVersionInfo(exe);
            string? v = info.ProductVersion;
            return string.IsNullOrWhiteSpace(v) ? null : v.Trim();
        }
        catch (Exception ex)
        {
            Debug.WriteLine(ex);
            return null;
        }
    }

    private static FrameworkElement BuildAboutContent()
    {
        string dir = ExeDirectory();

        var mark = new Image
        {
            Width = 64,
            Height = 64,
            VerticalAlignment = VerticalAlignment.Top,
            Margin = new Thickness(0, 2, 14, 0),
        };
        try
        {
            mark.Source = new BitmapImage(new Uri(Path.Combine(dir, AboutMarkFile)))
            {
                DecodePixelWidth = 128,
            };
        }
        catch (Exception ex)
        {
            Debug.WriteLine(ex);
        }

        var name = new TextBlock
        {
            Text = "MediaViewer",
            Foreground = Brush(Title),
            FontFamily = UiFont,
            FontSize = UiFontSize + 4,
        };
        var version = new TextBlock
        {
            Text = "Version …",
            Foreground = Brush(Body),
            FontFamily = UiFont,
            FontSize = UiFontSize,
        };
        var licence = new TextBlock
        {
            Text = "Licensed GPL-2.0-or-later",
            Foreground = Brush(Body),
            FontFamily = UiFont,
            FontSize = UiFontSize,
            Margin = new Thickness(0, 6, 0, 0),
        };
        var hint = new TextBlock
        {
            Text = "Everything works from the keyboard. Press ? for the shortcuts of what you are doing.",
            Foreground = Brush(Body),
            FontFamily = UiFont,
            FontSize = UiFontSize,
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 10, 0, 0),
        };

        var sourceLink = AboutLink("Source code for this build (LGPL components)",
                                   () => OpenUri(RepoUrl + "/releases"));
        var links = new StackPanel { Spacing = 0, Margin = new Thickness(-6, 8, 0, 0) };
        links.Children.Add(AboutLink("Licence (GPL-2.0-or-later)",
                                     () => OpenLocal(Path.Combine(dir, LicenceFile), RepoUrl + "/blob/main/LICENSE", textOnly: true)));
        links.Children.Add(AboutLink("Third-party notices",
                                     () => OpenLocal(Path.Combine(dir, ThirdPartyFile), RepoUrl + "/blob/main/THIRD-PARTY.md", textOnly: false)));
        links.Children.Add(AboutLink("GitHub — longtimeno-c/mediaviewer", () => OpenUri(RepoUrl)));
        links.Children.Add(sourceLink);

        var text = new StackPanel { Spacing = 0 };
        text.Children.Add(name);
        text.Children.Add(version);
        text.Children.Add(licence);
        text.Children.Add(links);
        text.Children.Add(hint);

        var root = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Margin = new Thickness(12, 10, 12, 10),
            MaxWidth = 460,
            TabFocusNavigation = Microsoft.UI.Xaml.Input.KeyboardNavigationMode.Cycle,
        };
        root.Children.Add(mark);
        root.Children.Add(text);
        text.MaxWidth = 460 - 64 - 14 - 24;

        // Reading VERSIONINFO touches the exe on disk: off the UI thread, then
        // marshalled back. The source-offer link waits for the version so it
        // never points at the wrong release.
        var queue = Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread();
        _ = Task.Run(() =>
        {
            string? v = ReadHostVersion();
            queue?.TryEnqueue(() =>
            {
                version.Text = v is null ? "Version unknown" : $"Version {v}";
                if (v is not null)
                {
                    string tag = v.Split('+')[0];
                    sourceLink.Tag = $"{RepoUrl}/releases/tag/v{tag}";
                }
            });
        });
        return root;
    }

    private static HyperlinkButton AboutLink(string label, Action open)
    {
        var link = new HyperlinkButton
        {
            Content = new TextBlock
            {
                Text = label,
                FontFamily = UiFont,
                FontSize = UiFontSize,
                TextWrapping = TextWrapping.Wrap,
            },
            Padding = new Thickness(6, 3, 6, 3),
            FontFamily = UiFont,
            FontSize = UiFontSize,
            IsTabStop = true,
            AllowFocusOnInteraction = true,
        };
        link.Click += (sender, _) =>
        {
            // A link whose target depends on runtime state carries it in Tag.
            if (sender is HyperlinkButton { Tag: string uri }) OpenUri(uri);
            else open();
        };
        return link;
    }

    private static void OpenUri(string uri)
    {
        _ = Task.Run(() =>
        {
            try
            {
                Process.Start(new ProcessStartInfo(uri) { UseShellExecute = true })?.Dispose();
            }
            catch (Exception ex)
            {
                Debug.WriteLine(ex);
            }
        });
    }

    /// Opens a bundled file with the default app, off the UI thread. LICENSE has
    /// no extension and .md often has no association, so fall back to Notepad,
    /// then to the file on GitHub.
    private static void OpenLocal(string path, string fallbackUri, bool textOnly)
    {
        _ = Task.Run(() =>
        {
            try
            {
                if (!File.Exists(path))
                {
                    Process.Start(new ProcessStartInfo(fallbackUri) { UseShellExecute = true })?.Dispose();
                    return;
                }
                if (!textOnly)
                {
                    try
                    {
                        Process.Start(new ProcessStartInfo(path) { UseShellExecute = true })?.Dispose();
                        return;
                    }
                    catch (System.ComponentModel.Win32Exception)
                    {
                        // No association for .md — Notepad below.
                    }
                }
                var notepad = new ProcessStartInfo("notepad.exe") { UseShellExecute = true };
                notepad.ArgumentList.Add(path);
                Process.Start(notepad)?.Dispose();
            }
            catch (Exception ex)
            {
                Debug.WriteLine(ex);
            }
        });
    }
}
