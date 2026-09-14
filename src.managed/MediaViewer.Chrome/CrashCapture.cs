// SPDX-License-Identifier: GPL-2.0-or-later
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace MediaViewer.Chrome;

/// <summary>
/// Managed half of crash reporting (plan/13 Part 2, "two capture paths"). A
/// native crash is a Crashpad minidump; an unhandled managed exception is a
/// small text report in the same %LocalAppData%\MediaViewer\Crashes folder,
/// under managed\. Nothing here uploads.
/// </summary>
/// <remarks>
/// Privacy (rule 6): exception type, HResult, stack frames and the last native
/// correlation id. Messages and stacks are scrubbed of path-like substrings,
/// media filenames and the username/computer name before they touch disk —
/// an IOException message routinely carries the path of the user's photo.
/// </remarks>
internal static class CrashCapture
{
    private static int _installed;

    [DllImport("mediaviewer_core", CallingConvention = CallingConvention.Cdecl)]
    private static extern ulong mv_last_error_correlation_id();

    // Drive/UNC paths up to a quote, angle bracket, pipe or line end.
    private static readonly Regex PathLike = new(
        @"(?<![A-Za-z0-9])(?:[A-Za-z]:[\\/]|\\\\)[^""<>|\r\n]*",
        RegexOptions.CultureInvariant | RegexOptions.Compiled);

    private static readonly Regex MediaName = new(
        @"[^\\/:*?""<>|\r\n\s]+\.(jpe?g|jfif|png|bmp|gif|tiff?|webp|hei[cf]|avif|ico|dng|cr[23w]|nef|nrw|arw|sr[f2]|raf|orf|rw2|pef|srw|x3f|mp4|m4v|mov|mkv|webm|avi|m?ts|m2ts|xmp|aae)\b",
        RegexOptions.CultureInvariant | RegexOptions.IgnoreCase | RegexOptions.Compiled);

    public static void Install()
    {
        if (Interlocked.Exchange(ref _installed, 1) == 1) return;
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
            Write("AppDomain.UnhandledException", e.ExceptionObject as Exception);
        TaskScheduler.UnobservedTaskException += (_, e) =>
            Write("TaskScheduler.UnobservedTaskException", e.Exception);
    }

    /// <summary>XAML's Application.UnhandledException, when an Application exists.</summary>
    public static void Attach(Microsoft.UI.Xaml.Application? app)
    {
        if (app is null) return;
        app.UnhandledException += (_, e) => Write("Application.UnhandledException", e.Exception);
    }

    internal static string Scrub(string text)
    {
        if (string.IsNullOrEmpty(text)) return string.Empty;
        string s = PathLike.Replace(text, m => m.Value.Length <= 3 ? m.Value : m.Value[..3] + "<path>");
        s = MediaName.Replace(s, m => "<file>." + m.Groups[1].Value);
        foreach (string id in new[] { Environment.UserName, Environment.MachineName })
        {
            if (id.Length >= 3)
                s = Regex.Replace(s, @"(?<![A-Za-z0-9])" + Regex.Escape(id) + @"(?![A-Za-z0-9])",
                    "%USER%", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        }
        return s;
    }

    private static void Write(string source, Exception? ex)
    {
        try
        {
            string dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "MediaViewer", "Crashes", "managed");
            Directory.CreateDirectory(dir);

            ulong correlation = 0;
            try { correlation = mv_last_error_correlation_id(); } catch { /* core not loaded */ }

            var sb = new StringBuilder();
            sb.AppendLine("MediaViewer managed exception report");
            sb.AppendLine($"source: {source}");
            sb.AppendLine($"utc: {DateTime.UtcNow:O}");
            sb.AppendLine($"runtime: {RuntimeInformation.FrameworkDescription}");
            sb.AppendLine($"last_native_correlation_id: {correlation}");
            for (Exception? e = ex; e is not null; e = e.InnerException)
            {
                sb.AppendLine($"type: {e.GetType().FullName}");
                sb.AppendLine($"hresult: 0x{e.HResult:X8}");
                sb.AppendLine($"message: {Scrub(e.Message)}");
                sb.AppendLine("stack:");
                sb.AppendLine(Scrub(e.StackTrace ?? string.Empty));
            }
            string name = $"{DateTime.UtcNow:yyyyMMdd'T'HHmmss'Z'}-{Environment.ProcessId}.txt";
            File.WriteAllText(Path.Combine(dir, name), sb.ToString());
        }
        catch
        {
            // A crash reporter that throws while reporting hides the original.
        }
    }
}
