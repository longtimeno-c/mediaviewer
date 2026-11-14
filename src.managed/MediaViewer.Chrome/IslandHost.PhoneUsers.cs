// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
using MediaViewer.Shared;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace MediaViewer.Chrome;

/// <summary>
/// Settings → Phone access → This computer → Users (docs/plans/self-host-users-smb.md §8):
/// who may open this PC's shared folders from other computers at home, and
/// with what role. The Mac twin is PhoneUsersView.swift; the words are
/// Shared/PhoneWords.cs. Admin unlock first (the server passphrase, the card's
/// own unlock); each change goes to the host's service over the owner session,
/// then to Windows' file sharing through one UAC approval the driftwire
/// program asks for. A typed password is handed on once and cleared: nothing on
/// this PC keeps it, and it never reaches a log (rule 6).
/// </summary>
public static partial class IslandHost
{
    private static FakeInput? _usersUnlock, _usersName, _usersPassword, _usersPasswordAgain;
    private static ComboBox? _usersRole;
    private static bool _usersAdding;
    private static bool _usersAsked;  // users_refresh sent for this showing of the card

    private static void MakePhoneUserInputs()
    {
        if (_usersUnlock is not null) return;
        _usersUnlock = PhoneSecret("Server passphrase", "Server passphrase to unlock as admin");
        _usersUnlock.Submitted += UnlockUsers;
        _usersName = PhoneInput("anna", "New user's name");
        _usersPassword = PhoneSecret("Password", "New user's password");
        _usersPasswordAgain = PhoneSecret("Again", "New user's password again");
        _usersRole = new ComboBox { FontFamily = UiFont, FontSize = UiFontSize, Foreground = Brush(Title), Width = 240 };
        foreach ((string _, string label) in PhoneWords.Roles) _usersRole.Items.Add(label);
        _usersRole.SelectedIndex = 0;
        AutomationProperties.SetName(_usersRole, "New user's role");
    }

    // The section, once this PC is the claimed server (IslandHost.Phone.cs draws it under the host row).
    private static void AddPhoneUsers(StackPanel rows, PhoneLinkState s, bool busy)
    {
        MakePhoneUserInputs();
        foreach (FrameworkElement? f in new FrameworkElement?[] { _usersUnlock, _usersName, _usersPassword, _usersPasswordAgain, _usersRole })
            if (f?.Parent is Panel p) p.Children.Remove(f);
        if (!_usersAsked)
        {
            _usersAsked = true;
            PhoneRequest(("do", "users_refresh"));
        }
        bool locked = s.NasPassphrase == "set" && s.UnlockedSeconds == 0;
        FrameworkElement add = new Border();
        if (!locked && !_usersAdding)
        {
            Button b = PhoneButton("Add user…", () =>
            {
                _usersAdding = true;
                if (_phoneShown is { } shown) ShowPhone(shown);
            }, "Add a user");
            b.IsEnabled = !busy;
            add = b;
        }
        rows.Children.Add(SettingsRow("Users",
            "People who can open your shared folders from other computers at home, with their own name and password. For your home network only: MediaViewer never opens it on your router.",
            add, keepName: true));
        if (locked)
        {
            var unlock = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
            unlock.Children.Add(_usersUnlock!);
            Button go = PhoneButton("Unlock", UnlockUsers, "Unlock as admin");
            go.IsEnabled = !busy;
            unlock.Children.Add(go);
            rows.Children.Add(SettingsRow("Unlock as admin",
                "Enter the server passphrase to add or change users. This PC keeps the key for 10 minutes and never the passphrase.",
                unlock, keepName: true));
        }
        else
        {
            foreach (PhoneUser u in s.Users ?? Array.Empty<PhoneUser>()) AddPhoneUserRow(rows, u, busy);
            if (_usersAdding) AddPhoneUserFields(rows, busy);
            if ((s.Users ?? Array.Empty<PhoneUser>()).Any(u => !u.Applied))
            {
                Button finish = PhoneButton("Finish…", () => PhoneRequest(("do", "users_apply")), "Finish with your PC's administrator");
                finish.IsEnabled = !busy;
                rows.Children.Add(SettingsRow("Waiting for your PC's administrator",
                    "Windows asks for an administrator to make these accounts and share the folders.", finish, keepName: true));
            }
        }
        foreach (string a in s.ShareAddresses)
        {
            string address = a;
            rows.Children.Add(SettingsRow(address, "", PhoneButton("Copy", () => CopyPhoneText(address), $"Copy {address}"), keepName: true));
        }
    }

