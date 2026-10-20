// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #509: Local search would not start on Windows. AiApi's constructor
// measured its table's required size as &((MvAiApi*)null)->EachLocation, a field
// address through a null pointer; the JIT checked it, so every attach threw a
// NullReferenceException, and the alert kept only the type's name.
using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using MediaViewer.Shared;

namespace MediaViewer.Chrome.Tests;

public class AddonAttachTests
{
    // A pointer local set to null, then a field address taken through it.
    private static readonly Regex NullPointerLocal = new(@"\b\w+\s*\*\s*(\w+)\s*=\s*null\s*;", RegexOptions.Compiled);

    [Fact]
    public void NoManagedSourceTakesAFieldAddressThroughANullPointer()
    {
        var offenders = new List<string>();
        foreach (string dir in new[] { "MediaViewer.Interop", "MediaViewer.Chrome", "MediaViewer.Ai.Chrome", "MediaViewer.Import.Chrome", "Shared" })
        {
            foreach (string file in Directory.EnumerateFiles(Repo.Path("src.managed", dir), "*.cs", SearchOption.AllDirectories))
            {
                if (file.Contains($"{System.IO.Path.DirectorySeparatorChar}obj{System.IO.Path.DirectorySeparatorChar}") ||
                    file.Contains($"{System.IO.Path.DirectorySeparatorChar}bin{System.IO.Path.DirectorySeparatorChar}")) continue;
                string text = SourceText.StripComments(File.ReadAllText(file));
                // The literal form, and a null pointer local used the same way.
                if (Regex.IsMatch(text, @"&\s*\(\s*\(\s*\w+\s*\*\s*\)\s*(null|0)\s*\)\s*->"))
                    offenders.Add(System.IO.Path.GetFileName(file));
                foreach (Match m in NullPointerLocal.Matches(text))
                {
                    if (Regex.IsMatch(text, @"&\s*" + Regex.Escape(m.Groups[1].Value) + @"\s*->"))
                        offenders.Add($"{System.IO.Path.GetFileName(file)} ({m.Groups[1].Value})");
                }
            }
        }
        Assert.Empty(offenders);
    }

    [Fact]
    public void AiApiMeasuresItsTableOnARealValue()
    {
        string ai = SourceText.StripComments(Repo.Read("src.managed", "MediaViewer.Interop", "Ai.cs"));
        Assert.Matches(@"MvAiApi\s+\w+\s*=\s*default\s*;", ai);
        Assert.Matches(@"_api->StructSize\s*<\s*RequiredSize", ai);
    }

    private sealed class Holder
    {
        public string? Name { get; init; }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int Inner(Holder h) => h.Name!.Length;

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int Outer(Holder h) => Inner(h);

    private static Exception Caught(Action a)
    {
        try { a(); }
        catch (Exception ex) { return ex; }
        throw new InvalidOperationException("did not throw");
    }

    [Fact]
    public void DescribeNamesTheTypeAndTheThrowingMethodsInnermostFirst()
    {
        Exception ex = Caught(() => Outer(new Holder()));
        string d = ExceptionFrames.Describe(ex);
        Assert.StartsWith("NullReferenceException at AddonAttachTests.Inner ← AddonAttachTests.Outer", d);
    }

    [Fact]
    public void DescribeCarriesNoMessageNoFileAndNoLine()
    {
        const string secret = @"C:\Users\someone\Pictures\IMG_0001.JPG";
        Exception ex = Caught(() => throw new IOException(secret));
        string d = ExceptionFrames.Describe(ex);
        Assert.DoesNotContain("IMG_0001", d);
        Assert.DoesNotContain(@":\", d);
        Assert.DoesNotContain(".cs", d);
        Assert.DoesNotContain(":line", d);
        Assert.StartsWith("IOException at ", d);
    }

    [Fact]
    public void ALambdaIsNamedByTheTypeItWasWrittenIn()
    {
        Exception ex = Caught(() => throw new InvalidOperationException());
        Assert.StartsWith("InvalidOperationException at AddonAttachTests.", ExceptionFrames.Describe(ex));
        Assert.StartsWith("InvalidOperationException at AddonAttachTests.ALambdaIsNamedByTheTypeItWasWrittenIn (lambda)", ExceptionFrames.Describe(ex));
        Assert.DoesNotContain("<", ExceptionFrames.Describe(ex));
    }

    [Fact]
    public void AWrappingExceptionIsUnwrappedToTheOneThatThrew()
    {
        Exception inner = Caught(() => Outer(new Holder()));
        var wrapped = new System.Reflection.TargetInvocationException(inner);
        Assert.StartsWith("NullReferenceException at AddonAttachTests.Inner", ExceptionFrames.Describe(wrapped));
    }

    [Fact]
    public void FramesStopAtTheLimit()
    {
        Exception ex = Caught(() => Outer(new Holder()));
        Assert.Equal("AddonAttachTests.Inner", ExceptionFrames.Methods(ex, 1));
        Assert.Equal("", ExceptionFrames.Methods(new Exception(), 6));  // never thrown: no frames
    }
}
