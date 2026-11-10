// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/20 "Craft rules" on the phone: dark, one fill per surface, depth from a lighter
// surface, 12 pt word insets. Text uses Dynamic Type's styles rather than the desktop's fixed sizes.
import SwiftUI

/// Preferences kept on this phone, shared by the screen that shows a thing and the switch for it.
public enum Preferences {
  public static let filmstrip = "viewerFilmstrip"
}

public enum Theme {
  static let ground = Color(red: 0.055, green: 0.055, blue: 0.063)
  static let surface = Color(red: 0.102, green: 0.102, blue: 0.114)
  static let lifted = Color(red: 0.149, green: 0.149, blue: 0.165)
  static let title = Color(red: 0.925, green: 0.925, blue: 0.933)
  static let body = Color(red: 0.635, green: 0.635, blue: 0.655)
  public static let accent = Color(red: 0.34, green: 0.60, blue: 1.0)
  /// The accent as a button's fill: dark enough for white words to pass 4.5:1.
  static let accentFill = Color(red: 0.18, green: 0.44, blue: 0.90)
  static let warm = Color(red: 0.96, green: 0.70, blue: 0.32)
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

/// The viewer's round controls, readable over any photo.
struct ChromeIconStyle: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .font(.system(size: 16, weight: .semibold))
      .foregroundStyle(.white)
      .frame(width: 44, height: 44)
      .background(Circle().fill(.black.opacity(configuration.isPressed ? 0.75 : 0.5)))
      .contentShape(Circle())
  }
}

/// A photo in a grid: dims the moment a finger lands.
struct TileButtonStyle: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    configuration.label.opacity(configuration.isPressed ? 0.7 : 1)
  }
}

/// Everything the first-run screens share: words at a readable width, centred on an iPad.
struct SetupPage<Content: View>: View {
  @ViewBuilder var content: Content

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 0) { content }
        .frame(maxWidth: Theme.readableWidth, alignment: .leading)
        .padding(.horizontal, 24)
        .frame(maxWidth: .infinity)
        .padding(.vertical, 24)
    }
    .scrollBounceBehavior(.basedOnSize)
    .background(Theme.ground)
  }
}

extension View {
  /// The arrival every notice, row and bar uses: a fade and a few points of rise. A cut with
  /// Reduce Motion.
  func arrives(reduceMotion: Bool) -> some View {
    transition(reduceMotion ? .identity : .opacity.combined(with: .offset(y: 6)))
  }
}
