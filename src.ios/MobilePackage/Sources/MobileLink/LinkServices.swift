// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import MobileKit

/// What the app gets from the link: the rest of `Services` comes from their own owners.
public enum LinkServices {
  public struct Bundle: Sendable {
    public let connection: LinkConnection
    public let images: LinkImages
    public let clips: LinkClips
    public let library: InterimLibrary
    public let backup: LinkBackup
  }

  public static func make() -> Bundle {
    let connection = LinkConnection()
    connection.becomeShared()
    let images = LinkImages(connection: connection)
    return Bundle(connection: connection, images: images, clips: LinkClips(connection: connection, images: images),
                  library: InterimLibrary(connection: connection), backup: LinkBackup(connection: connection))
  }
}
