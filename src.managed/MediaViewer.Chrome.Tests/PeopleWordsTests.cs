// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Correcting people (docs/design/17 "People"): the AI pack's JSON parsed, and
// the person sheet's words, as the Windows chrome uses them
// (src.managed/Shared/PeopleWords.cs). Strings match the Mac's
// PeopleCorrections.swift.
using MediaViewer.Shared;

namespace MediaViewer.Chrome.Tests;

public class PeopleWordsTests
{
    private const string Review = """
        [{"face":9,"path":"C:\\p\\a.jpg","pts_ms":-1,"box":[0.1,0.2,0.3,0.4],"score":0.9,"pinned":false,
          "quality":0.4,"support":0.42,"rival":0.31,"rival_id":12,"rival_name":"Ben","check":true,"doubtful_pin":false},
         {"face":10,"path":"C:\\p\\b.mp4","pts_ms":1500,"box":[0,0,1,1],"score":0.8,"pinned":true,
          "quality":0.9,"support":0.2,"rival":-1,"rival_id":0,"rival_name":"","check":true,"doubtful_pin":true},
         {"face":11,"path":"C:\\p\\c.jpg","pts_ms":-1,"box":[0,0,1,1],"score":0.7,"pinned":false,
          "quality":0.7,"support":0.1,"rival":-1,"rival_id":0,"rival_name":"","check":true,"doubtful_pin":false},
         {"face":12,"path":"C:\\p\\d.jpg","pts_ms":-1,"box":[0,0,1,1],"score":0.95,"pinned":false,
          "quality":0.2,"support":0.9,"rival":-1,"rival_id":0,"rival_name":"","check":false,"doubtful_pin":false},
         {"face":13,"path":"C:\\p\\e.jpg","pts_ms":-1,"box":[0,0,1,1],"score":0.95,"pinned":false,
          "quality":0.8,"support":0.9,"rival":-1,"rival_id":0,"rival_name":"","check":false,"doubtful_pin":false}]
        """;

    [Fact]
    public void ReviewRowsParseWithEveryField()
    {
        List<ReviewFace> faces = PeopleWords.ParseReview(Review);
        Assert.Equal(5, faces.Count);
        ReviewFace a = faces[0];
        Assert.Equal(9UL, a.Face);
        Assert.Equal(new[] { 0.1, 0.2, 0.3, 0.4 }, a.Box);
        Assert.True(a.Check);
        Assert.Equal(12UL, a.RivalId);
        Assert.Equal("Ben", a.RivalName);
        Assert.Equal(1500, faces[1].PtsMs);
        Assert.True(faces[1].Pinned);
        Assert.True(faces[1].DoubtfulPin);
    }

    [Fact]
    public void AnOlderPacksFacesParseWithoutReviewFields()
    {
        List<ReviewFace> faces = PeopleWords.ParseReview("""[{"face":3,"path":"x.jpg","pts_ms":-1,"box":[0,0,1,1]},{"face":0}]""");
        ReviewFace f = Assert.Single(faces);
        Assert.False(f.Check);
        Assert.False(f.Pinned);
        Assert.Equal(-1, f.Support);
        Assert.Equal("", f.RivalName);
    }

    [Fact]
    public void CheckCaptionsSayWhyAFaceIsThere()
    {
        List<ReviewFace> faces = PeopleWords.ParseReview(Review);
        Assert.Equal("Looks like Ben?", PeopleWords.CheckCaption(faces[0]));
        Assert.Equal("Pinned, but unlike the others", PeopleWords.CheckCaption(faces[1]));
        Assert.Equal("Not sure", PeopleWords.CheckCaption(faces[2]));
        Assert.Equal("Check these (3)", PeopleWords.CheckHeader(3));
    }

    [Fact]
    public void DoubtfulFirstKeepsThePacksOrderAndClearestSortsEachGroup()
    {
        List<ReviewFace> faces = PeopleWords.ParseReview(Review);
        (List<ReviewFace> check, List<ReviewFace> others) = PeopleWords.Group(faces, clearest: false);
        Assert.Equal(new ulong[] { 9, 10, 11 }, check.Select(f => f.Face));
        Assert.Equal(new ulong[] { 12, 13 }, others.Select(f => f.Face));
        (check, others) = PeopleWords.Group(faces, clearest: true);
        Assert.Equal(new ulong[] { 10, 11, 9 }, check.Select(f => f.Face));
        Assert.Equal(new ulong[] { 13, 12 }, others.Select(f => f.Face));
    }

