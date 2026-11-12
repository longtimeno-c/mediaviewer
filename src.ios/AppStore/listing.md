# App Store listing: MediaViewer for NAS 1.0

What was entered in App Store Connect for app 6833437185 (`io.github.longtimeno-c.mediaviewer.mobile`),
version 1.0, primary language English (U.K.), on 2026-11-12. Saved, not submitted. Screenshots and the
header artwork are in `src.ios/AppStore` on the assets branch, uploaded separately.

## Before submitting

- **Merge, then release.** The support, privacy and marketing URLs below are GitHub Pages pages from
  `packaging/site`. Pages is deployed only by release.yml's `driftwire-repo` job, on a **stable** release
  (with `MV_DRIFTWIRE_REPO=1`, which is set). Until a stable release runs after the merge, `/iphone/...`
  answers 404 and the site's front page is still the old redirect to `driftwire/`. Apple checks the
  privacy policy URL at review.
- **Review access.** The notes send the reviewer to the sample library (code SAMPLE). That needs a build
  with `claude/review-sample-library-73054c` merged, and its `sample-library` GitHub release uploaded
  (`tools/ios/sample-library.sh <work> 1 --upload`). Until both are true, SAMPLE does not work.
- **Privacy page.** With the sample library, the app also reads a GitHub release over HTTPS. The privacy
  page's "Who the app talks to" says only the NAS: add the sample library before submission.
- **Contact.** Filled in by the owner on 2026-11-12.
- **Build.** None attached. Build 1 is TestFlight internal-only, which App Store Connect will not attach
  to a version; build 2 is to be uploaded without that flag.
- **Windows.** The description and the app's own welcome screen say "Mac or Windows PC". The phone access
  card is built on the Mac only so far (`docs/DEVELOPMENT.md`, "Phone access on the Mac": the Windows card
  is not done). Either finish it before release or change "Mac or Windows PC" to "Mac" here and in the app.
- **The Synology service package** is not built yet (`docs/design/28-driftwire.md`, "Not built"). The
  listing and support page say "a Synology NAS running the MediaViewer service"; a customer needs a way to
  install it.

## Version 1.0 page

**Promotional text** (153 of 170)

> Type what you remember and the photo, or the moment in the clip, comes back from your own NAS. Search runs on the phone, so it works even with no signal.

**Description** (2305 of 4000)

```text
Type what you remember, on your phone, anywhere, and the photo or the moment in the clip comes back from your own NAS.

MediaViewer for NAS is the phone half of MediaViewer, the free photo and video viewer for Mac and Windows. MediaViewer on your computer indexes the library on your NAS. Scan one code and this app copies that index to your phone. From then on, search runs on the phone itself, and the photos and clips stream from your NAS.

WHAT YOU NEED
The app shows nothing on its own. Before you install it, check you have:
• MediaViewer on a Mac or Windows PC, with Local search and Driftwire installed, and your library indexed.
• A Synology NAS running the MediaViewer service.
• To use it away from home: a public IPv4 address with one port forwarded to the service, or a public IPv6 address with that port open. Without either, the app works only on the NAS's own network.
Setup tests the route from outside over mobile data and tells you there and then.

SEARCH WHAT'S IN THEM
• By meaning: "dog on a beach", "birthday cake", "snow in the mountains"
• Words on screen: signs, menus, whiteboards
• Words said in a clip, which plays from the moment they were said
• People you named in MediaViewer on your computer
• Place, date, camera, file name, photos or clips
Search needs no connection. On a plane or out of signal, results and small thumbnails still come up from what is on the phone.

BROWSE AND VIEW
• Your whole library by date, or folder by folder as it is on the NAS
• Photos full screen: pinch to zoom, swipe through, with a filmstrip
• Clips play from the matching moment, with the moments marked
• Details: date, place, camera, file
• Save a copy of any photo or clip to Photos

YOURS, ON YOUR OWN HARDWARE
• No account, no cloud, no subscription. The app talks only to the MediaViewer service on your NAS.
• Every connection is encrypted end to end, and the app checks it is your NAS before it asks for anything.
• Pairing shows your library's name and a check code on the phone and the computer. Pair only if they match.
• Your computer can be off: the NAS serves the phone on its own.
• The app never writes to your NAS.
• No data collected: no analytics, no tracking, no crash upload.
• Forget this library removes everything the app keeps on the phone.

For iPhone and iPad.
```

**Keywords** (97 of 100)

```text
photos,video,viewer,gallery,offline,private,people,places,clips,self-hosted,server,storage,camera
```

Words already in the name and subtitle (MediaViewer, NAS, search, photo, library) are indexed anyway, so
they are not repeated. No other company's name is used as a keyword.

| Field | Value |
|---|---|
| Support URL | https://longtimeno-c.github.io/mediaviewer/iphone/support/ |
| Marketing URL | https://longtimeno-c.github.io/mediaviewer/ |
| Copyright | 2026 Oakforge Studios LTD |
| What's New | Not shown for 1.0 |
| Sign-in required | Unticked: the app has no account |
| Version release | Left as it was (automatically release after approval) |

