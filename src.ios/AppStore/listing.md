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
- **Review access.** The notes hold a marked placeholder for the review library (being built on
  `claude/review-sample-library-73054c`). Replace it with the code or link and the steps.
- **Contact.** First name, last name, phone and email under App Review Information are the owner's to
  fill in.
- **Build.** No build is attached to version 1.0 yet ("Add Build" on the version page).
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

**App Review notes** (1443 of 4000)

```text
MediaViewer for NAS is the phone companion to MediaViewer, a free photo and video viewer for Mac and Windows. There is no account and no sign-in. The app shows a library that MediaViewer on a computer has indexed and that a MediaViewer service on the person's own Synology NAS serves. The phone pairs with that service by scanning a QR code shown by MediaViewer on the computer (or by choosing a picture of the code, or pasting its link).

>>> REVIEW ACCESS: TO BE FILLED IN BEFORE SUBMISSION <<<
[A review library of sample photos only, its code or link, and the steps to pair with it go here.]

Once paired:
1. The app copies the library's catalogue. Search then runs on the phone: try "dog on a beach", or a place or a year.
2. Tap a result to view it; swipe through results; pinch to zoom.
3. Library tab: browse by date or by folder. Clips play from the matching moment.
4. In the viewer, the download button at the top right saves a copy to Photos.
5. Settings shows the connection and "Forget this library".

Permissions: Camera, only to scan the pairing code. Local network, to reach the NAS on the same network. Photos add-only, only when the person taps Save to Photos.

The app collects no data and talks only to the person's own NAS service. Encryption: standard algorithms (Noise protocol with libsodium), declared exempt in the build (ITSAppUsesNonExemptEncryption = NO).

Source code: https://github.com/longtimeno-c/mediaviewer
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
