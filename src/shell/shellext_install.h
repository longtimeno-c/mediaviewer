// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 15 (plan/09 "Windows integration", plan/12 2026-09-25): installs and
// registers the Explorer thumbnail handler (MediaViewerThumbs.dll) for the
// running version. Windows host only.
//
//   * The handler runs from <root>\shellext\<version>\, a copy of the files
//     MediaViewerThumbs.files lists, made from current\ on the first start of
//     a version. An isolated surrogate may hold the DLL for minutes; an update
//     swaps current\ and never has to replace a DLL in use.
//   * Per-user COM (HKCU\Software\Classes): the CLSID with its
//     InprocServer32 and a DllSurrogate AppID, and the thumbnail ShellEx on
//     the MediaViewer.Image ProgId only, so it runs for the types the user
//     made MediaViewer the default for and never replaces another handler.
//   * Older version folders are removed when nothing holds them any more.
//
// The wizard creates the same keys empty with uninsdeletekey, and deletes
// <root>\shellext, so uninstall removes all of it (mediaviewer.iss).
#pragma once

#include <string>

namespace mv::shell {

// {6A3F1B52-8C0E-4D7A-9B21-5E4C7D2F9A13}, as in shellext/thumb_provider_win.cpp.
inline constexpr wchar_t kThumbHandlerClsid[] = L"{6A3F1B52-8C0E-4D7A-9B21-5E4C7D2F9A13}";
// The handler's own surrogate AppID.
inline constexpr wchar_t kThumbHandlerAppId[] = L"{3E91A7C2-5B4D-4F18-8C6E-9D2A1B7F4E05}";

// Worker thread: file copies and registry writes (rule 1). Idempotent: a
// start of an already-installed version rewrites nothing and tells Explorer
// nothing. Does nothing for a dev build (`root` empty). Never throws.
void install_thumbnail_handler(const std::wstring& root, const std::string& version) noexcept;

}  // namespace mv::shell
