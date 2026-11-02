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
}
