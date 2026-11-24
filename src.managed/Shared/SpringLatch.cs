// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
namespace MediaViewer.Shared;

/// <summary>
/// docs/design/16 "Drag and drop into folders": a folder rested on with a drag opens once.
/// The island's timer is made once per target with this as its one Tick handler; DragEnter
/// arms it, DragLeave and Drop disarm it, and a tick opens the folder only when armed -- so
/// however many times a drag passes over, one rest opens it once.
/// </summary>
public sealed class SpringLatch
{
    private readonly Action _open;
    private bool _armed;

    public SpringLatch(Action open) => _open = open;

    public bool Armed => _armed;
    public void Arm() => _armed = true;
    public void Disarm() => _armed = false;

    public void Fire()
    {
        if (!_armed) return;
        _armed = false;
        _open();
    }
}