    [Fact]
    public void ARejectSaysWhereTheFacesWent()
    {
        RejectOutcome o = PeopleWords.ParseReject("""{"unassigned":1,"spread":3,"to":[{"id":7,"name":"Carl","faces":2}]}""");
        Assert.Equal("2 now with Carl, 1 unknown, 3 frames of the same clip went too", PeopleWords.RejectSummary(o, 3));
        o = PeopleWords.ParseReject("""{"unassigned":0,"spread":1,"to":[{"id":7,"name":"","faces":1}]}""");
        Assert.Equal("1 now with someone unnamed, 1 frame of the same clip went too", PeopleWords.RejectSummary(o, 1));
        o = PeopleWords.ParseReject("""{"unassigned":0,"spread":0,"to":[]}""");
        Assert.Equal("2 faces removed", PeopleWords.RejectSummary(o, 2));
    }

    [Fact]
    public void ActionWordsNameThePerson()
    {
        Assert.Equal("Yes, this is Anna", PeopleWords.YesLabel("Anna"));
        Assert.Equal("Yes, same person", PeopleWords.YesLabel(""));
        Assert.Equal("Not Anna", PeopleWords.NotLabel("Anna"));
        Assert.Equal("Moved 2 to Carl", PeopleWords.Moved(2, "Carl"));
        Assert.Equal("Moved 2 to Carl, 1 to Ben", PeopleWords.MovedEach(new[] { ("Carl", 2), ("Ben", 1) }));
        Assert.Equal("Might also be Anna", PeopleWords.MightAlsoBe("Anna"));
        Assert.Equal($"Unknown faces ({1204:N0})", PeopleWords.UnknownEntry(1204));
        Assert.Equal("Anna?", PeopleWords.SuggestCaption(7, "Anna"));
        Assert.Equal("", PeopleWords.SuggestCaption(0, "Anna"));
    }

    [Fact]
    public void UndoPutsFacesBackWithTheirPinsInTwoGroups()
    {
        (List<ulong> pinned, List<ulong> unpinned) = PeopleWords.ByPin(new ulong[] { 1, 2, 3, 4 }, new HashSet<ulong> { 2, 4 });
        Assert.Equal(new ulong[] { 2, 4 }, pinned);
        Assert.Equal(new ulong[] { 1, 3 }, unpinned);
    }

    [Fact]
    public void ThePickerListsNamedPeopleFirstAndSearchesByName()
    {
        var people = new[]
        {
            new PickPerson(1, "", 40), new PickPerson(2, "carl", 3), new PickPerson(3, "Anna", 9),
            new PickPerson(4, "", 90), new PickPerson(5, "Ben", 1),
        };
        Assert.Equal(new ulong[] { 3, 2, 4, 1 }, PeopleWords.PickerOrder(people, "", except: 5).Select(p => p.Id));
        Assert.Equal(new ulong[] { 3 }, PeopleWords.PickerOrder(people, " an ", except: 0).Select(p => p.Id));
        Assert.Equal("Unnamed (40 photos)", PeopleWords.PickName(people[0]));
    }

    [Fact]
    public void UnknownFacesAndSuggestionsParse()
    {
        UnknownPage page = PeopleWords.ParseUnknown(
            """{"total":61,"faces":[{"face":5,"path":"a.jpg","pts_ms":-1,"box":[0,0,0.5,0.5],"quality":0.8,"suggest":7,"suggest_name":"Anna","score":0.27}]}""");
        Assert.Equal(61, page.Total);
        UnknownFace f = Assert.Single(page.Faces);
        Assert.Equal(7UL, f.Suggest);
        Assert.Equal("Anna", f.SuggestName);

        Suggestions s = PeopleWords.ParseSuggest(
            """{"people":[{"id":4,"score":0.6,"faces":12,"cover_face":40,"cover_path":"b.jpg","cover_ms":-1,"cover_box":[0,0,1,1]}],"faces":[{"face":41,"path":"c.jpg","pts_ms":200,"box":[0,0,1,1],"score":0.5}]}""");
        Assert.False(s.IsEmpty);
        Assert.Equal(40UL, Assert.Single(s.People).CoverFace);
        Assert.Equal(200, Assert.Single(s.Faces).PtsMs);
        Assert.True(PeopleWords.ParseSuggest("""{"people":[],"faces":[]}""").IsEmpty);
    }

