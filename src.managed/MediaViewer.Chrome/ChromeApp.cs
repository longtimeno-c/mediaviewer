// SPDX-License-Identifier: GPL-2.0-or-later
using Microsoft.UI.Xaml;

namespace MediaViewer.Chrome;

/// <summary>
/// Process-wide WinUI Application so generic.xaml styles load. There is no
/// WinUI Window — the native Win32 HWND is the app.
/// </summary>
internal sealed class ChromeApp : Application
{
}