    private static void AddPhoneUserRow(StackPanel rows, PhoneUser u, bool busy)
    {
        var controls = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8) };
        var role = new ComboBox { FontFamily = UiFont, FontSize = UiFontSize, Foreground = Brush(Title), Width = 170 };
        int at = 0;
        for (int i = 0; i < PhoneWords.Roles.Length; ++i)
        {
            role.Items.Add(PhoneWords.Roles[i].Label);
            if (PhoneWords.Roles[i].Value == u.Role) at = i;
        }
        role.SelectedIndex = at;
        role.IsEnabled = !busy;
        AutomationProperties.SetName(role, $"Role for {u.Name}");
        role.SelectionChanged += (_, _) =>
        {
            int i = role.SelectedIndex;
            if (i < 0 || PhoneWords.Roles[i].Value == u.Role) return;
            _ = PhoneRequestJson(PhoneWords.UserSetRequest(u.Name, PhoneWords.Roles[i].Value, u.Folders, ""));
        };
        controls.Children.Add(role);
        Button remove = PhoneButton("Remove", () => PhoneRequest(("do", "user_remove"), ("name", u.Name)), $"Remove {u.Name}");
        remove.IsEnabled = !busy;
        controls.Children.Add(remove);
        rows.Children.Add(SettingsRow(u.Name, PhoneWords.UserDetail(u), controls, keepName: true));
    }

    private static void AddPhoneUserFields(StackPanel rows, bool busy)
    {
        rows.Children.Add(SettingsRow("Name", "Lowercase letters and digits, starting with a letter. Their sign-in from other computers.", _usersName!, keepName: true));
        rows.Children.Add(SettingsRow("Role", "", _usersRole!, keepName: true));
        rows.Children.Add(SettingsRow("Password", "At least 8 characters. Nothing on this PC keeps it.", _usersPassword!, keepName: true));
        rows.Children.Add(SettingsRow("Again", "", _usersPasswordAgain!, keepName: true));
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = Space(8), HorizontalAlignment = HorizontalAlignment.Right };
        buttons.Children.Add(PhoneButton("Cancel", () =>
        {
            ForgetPhoneUserInputs();
            if (_phoneShown is { } shown) ShowPhone(shown);
        }, "Cancel adding a user"));
        Button go = PhoneButton("Add", AddPhoneUser, "Add the user; Windows asks for its administrator");
        go.IsEnabled = !busy;
        buttons.Children.Add(go);
        rows.Children.Add(buttons);
    }

    private static void AddPhoneUser()
    {
        string name = _usersName!.Text.Trim(), pass = _usersPassword!.Text, again = _usersPasswordAgain!.Text;
        int i = _usersRole!.SelectedIndex;
        if (!PhoneWords.ValidUserName(name) || System.Text.Encoding.UTF8.GetByteCount(pass) < 8 || pass != again || i < 0) return;
        string request = PhoneWords.UserSetRequest(name, PhoneWords.Roles[i].Value, null, pass);
        ForgetPhoneUserInputs();
        _ = PhoneRequestJson(request);
    }

    private static void UnlockUsers()
    {
        string pass = _usersUnlock!.Text;
        if (pass.Length == 0) return;
        _usersUnlock.SetText("");
        PhoneRequest(("do", "unlock"), ("passphrase", pass));
    }

    // Typed and not sent: gone when its rows are not drawn, or the card closes.
    private static void ForgetPhoneUserInputs()
    {
        _usersAdding = false;
        _usersName?.SetText("");
        _usersPassword?.SetText("");
        _usersPasswordAgain?.SetText("");
        _usersUnlock?.SetText("");
        if (_usersRole is not null) _usersRole.SelectedIndex = 0;
    }

    private static void ForgetPhoneUserSecretsNotShown()
    {
        if (_usersPassword is not null && !IsInTree(_usersPassword)) ForgetPhoneUserInputs();
        if (_usersUnlock is not null && !IsInTree(_usersUnlock)) _usersUnlock.SetText("");
    }

    private static void CopyPhoneText(string text)
    {
        try
        {
            var package = new Windows.ApplicationModel.DataTransfer.DataPackage();
            package.SetText(text);
            Windows.ApplicationModel.DataTransfer.Clipboard.SetContent(package);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);  // another process holds the clipboard: nothing copied
        }
    }
}
