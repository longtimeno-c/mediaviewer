// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/20 "Craft rules" on the phone: dark, one fill per surface, depth from a lighter
// surface, 12 pt word insets. Text uses Dynamic Type's styles rather than the desktop's fixed sizes.
import MobileKit
import SwiftUI

/// Preferences kept on this phone, shared by the screen that shows a thing and the switch for it.
public enum Preferences {
  public static let filmstrip = "viewerFilmstrip"
  /// "dark" (the default, as the viewer is), "light" or "system".
  public static let appearance = "appearance"
  /// The core reads it too (LinkPreferences): a direct path after the relay on mobile data.
  public static let directOnMobileData = LinkPreferences.directOnMobileData
}

/// The colour scheme the person chose, for `preferredColorScheme`; nil follows the system.
func chosenScheme(_ stored: String) -> ColorScheme? {
  switch stored {
  case "light": return .light
  case "system": return nil
  default: return .dark
  }
}

public enum Theme {
  private static func pair(dark: UInt32, light: UInt32) -> Color {
    Color(UIColor { traits in
      let hex = traits.userInterfaceStyle == .light ? light : dark
      return UIColor(red: CGFloat((hex >> 16) & 255) / 255, green: CGFloat((hex >> 8) & 255) / 255,
                     blue: CGFloat(hex & 255) / 255, alpha: 1)
    })
  }

  // Dark is the app's own look. Light keeps the same structure: depth from a lighter or darker
  // fill, never a shadow, and every text colour at 4.5:1 or better on its ground.
  static let ground = pair(dark: 0x0E0E10, light: 0xF2F2F7)
  static let surface = pair(dark: 0x1A1A1D, light: 0xFFFFFF)
  static let lifted = pair(dark: 0x26262A, light: 0xE3E3E8)
  static let title = pair(dark: 0xECECEE, light: 0x1C1C1E)
  static let body = pair(dark: 0xA2A2A7, light: 0x5F5F64)
  public static let accent = pair(dark: 0x5799FF, light: 0x0A63D6)
  /// The accent as a button's fill: dark enough for white words to pass 4.5:1 in both looks.
  static let accentFill = pair(dark: 0x2E70E6, light: 0x0A63D6)
  static let warm = pair(dark: 0xF5B352, light: 0x9A5300)
  /// Forget, the one action that loses something: 4.5:1 or better on ground in both looks.
  static let danger = pair(dark: 0xFF453A, light: 0xC4161C)
  static let onAccent = Color.white

  static let inset: CGFloat = 12
  static let cardRadius: CGFloat = 10
  static let readableWidth: CGFloat = 480
}

/// The main action. Feedback starts on the touch, and the fill carries it.
struct PrimaryButtonStyle: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .font(.headline)
      .foregroundStyle(Theme.onAccent)
      .multilineTextAlignment(.center)
      .fixedSize(horizontal: false, vertical: true)
      .padding(.vertical, 8)
      .frame(maxWidth: .infinity, minHeight: 50)
      .padding(.horizontal, Theme.inset)
      .background(RoundedRectangle(cornerRadius: 12).fill(Theme.accentFill))
      .opacity(configuration.isPressed ? 0.7 : 1)
      .contentShape(Rectangle())
  }
}

/// A button with a surface at rest, second to `PrimaryButtonStyle`.
struct SecondaryButtonStyle: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .font(.headline)
      .foregroundStyle(Theme.title)
      .multilineTextAlignment(.center)
      .fixedSize(horizontal: false, vertical: true)
      .padding(.vertical, 8)
      .frame(maxWidth: .infinity, minHeight: 50)
      .padding(.horizontal, Theme.inset)
      .background(RoundedRectangle(cornerRadius: 12).fill(configuration.isPressed ? Theme.surface : Theme.lifted))
      .contentShape(Rectangle())
  }
}

/// A quiet action at the end of a line: Cancel, Try again.
struct QuietButtonStyle: ButtonStyle {
  var color: Color = Theme.body
  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .font(.footnote)
      .foregroundStyle(configuration.isPressed ? Theme.title : color)
      .padding(.horizontal, 8)
      .frame(minHeight: 44)
      .contentShape(Rectangle())
  }
}

/// The viewer's round controls, readable over any photo. They keep their size over the picture;
/// at the accessibility text sizes a long press shows them large instead.
struct ChromeIconStyle: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .font(.system(size: 16, weight: .semibold))
      .foregroundStyle(.white)
      .frame(width: 44, height: 44)
      .background(Circle().fill(.black.opacity(configuration.isPressed ? 0.75 : 0.5)))
      .contentShape(Circle())
      .accessibilityShowsLargeContentViewer()
  }
}

/// A photo in a grid: dims the moment a finger lands.
struct TileButtonStyle: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    configuration.label.opacity(configuration.isPressed ? 0.7 : 1)
  }
}

/// Everything the first-run screens share: words at a readable width, centred on an iPad.
struct SetupPage<Content: View, Footer: View>: View {
  @ViewBuilder var content: Content
  /// The page's buttons: held at the bottom, except at the accessibility text sizes, where they
  /// would leave the words a sliver, so they follow the words and scroll with them.
  @ViewBuilder var footer: Footer
  @Environment(\.dynamicTypeSize) private var typeSize

  var body: some View {
    let inline = typeSize.isAccessibilitySize
    let page = ScrollView {
      VStack(alignment: .leading, spacing: 0) {
        content
        if inline { footer.padding(.top, 32) }
      }
      .frame(maxWidth: Theme.readableWidth, alignment: .leading)
      .padding(.horizontal, 24)
      .frame(maxWidth: .infinity)
      .padding(.vertical, 24)
    }
    .scrollBounceBehavior(.basedOnSize)
    .background(Theme.ground)
    if inline {
      page
    } else {
      page.safeAreaInset(edge: .bottom, spacing: 0) {
        footer
          .frame(maxWidth: Theme.readableWidth)
          .padding(.horizontal, 24)
          .padding(.vertical, 16)
          .frame(maxWidth: .infinity)
          .background(Theme.ground)
      }
    }
  }
}

extension SetupPage where Footer == EmptyView {
  init(@ViewBuilder content: () -> Content) {
    self.init(content: content, footer: { EmptyView() })
  }
}

extension View {
  /// The arrival every notice, row and bar uses: a fade and a few points of rise. A cut with
  /// Reduce Motion.
  func arrives(reduceMotion: Bool) -> some View {
    transition(reduceMotion ? .identity : .opacity.combined(with: .offset(y: 6)))
  }
}
