// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.Text;

namespace MediaViewer.Shared;

/// <summary>
/// Where an exception came from, safe to show and to keep (issue #509): its
/// type and the methods of its top frames, as <c>Type.Method</c>. Never a
/// message, a file name or a line (rule 6: a .NET message can carry a path).
/// </summary>
internal static class ExceptionFrames
{
    /// <summary>"NullReferenceException at AiChrome.ReadStatus ← AiChrome.Attach ← …",
    /// the innermost exception's frames first, at most <paramref name="max"/> of them.</summary>
    public static string Describe(Exception ex, int max = 6)
    {
        Exception inner = ex;
        // A TargetInvocationException / TypeInitializationException wraps the real one.
        while (inner.InnerException is not null &&
               inner is System.Reflection.TargetInvocationException or TypeInitializationException or AggregateException)
        {
            inner = inner.InnerException;
        }
        var sb = new StringBuilder(inner.GetType().Name);
        string frames = Methods(inner, max);
        if (frames.Length > 0) sb.Append(" at ").Append(frames);
        return sb.ToString();
    }

    /// <summary>The top frames' <c>DeclaringType.Method</c>, joined by " ← ".</summary>
    public static string Methods(Exception ex, int max = 6)
    {
        StackFrame[] frames;
        try { frames = new StackTrace(ex, fNeedFileInfo: false).GetFrames(); }
        catch (Exception) { return ""; }
        var names = new List<string>(Math.Min(max, frames.Length));
        foreach (StackFrame f in frames)
        {
            if (names.Count >= max) break;
            System.Reflection.MethodBase? m = f.GetMethod();
            if (m is null) continue;
            names.Add($"{TypeName(m.DeclaringType)}.{MethodName(m.Name)}");
        }
        return string.Join(" ← ", names);
    }

    // A lambda or local function is "<Outer>b__8_0" / "<Outer>g__Local|3_0":
    // the method it was written in, marked.
    private static string MethodName(string name)
    {
        if (!name.StartsWith('<')) return name;
        int end = name.IndexOf('>');
        if (end <= 1) return name;
        int local = name.IndexOf("g__", end, StringComparison.Ordinal);
        if (local > 0)
        {
            int bar = name.IndexOf('|', local);
            string inner = bar > local ? name[(local + 3)..bar] : name[(local + 3)..];
            return $"{name[1..end]}.{inner}";
        }
        return $"{name[1..end]} (lambda)";
    }

    // A lambda's or local function's closure class is nested in the type it was
    // written in: name that one, as "AiChrome+<>c" says nothing more useful.
    private static string TypeName(Type? t)
    {
        if (t is null) return "?";
        while (t.DeclaringType is not null && t.Name.StartsWith('<')) t = t.DeclaringType;
        return t.Name;
    }
}