    [Fact]
    public void ArrowsHoldAtTheEnds()
    {
        Assert.Equal(0, PeopleWords.Step(-1, 5, 1));
        Assert.Equal(4, PeopleWords.Step(-1, 5, -1));
        Assert.Equal(4, PeopleWords.Step(4, 5, 1));
        Assert.Equal(0, PeopleWords.Step(0, 5, -1));
        Assert.Equal(2, PeopleWords.Step(1, 5, 1));
        Assert.Equal(-1, PeopleWords.Step(0, 0, 1));
    }

    [Fact]
    public void RejectUndoPutsEveryFaceBackWithItsPin()
    {
        RejectOutcome o = PeopleWords.ParseReject(
            "{\"unassigned\":1,\"spread\":2,\"to\":[{\"id\":7,\"name\":\"Ben\",\"faces\":2}]," +
            "\"faces\":[{\"face\":9,\"from\":3,\"to\":7,\"pinned\":true},{\"face\":10,\"from\":3,\"to\":7,\"pinned\":false}," +
            "{\"face\":11,\"from\":3,\"to\":0,\"pinned\":false}]}");
        Assert.Equal(3, o.Left.Count);
        var groups = PeopleWords.PutBackGroups(o);
        Assert.All(groups, g => Assert.Equal(3UL, g.Person));
        Assert.Equal(new ulong[] { 9 }, groups.Single(g => g.Pinned).Faces);
        Assert.Equal(new ulong[] { 10, 11 }, groups.Single(g => !g.Pinned).Faces);
        Assert.Empty(PeopleWords.PutBackGroups(PeopleWords.ParseReject("{\"unassigned\":1,\"spread\":0,\"to\":[]}")));
    }
    // ---- Refine faces / Merge duplicates in detail ----------------------------------------

    private const string RefineLook = """
        {"dry":true,"removed":3,"faces":[
          {"face":21,"path":"a.jpg","pts_ms":-1,"box":[0.1,0.1,0.2,0.2],"why":"move","to":7,"group":0,"to_name":"Ben"},
          {"face":22,"path":"b.mp4","pts_ms":4000,"box":[0,0,1,1],"why":"evict","to":0,"group":0,"to_name":""},
          {"face":23,"path":"c.jpg","pts_ms":-1,"box":[0,0,1,1],"why":"regroup","to":0,"group":1,"to_name":""},
          {"face":24,"path":"d.jpg","pts_ms":-1,"box":[0,0,1,1],"why":"move","to":9,"group":0,"to_name":""},
          {"face":0}]}
        """;

    [Fact]
    public void RefinesLookSaysWhereEachFaceWouldGo()
    {
        RefineOutcome o = PeopleWords.ParseRefine(RefineLook);
        Assert.True(o.Dry);
        Assert.Equal(4, o.Faces.Count);
        Assert.Equal(4, o.Count);
        Assert.Equal(4000, o.Faces[1].PtsMs);
        Assert.Equal("To Ben", PeopleWords.RefineCaption(o.Faces[0]));
        Assert.Equal("To nobody", PeopleWords.RefineCaption(o.Faces[1]));
        Assert.Equal("A new person", PeopleWords.RefineCaption(o.Faces[2]));
        Assert.Equal("To someone unnamed", PeopleWords.RefineCaption(o.Faces[3]));
        Assert.Equal("4 faces would leave Anna", PeopleWords.RefineLookHeader(o.Count, "Anna"));
        Assert.Equal("1 face would leave this person", PeopleWords.RefineLookHeader(1, ""));
    }

