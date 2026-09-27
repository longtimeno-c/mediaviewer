# System appearance verification

The UI follows the system's light/dark appearance. **Canvas background** defaults
to System, including the gutters around images, transparent pixels and video.
Grey, White, Checkerboard and Dark remain fixed choices; changing appearance
must not alter a photo, its adjustments or the saved choice.

## Implementation

- macOS uses dynamic AppKit window, control, label and separator colours in
  SwiftUI. Settings and popovers inherit the hosting window's appearance.
  Existing system-material panes retain their native appearance behaviour.
- Windows islands use the default system theme for controls. Shared semantic
  brushes update on the UI dispatcher after `UISettings.ColorValuesChanged`
  or `AccessibilitySettings.HighContrastChanged`; existing controls and open
  flyouts keep their focus/state. The native title bar follows the same update.
- Windows contrast themes use the user's window, window-text and highlight
  colours. Selected folder tiles retain readable window text and an outline.
- Import follows the system in both hosts; its Windows root keeps a live
  `ThemeResource` expression instead of sampling a brush once.
- The native canvas receives the window colour through the UI-thread snapshot.
  The empty welcome, 2D/3D runner and default image/video surround follow it;
  image shader backgrounds use the same linear RGB as the swapchain clear.
- Histogram backgrounds and luminance traces adapt; RGB traces still identify
  their channels. White-on-black captions over thumbnails remain paired for
  readability against arbitrary images.

## Build checks

CI compiles both the main chrome and Import chrome on Windows and macOS:

```sh
swift build --package-path src.swift/MediaViewerChrome -c release
swift build --package-path src.swift/ImportChrome -c release
```

```powershell
dotnet build src.managed/MediaViewer.Chrome/MediaViewer.Chrome.csproj -c Release
dotnet build src.managed/MediaViewer.Import.Chrome/MediaViewer.Import.Chrome.csproj -c Release
```

## Interactive checks (both platforms)

Run on a native desktop; a successful compile is not a visual or GPU result.

| Check | Expected result |
| --- | --- |
| Start with the system in light mode, then dark mode | Title bar, toolbar, paths, settings and Import match the system; labels and separators remain legible. |
| Open with no media; play the 2D and 3D dinosaur with Space and 3, switch system appearance in each state | Empty canvas, welcome card, runner, track and HUD adopt the system appearance without restarting; the idle view redraws once rather than presenting continuously. |
| Open a fitted image with wide gutters and an image with transparent pixels, select System, then switch light → dark → light | Gutters, transparent pixels and the clear colour change together without a dark rectangle or one-frame mismatch; the image's actual colours do not change. Repeat while a video is playing and paused. |
| Open each menu and Settings category | Hover, pressed, disabled and keyboard-focus states are visible in both appearances. |
| Open gallery, filmstrip, folder tree and metadata | Empty states, folder tiles, captions, selected/marked items and metadata tabs remain readable. |
| Open Adjust, Export, video transport, clip tools and Jobs | Histograms, controls, trim markers, job progress and dialogue text remain readable. |
| Switch light → dark → light while Settings, a flyout or a text field is open | Existing UI updates without restarting, closing the overlay, losing keyboard focus or resetting typed text. |
| Switch appearance while Import is open and while a job runs | The window changes appearance without restarting the scan/copy or resetting selection. |
| Change the system accent colour | Native control accents update; Windows trim/rating accents update with the palette. |
| Enable Windows contrast themes, including a light-background scheme | Surfaces/text use the selected system colours; selected items and hover/focus remain distinguishable. |
| Enable macOS Increase contrast and Reduce transparency | Dynamic colours and system materials remain readable. |
| Select Grey, White, Checkerboard and Dark in turn; toggle system appearance, restart and press B through all five modes | Explicit backgrounds remain fixed, the saved choice survives restart, System follows the OS again, and the shortcut cycles to each mode. |

Inherited PR 3 verify: **zero dropped frames while panning a cached image at
the display's refresh rate**; focus/tab traversal must cross island boundaries,
and flyouts must open over the canvas without clipping. Run the existing PR 1
animated/idle GPU gates on both native platforms. A skipped GPU job does not
satisfy that gate.

Native visual checks and GPU soaks require Windows/macOS hardware and are not
claimed by the headless source checks or the hosted chrome builds.