**App Review notes** (2262 of 4000; saved 2026-11-12, from the sample-library branch's `review-notes.md`, with the real setup described as Mac-only and the attached-QR line dropped)

```text
MediaViewer for NAS searches and views a photo and video library that lives on the person's own Synology NAS. In a real setup, MediaViewer on the person's Mac indexes their library and publishes a search catalogue to a small MediaViewer service on the NAS; the phone pairs with that service by scanning a QR code the Mac shows, then searches the catalogue on the phone and fetches photos and clips from the NAS. You do not need a Mac or a NAS to review the app: the code SAMPLE opens a signed demo library of public-domain pictures, read over HTTPS.

To open the sample library:

1. Launch the app and tap "Scan code". If iOS asks for the camera, you may allow or decline it.
2. At the bottom of the scan screen, tap "Enter a code".
3. Type SAMPLE and tap "Open". (Opening the link mediaviewer://sample does the same.)
4. The app shows "Open the sample library?". Tap "Open sample library".
5. The app copies the library's search catalogue, about 130 MB, from GitHub. On Wi-Fi this takes under a minute. Over mobile data the app asks first.
6. The library appears: 44 photos and 3 short clips. A "Sample library" label at the top of the grid shows where it comes from.

Things to try:

- Search tab: type "dog on a beach", "waterfall", "bridge at night" or "breakfast". Search runs entirely on the phone, with the same model and code as for a real library.
- Tap a photo to open it; swipe between photos; pinch to zoom.
- Open a clip from the "Clips" folder in the Library tab to play it.
- Settings tab: the sample library is named, with where it comes from. "Forget the sample library" removes everything the app keeps for it and returns to the start.

About the sample library: its photos and clips come from Wikimedia Commons and are CC0 or in the public domain. It is read from a GitHub release of this app's open-source repository, over HTTPS, and every file is checked against a signature before it is used, as a library from a NAS is. Nothing about the user is sent: the app collects no data, has no accounts and no analytics.

For a real library, a person needs MediaViewer on a Mac with its Local search and Driftwire add-ons, a Synology NAS running the MediaViewer service, and a way to reach it from the internet. The app says so on its first screen.
```

## App Information

| Field | Value |
|---|---|
| Name | MediaViewer for NAS (already set) |
| Subtitle | Search your NAS photo library (29 of 30) |
| Category | Photo & Video; secondary Utilities |
| Content rights | No, it does not contain, show, or access third-party content |
| Age rating | 4+ (all questions No or None; age category Not Applicable) |

Age rating answers: no parental controls or age assurance; no unrestricted web access, user-generated
content, social media, messaging or advertising; no mature themes, medical or wellness content, sexuality,
violence, gambling, contests or loot boxes. The app shows only the person's own library.

## App Privacy (published)

- Privacy Policy URL: https://longtimeno-c.github.io/mediaviewer/iphone/privacy/
- Data collection: **No, we do not collect data from this app** ("Data Not Collected"), as the owner
  approved. Matches `src.ios/App/MediaViewer/PrivacyInfo.xcprivacy` (no tracking, no collected types).

## Pricing and Availability

- Price: free ($0.00 in every storefront; base country left as United States).
- Availability: 174 countries or regions, every one except **France**, per the owner's export-compliance
  answer recorded in `src.ios/App/Info/MediaViewer.plist` (2026-11-11, call 10: not distributed in France
  until the French encryption declaration is done). Add France there once it is.

## Where the privacy page's claims come from

| Claim | Code |
|---|---|
| No tracking, nothing collected | `App/MediaViewer/PrivacyInfo.xcprivacy`; no analytics or crash SDK in `src.ios`; ONNX Runtime's uploader off (`src/infer/ort_static_ios.cpp`) |
| Talks only to the NAS service, encrypted, checked identity | `MobileLink/LinkConnection.swift`, `src/mobile/link.cpp`; `docs/design/28-driftwire.md` (Noise XXhfs+psk0, paired list) |
| One mobile-data test of the public name | `OutsideRoute.test` in `LinkConnection.swift` |
| Never writes to the NAS | 1.0 has read operations only (`roots`, `list`, `stat`, `read_range`) |
| Camera only for the code | `SetupScreens.swift` (AVCapture), `NSCameraUsageDescription` |
| Local network | `App/Info/MediaViewer.plist` `NSLocalNetworkUsageDescription` |
| Photos add-only, on Save | `App/MediaViewer/PhotoLibrarySaver.swift` (`requestAuthorization(for: .addOnly)`) |
| What is stored; out of backups; data protection | `MobileLink/LinkCore.swift` (`LinkFolders.prepare`), `MobileCore/LocalCatalogue.swift` (`excludeFromBackup`, `recent-searches.json`, 20 kept) |
| Keychain, this device only | `MobileLink/Keychain.swift` (`kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`) |
| Forget removes it all | `LinkConnection.removeLibrary` (library folder, thumbs, originals, clips, pairing record, device key) |
