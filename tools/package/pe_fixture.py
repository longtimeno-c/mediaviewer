# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tiny PE files for the tests (test_driftwire_windows.py, test_driftwire_repo.py): a DOS
stub, the PE and COFF headers, a PE32+ (or PE32) optional header and one section holding
an import table, a delay-load table and their DLL names; optionally a certificate table, as
an Authenticode-signed file has. Nothing runs them; they are read."""
import struct


def make_pe(imports=(), delay=(), machine=0x8664, pe32=False, signed=False):
    imports, delay = list(imports), list(delay)
    opt_size = 224 if pe32 else 240
    section_table = 0x40 + 4 + 20 + opt_size
    raw = 0x400
    va = 0x1000

    # The section: import descriptors, a zero one, delay descriptors, a zero one, the names.
    imp_at = 0
    delay_at = imp_at + 20 * (len(imports) + 1)
    names_at = delay_at + 32 * (len(delay) + 1) if delay else delay_at
    names = b''
    name_rvas = []
    for dll in imports + delay:
        name_rvas.append(va + names_at + len(names))
        names += dll.encode('ascii') + b'\0'
    body = bytearray(names_at + len(names))
    for i, rva in enumerate(name_rvas[:len(imports)]):
        struct.pack_into('<I', body, imp_at + 20 * i + 12, rva)
    for i, rva in enumerate(name_rvas[len(imports):]):
        struct.pack_into('<II', body, delay_at + 32 * i, 1, rva)   # Attributes: RVA-based
    body[names_at:] = names
    while len(body) % 0x200:
        body.append(0)

    out = bytearray(raw)
    out[0:2] = b'MZ'
    struct.pack_into('<I', out, 0x3C, 0x40)
    out[0x40:0x44] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', out, 0x44, machine, 1, 0, 0, 0, opt_size, 0x22)
    opt = 0x58
    if pe32:
        struct.pack_into('<H', out, opt, 0x10B)
        struct.pack_into('<I', out, opt + 28, 0x400000)
        dirs = opt + 96
    else:
        struct.pack_into('<H', out, opt, 0x20B)
        struct.pack_into('<Q', out, opt + 24, 0x140000000)
        dirs = opt + 112
    struct.pack_into('<I', out, dirs - 4, 16)
    if imports:
        struct.pack_into('<II', out, dirs + 8 * 1, va + imp_at, 20 * (len(imports) + 1))
    if delay:
        struct.pack_into('<II', out, dirs + 8 * 13, va + delay_at, 32 * (len(delay) + 1))
    struct.pack_into('<8sIIII', out, section_table, b'.idata', len(body), va, len(body), raw)
    out += body
    if signed:
        # WIN_CERTIFICATE: dwLength, wRevision 2.0, WIN_CERT_TYPE_PKCS_SIGNED_DATA, then the
        # PKCS#7 blob (a stand-in here). Its directory entry holds a file offset, not an RVA.
        cert = struct.pack('<IHH', 16, 0x0200, 0x0002) + b'\x30\x82fake'
        cert += b'\0' * (-len(cert) % 8)
        struct.pack_into('<II', out, dirs + 8 * 4, len(out), len(cert))
        out += cert
    return bytes(out)
