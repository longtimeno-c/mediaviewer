// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The executable of the Final Cut Pro add-on's container bundle
// (MediaViewer Search.app, inside the add-on's folder; docs/design/23). macOS
// discovers a workflow extension only inside an app bundle LaunchServices
// knows, so the add-on carries one around its extension. Nobody opens it:
// MediaViewer registers it when Final Cut Pro is turned on, and it is
// background-only (LSBackgroundOnly). Opened anyway, it does nothing.
int main() { return 0; }
