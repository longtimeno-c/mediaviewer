# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""provision.py against a stand-in App Store Connect: the token is a real
ES256 JWS (checked with openssl), App IDs and capabilities are created only
when missing, a dry run changes nothing, and each profile Apple returns is
checked with the release's own check before it is kept."""
from __future__ import annotations

import base64
import json
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
import macpack  # noqa: E402
import provision  # noqa: E402

TEAM = "ABCDE12345"
APP = provision.APP_BUNDLE_ID
TUNNEL = APP + provision.TUNNEL_SUFFIX


def raw_to_der(raw: bytes) -> bytes:
    def integer(b: bytes) -> bytes:
        b = b.lstrip(b"\x00") or b"\x00"
        if b[0] & 0x80:
            b = b"\x00" + b
        return b"\x02" + bytes([len(b)]) + b
    body = integer(raw[:32]) + integer(raw[32:])
    return b"\x30" + bytes([len(body)]) + body


class TokenTests(unittest.TestCase):
    def test_der_to_raw_pads_and_strips(self):
        r, s = b"\x00" + b"\x01" * 31, b"\x80" + b"\x02" * 31   # a short r; an s needing a sign byte
        self.assertEqual(provision.der_to_raw(raw_to_der(r + s)), r + s)
        with self.assertRaises(ValueError):
            provision.der_to_raw(b"\x31\x00")

    @unittest.skipUnless(shutil.which("openssl"), "needs openssl")
    def test_the_token_is_a_valid_es256_jws(self):
        with tempfile.TemporaryDirectory() as tmp:
            key, pub = Path(tmp) / "AuthKey.p8", Path(tmp) / "pub.pem"
            subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out",
                            str(Path(tmp) / "ec.pem")], check=True, capture_output=True)
            subprocess.run(["openssl", "pkcs8", "-topk8", "-nocrypt", "-in", str(Path(tmp) / "ec.pem"),
                            "-out", str(key)], check=True, capture_output=True)
            subprocess.run(["openssl", "ec", "-in", str(key), "-pubout", "-out", str(pub)],
                           check=True, capture_output=True)
            token = provision.make_token(key, "KEY123", "issuer-uuid", now=1_700_000_000)
            head, body, sig = token.split(".")
            pad = lambda t: t + "=" * (-len(t) % 4)  # noqa: E731
            self.assertEqual(json.loads(base64.urlsafe_b64decode(pad(head))),
                             {"alg": "ES256", "kid": "KEY123", "typ": "JWT"})
            claims = json.loads(base64.urlsafe_b64decode(pad(body)))
            self.assertEqual((claims["iss"], claims["aud"], claims["exp"] - claims["iat"]),
                             ("issuer-uuid", "appstoreconnect-v1", 1200))
            raw = base64.urlsafe_b64decode(pad(sig))
            self.assertEqual(len(raw), 64)
            (Path(tmp) / "sig.der").write_bytes(raw_to_der(raw))
            ok = subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(pub), "-signature",
                                 str(Path(tmp) / "sig.der")], input=f"{head}.{body}".encode(),
                                capture_output=True)
            self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)


class FakeConnect:
    """Enough of App Store Connect for provision.run."""

    def __init__(self, bundles=None, certs=None):
        self.bundles = bundles or {}          # identifier -> (id, set of caps)
        self.certs = certs if certs is not None else [
            {"id": "C1", "attributes": {"name": "Developer ID Application: Example", "serialNumber": "S1",
                                        "expirationDate": "2030-01-01T00:00:00.000+0000"}}]
        self.posts = []

    def bundle(self, ident):
        bid, _ = self.bundles[ident]
        return {"id": bid, "type": "bundleIds", "attributes": {"identifier": ident}}

    def send(self, method, path, body):
        if method == "POST":
            self.posts.append((path, body))
        if method == "GET" and path.startswith("/bundleIds?"):
            ident = path.split("filter[identifier]=")[1].split("&")[0]
            # The real filter matches by prefix: both come back for the app's id.
            return {"data": [self.bundle(i) for i in self.bundles if i.startswith(ident)]}
        if method == "GET" and path.endswith("/bundleIdCapabilities"):
            bid = path.split("/")[2]
            caps = next(c for i, c in self.bundles.values() if i == bid)
            return {"data": [{"attributes": {"capabilityType": c}} for c in sorted(caps)]}
        if method == "POST" and path == "/bundleIds":
            ident = body["data"]["attributes"]["identifier"]
            self.bundles[ident] = (f"B{len(self.bundles) + 1}", set())
            return {"data": self.bundle(ident)}
        if method == "POST" and path == "/bundleIdCapabilities":
            bid = body["data"]["relationships"]["bundleId"]["data"]["id"]
            for ident, (i, caps) in self.bundles.items():
                if i == bid:
                    caps.add(body["data"]["attributes"]["capabilityType"])
            return {"data": {}}
        if method == "GET" and path.startswith("/certificates?"):
            # Both Developer ID types are asked for: a G2 certificate is the usual one now.
            types = path.split("filter[certificateType]=")[1].split("&")[0].split(",")
            assert set(types) == {"DEVELOPER_ID_APPLICATION", "DEVELOPER_ID_APPLICATION_G2"}, types
            return {"data": self.certs}
        if method == "POST" and path == "/profiles":
            bid = body["data"]["relationships"]["bundleId"]["data"]["id"]
            ident, caps = next((i, c) for i, (b, c) in self.bundles.items() if b == bid)
            ent = {macpack.APP_ID_KEY: f"{TEAM}.{ident}"}
            if "NETWORK_EXTENSIONS" in caps:
                ent[macpack.NETWORK_EXTENSION] = [macpack.PACKET_TUNNEL, "app-proxy-provider-systemextension"]
            if "SYSTEM_EXTENSION_INSTALL" in caps:
                ent[macpack.SYSEXT_INSTALL] = True
            profile = {"Name": body["data"]["attributes"]["name"], "TeamIdentifier": [TEAM],
                       "ProvisionsAllDevices": True, "ExpirationDate": datetime(2030, 1, 1), "Entitlements": ent}
            return {"data": {"attributes": {"profileContent": base64.b64encode(plistlib.dumps(profile)).decode()}}}
        raise AssertionError(f"unexpected {method} {path}")


