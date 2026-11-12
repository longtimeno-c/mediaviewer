# App Review notes: MediaViewer for NAS

The text below goes in App Store Connect, App Review Information, Notes, for every submission. No
sign-in is needed, so the demo account fields stay empty. How the sample library works and how to
make it again: `docs/plans/mobile.md` §11.1 and `docs/DEVELOPMENT.md`, "The sample library".

Before each submission, check that the GitHub release `sample-library` is still published: the
code below reads it.

---

MediaViewer for NAS searches and views a photo and video library that lives on the person's own
Synology NAS. Normally the person scans a pairing code shown by MediaViewer on their Mac or Windows
PC, and the phone then connects to a small MediaViewer service on the NAS. You do not need a Mac,
a PC or a NAS to review the app: it has a built-in sample library, available to everyone, opened by
the code SAMPLE.

To open the sample library:

1. Launch the app and tap "Scan code". If iOS asks for the camera, you may allow or decline it.
2. At the bottom of the scan screen, tap "Enter a code".
3. Type SAMPLE and tap "Open". (Scanning the QR code attached to these notes, or opening the link
   mediaviewer://sample, does the same.)
4. The app shows "Open the sample library?". Tap "Open sample library".
5. The app copies the library's search catalogue, about 130 MB, from GitHub. On Wi-Fi this takes
   under a minute. Over mobile data the app asks first.
6. The library appears: 44 photos and 3 short clips. A "Sample library" label at the top of the grid
   shows where it comes from.

Things to try:

- Search tab: type "dog on a beach", "waterfall", "bridge at night" or "breakfast". Search runs
  entirely on the phone, with the same model and code as for a real library.
- Tap a photo to open it; swipe between photos; pinch to zoom.
- Open a clip from the "Clips" folder in the Library tab to play it.
- Settings tab: the sample library is named, with where it comes from. "Forget the sample library"
  removes everything the app keeps for it and returns to the start.

About the sample library: its photos and clips come from Wikimedia Commons and are CC0 or in the
public domain. It is read from a GitHub release of this app's open-source repository, over HTTPS,
and checked against a signature before it is used, exactly as a library from a NAS is. Nothing about
the user is sent: the app collects no data, has no accounts and no analytics.

For a real library, a person needs MediaViewer on a Mac or Windows PC with its Local search and
Driftwire add-ons, a Synology NAS running the MediaViewer service, and a way to reach it from the
internet. The app says so on its first screen.
