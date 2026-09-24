// SPDX-License-Identifier: GPL-2.0-or-later
// `?` cheat sheet (plan/16-commands.md "`?`"): a chrome overlay over the
// canvas listing the current bindings. The Mac key map lives in main_mac.mm's
// keyDown: (the shared command table is not built on Darwin yet), so this list
// is maintained by hand next to it -- a binding added there belongs here too.
import SwiftUI

struct HelpView: View {
  private struct Row: Identifiable {
    let keys: String
    let action: String
    var id: String { keys }
  }
  private struct Section: Identifiable {
    let title: String
    let rows: [Row]
    var id: String { title }
  }

  private let sections: [Section] = [
    Section(title: "Browse", rows: [
      Row(keys: "← →   A D", action: "Previous / next"),
      Row(keys: "Home  End", action: "First / last"),
      Row(keys: "PgUp  PgDn", action: "Skip ~10"),
      Row(keys: "Space", action: "Next (pause in a slideshow)"),
      Row(keys: "⌘O", action: "Open a folder or file"),
    ]),
    Section(title: "View", rows: [
      Row(keys: "0", action: "Fit to window"),
      Row(keys: "1", action: "Actual size"),
      Row(keys: "Wheel  Drag", action: "Zoom toward cursor / pan"),
      Row(keys: "T", action: "Filmstrip"),
      Row(keys: "G", action: "Gallery"),
      Row(keys: "F  F11", action: "Full screen"),
      Row(keys: "F5", action: "Slideshow"),
      Row(keys: "F3", action: "Frame-time overlay"),
    ]),
    Section(title: "Metadata", rows: [
      Row(keys: "I", action: "Metadata pane (summary, all tags, streams)"),
      Row(keys: "O", action: "Info overlay (exposure, camera, date)"),
      Row(keys: "⇧O", action: "AF points (from maker notes)"),
      Row(keys: "⇧I", action: "Eyedropper (pixel under the cursor)"),
      Row(keys: "⌘⇧E", action: "Folder tree"),
      Row(keys: "View ▸ Sort By", action: "Name, date, size, type, date taken"),
    ]),
    Section(title: "Gallery", rows: [
      Row(keys: "↑ ↓   W S", action: "Move by row"),
      Row(keys: "← →   A D", action: "Move by item"),
      Row(keys: "Enter  Click", action: "Open selection"),
      Row(keys: "+  −", action: "Larger / smaller thumbnails"),
      Row(keys: "Esc", action: "Close"),
    ]),
    Section(title: "Marks & files", rows: [
      Row(keys: "Insert  ⇧Space", action: "Mark / unmark"),
      Row(keys: "⌃A  ⌃D", action: "Mark all / unmark all"),
      Row(keys: "⌘C", action: "Copy marked (or current / selected) files; the colour if the eyedropper is on"),
      Row(keys: "F7  F8", action: "Copy / move marked (⇧ picks folder)"),
      Row(keys: "⌫  ⌘⌫", action: "Move marked or current to Trash"),
    ]),
    Section(title: "Video", rows: [
      Row(keys: "Space  K", action: "Play / pause"),
      Row(keys: ", .", action: "Frame step back / forward"),
      Row(keys: "Q  E", action: "Skip -2 s / +2 s"),
      Row(keys: "J  L", action: "Skip -10 s / +10 s"),
      Row(keys: "⇧Q  ⇧E", action: "Slower / faster (0.25x - 4x)"),
      Row(keys: "⇧M", action: "Mute"),
    ]),
    Section(title: "Slideshow", rows: [
      Row(keys: "Space", action: "Pause / resume"),
      Row(keys: "+  −", action: "Interval"),
      Row(keys: "Esc", action: "Leave"),
    ]),
  ]

  var body: some View {
    VStack(spacing: 16) {
      Text("Keyboard shortcuts").font(.title2.bold())
      ScrollView {
        LazyVGrid(
          columns: [GridItem(.adaptive(minimum: 300), spacing: 24, alignment: .top)],
          alignment: .leading, spacing: 20
        ) {
          ForEach(sections) { section in
            VStack(alignment: .leading, spacing: 6) {
              Text(section.title).font(.headline)
              ForEach(section.rows) { row in
                HStack(alignment: .firstTextBaseline) {
                  Text(row.keys)
                    .font(.system(.body, design: .monospaced))
                    .frame(width: 130, alignment: .leading)
                  Text(row.action).foregroundStyle(.secondary)
                }
              }
            }
            .frame(maxWidth: .infinity, alignment: .topLeading)
          }
        }
        .padding(.horizontal, 32)
      }
      Text("? or Esc to close").font(.footnote).foregroundStyle(.secondary)
    }
    .padding(.top, 56)
    .padding(.bottom, 16)
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(.regularMaterial)
  }
}