def args(out, **kw):
    a = {"bundle_id": APP, "certificate": None, "team_id": TEAM, "out": str(out), "dry_run": False}
    a.update(kw)
    return SimpleNamespace(**a)


def read_plist(path):
    return plistlib.loads(Path(path).read_bytes())


class ProvisionTests(unittest.TestCase):
    def test_creates_what_is_missing_and_keeps_checked_profiles(self):
        fake = FakeConnect(bundles={APP: ("B0", {"PHOTOS"})})   # the app's id exists, the tunnel's does not
        secrets = {}
        with tempfile.TemporaryDirectory() as tmp:
            saved = provision.run(args(tmp), provision.Api("t", fake.send), say=lambda s: None,
                                  read_profile=read_plist, set_secret=secrets.__setitem__)
            self.assertEqual([p.name for p in saved], ["MediaViewer_DeveloperID.provisionprofile",
                                                       "DriftwireTunnel_DeveloperID.provisionprofile"])
            self.assertEqual(read_plist(saved[1])["Entitlements"][macpack.APP_ID_KEY], f"{TEAM}.{TUNNEL}")
        created = [b["data"]["attributes"]["identifier"] for p, b in fake.posts if p == "/bundleIds"]
        self.assertEqual(created, [TUNNEL])
        self.assertEqual(fake.bundles[APP][1], {"PHOTOS", "NETWORK_EXTENSIONS", "SYSTEM_EXTENSION_INSTALL"})
        self.assertEqual(fake.bundles[TUNNEL][1], {"NETWORK_EXTENSIONS"})
        profiles = [b["data"] for p, b in fake.posts if p == "/profiles"]
        self.assertEqual({d["attributes"]["profileType"] for d in profiles}, {"MAC_APP_DIRECT"})
        self.assertEqual(sorted(secrets), ["MV_APP_PROVISION_PROFILE_B64", "MV_TUNNEL_PROVISION_PROFILE_B64"])

    def test_a_second_run_adds_nothing_but_new_profiles(self):
        fake = FakeConnect(bundles={APP: ("B0", {"NETWORK_EXTENSIONS", "SYSTEM_EXTENSION_INSTALL"}),
                                    TUNNEL: ("B1", {"NETWORK_EXTENSIONS"})})
        with tempfile.TemporaryDirectory() as tmp:
            provision.run(args(tmp), provision.Api("t", fake.send), say=lambda s: None, read_profile=read_plist)
        self.assertEqual({p for p, _ in fake.posts}, {"/profiles"})

    def test_a_dry_run_changes_nothing(self):
        fake = FakeConnect(bundles={APP: ("B0", set())})
        lines = []
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(provision.run(args(tmp, dry_run=True), provision.Api("t", fake.send),
                                           say=lines.append, read_profile=read_plist), [])
            self.assertEqual(list(Path(tmp).iterdir()), [])
        self.assertEqual(fake.posts, [])
        self.assertTrue(any("would create" in line for line in lines))

    def test_a_profile_that_fails_the_check_is_refused(self):
        fake = FakeConnect(bundles={APP: ("B0", set()), TUNNEL: ("B1", set())})
        real_send = fake.send

        def no_caps(method, path, body):   # Apple ignored the capability: the profile lacks it
            if path == "/bundleIdCapabilities":
                fake.posts.append((path, body))
                return {"data": {}}
            return real_send(method, path, body)

        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(SystemExit) as e:
            provision.run(args(tmp), provision.Api("t", no_caps), say=lambda s: None, read_profile=read_plist)
        self.assertIn("fails the release's check", str(e.exception))

    def test_several_certificates_must_be_named(self):
        two = [{"id": f"C{i}", "attributes": {"name": "Developer ID Application: Example", "serialNumber": f"S{i}",
                                              "expirationDate": "2030-01-01T00:00:00.000+00:00"}} for i in (1, 2)]
        expired = {"id": "C3", "attributes": {"serialNumber": "S3", "expirationDate": "2020-01-01T00:00:00Z"}}
        fake = FakeConnect(bundles={APP: ("B0", set()), TUNNEL: ("B1", set())}, certs=two + [expired])
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(SystemExit) as e:
                provision.run(args(tmp), provision.Api("t", fake.send), say=lambda s: None, read_profile=read_plist)
            self.assertIn("--certificate S2", str(e.exception))
            self.assertNotIn("S3", str(e.exception))
            provision.run(args(tmp, certificate="S2"), provision.Api("t", fake.send), say=lambda s: None,
                          read_profile=read_plist)
        certs = {b["data"]["relationships"]["certificates"]["data"][0]["id"] for p, b in fake.posts if p == "/profiles"}
        self.assertEqual(certs, {"C2"})


if __name__ == "__main__":
    unittest.main()