    [Fact]
    public void RefineAppliedSaysHowManyLeftAndPutsEachBackOnce()
    {
        RefineOutcome o = PeopleWords.ParseRefine("""
            {"dry":false,"removed":2,"faces":[{"face":21,"to":7},{"face":22,"to":0},{"face":21,"to":7}]}
            """);
        Assert.False(o.Dry);
        Assert.Equal(new ulong[] { 21, 22 }, PeopleWords.RefinePutBack(o));
        Assert.Equal("3 faces left", PeopleWords.RefineLeft(o.Count));
        Assert.Equal("1 face left", PeopleWords.RefineLeft(1));
        Assert.Equal("Every face matches", PeopleWords.RefineLeft(0));
        // A count without the list: nothing to put back.
        o = PeopleWords.ParseRefine("""{"dry":false,"removed":2}""");
        Assert.Equal(2, o.Count);
        Assert.Empty(PeopleWords.RefinePutBack(o));
    }

    private const string DedupeReal = """
        {"dry":false,"merged":3,"moved":12,"merges":[
          {"into":3,"into_name":"Anna","into_faces":40,"cover_face":30,"cover_path":"a.jpg","cover_ms":-1,"cover_box":[0,0,1,1],
           "from":[{"id":9,"name":"","faces":4,"cover_face":90,"cover_path":"b.jpg","cover_ms":-1,"cover_box":[0,0,1,1],"undo":101},
                   {"id":10,"name":"","faces":1,"cover_face":91,"cover_path":"c.mp4","cover_ms":2000,"cover_box":[0,0,1,1],"undo":0}]},
          {"into":5,"into_name":"","into_faces":7,"cover_face":50,
           "from":[{"id":11,"name":"","faces":2,"cover_face":92,"undo":102}]},
          {"into":6,"from":[]}]}
        """;

    [Fact]
    public void DedupeParsesEveryMergeWithCoversAndTokens()
    {
        DedupeOutcome o = PeopleWords.ParseDedupe(DedupeReal);
        Assert.False(o.Dry);
        Assert.Equal(2, o.Merges.Count);  // a merge of nobody is dropped
        DedupeMerge m = o.Merges[0];
        Assert.Equal(3UL, m.Into.Id);
        Assert.Equal("Anna", m.Into.Name);
        Assert.Equal(40, m.Into.Faces);
        Assert.Equal(30UL, m.Into.CoverFace);
        Assert.Equal(2000, m.From[1].CoverMs);
        Assert.Equal("Merge 3 people into Anna", PeopleWords.MergeLine(m));
        Assert.Equal("Merge 2 people into one", PeopleWords.MergeLine(o.Merges[1]));
        Assert.Equal("Anna · 40 faces", PeopleWords.DedupeCaption(m.Into));
        Assert.Equal("Unnamed · 1 face", PeopleWords.DedupeCaption(m.From[1]));
    }

    [Fact]
    public void DedupeUndoesEveryMergeLastFirst()
    {
        DedupeOutcome o = PeopleWords.ParseDedupe(DedupeReal);
        Assert.Equal(new ulong[] { 102, 101 }, PeopleWords.DedupeUndoTokens(o));
        Assert.Equal("3 people merged, 12 faces moved.", PeopleWords.DedupeSummary(o.Merged, o.Moved));
        Assert.Equal("1 person merged, 1 face moved.", PeopleWords.DedupeSummary(1, 1));
        Assert.Equal("No duplicates found, and every face matches.", PeopleWords.DedupeSummary(0, 0));
    }

    [Fact]
    public void DedupesLookSaysWhatWouldChange()
    {
        DedupeOutcome o = PeopleWords.ParseDedupe("""{"dry":true,"merged":2,"moved":5,"merges":[]}""");
        Assert.True(o.Dry);
        Assert.False(o.IsEmpty);
        Assert.Equal("2 people would merge, 5 faces would move.", PeopleWords.DedupeLookHeader(o));
        o = PeopleWords.ParseDedupe("""{"dry":true,"merged":0,"moved":1,"merges":[]}""");
        Assert.Equal("1 face would move.", PeopleWords.DedupeLookHeader(o));
        o = PeopleWords.ParseDedupe("""{"dry":true,"merged":0,"moved":0,"merges":[]}""");
        Assert.True(o.IsEmpty);
        Assert.Equal("No duplicates found, and every face matches.", PeopleWords.DedupeLookHeader(o));
    }
}
