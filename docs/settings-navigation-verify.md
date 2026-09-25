# Settings and folder navigation verification

Run these checks in both the Windows and macOS native applications. Building the
chrome libraries does not verify rendering, focus across native islands, or GPU
frame pacing. CI compiles WinUI and SwiftUI independently of the full native build.

## Settings

- Open Settings with Ctrl+, (Cmd+, on macOS), at a normal window size and at the
  smallest supported size. General shows labelled groups and right-aligned
  switches. Descriptions wrap; the page scrolls without moving the header or Done.
- Toggle both filmstrip preferences, wrap and keep pan/zoom; change sort, direction
  and background. Close/reopen Settings and restart the app: values persist and
  the viewer/menu agree. No setting changes merely by switching tabs.
- Tab/Shift+Tab through General and Keyboard shortcuts. Controls have readable
  accessibility names. Space activates a focused switch. Done closes Settings.
- Search shortcuts by command and by key, including Mac glyphs. Remap a command,
  cancel another capture, test a conflict swap, and reset to defaults. Search for
  the new key after a remap: it finds the updated command on Windows too.
- Begin capture, then switch to General: capture stops and typing cannot remap a
  hidden row. Esc cancels capture; in the search field it clears a non-empty
  search before closing Settings. Check close/reopen and repeated tab changes.
- Scroll to Add-ons; existing install/remove controls remain reachable. On
  Windows, update and diagnostics switches retain their existing consent rules.

## Folders

Use a tree at least six levels deep, with empty folders, folders containing only
folders, mixed photo/video folders, and long/repeated/Unicode folder names.

- Open its root and click through three levels. **Up** opens the enclosing folder
  and reselects the folder just left. **Root** returns directly to the first
  breadcrumb. Test in the gallery and while a photo/video is open.
- At the browsing root, Root is disabled. Up remains available if there is an
  enclosing filesystem folder. At a volume root, both are disabled.
- At six levels, open the ellipsis menu and choose each hidden ancestor. Its
  destination must match its full-path tooltip; no horizontal expansion needed.
- At a narrow width, scroll the ancestor trail: Up, Root and the current folder
  remain outside it. Hover truncated names to read their complete paths.
- Open a different folder externally; Root targets the new trail, not the old
  one. Go above the initial root; Root then means the highest folder reached.
- Recheck Ctrl/Cmd+Up, sibling shortcuts, arrows, Enter and Esc. In a mixed folder,
  Up/Down crosses between folders and photos in the same column. Reopen the tree
  sidebar and check it follows the new location.

The inherited PR 26 verify line also requires `test_dir_tree`, `test_browse_path`
and the PR 1 present-loop checks. Run the Windows and macOS native suites and
GPU soaks on suitable hosts before merge. A compile-only check is not a pass for
these interactive or hardware checks.
