#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Set up the Driftwire VPN's App IDs and Developer ID provisioning profiles
through the App Store Connect API, so nobody clicks through developer.apple.com.

  1. App IDs: the app's (io.github.longtimeno-c.mediaviewer) and the extension's
     (<app>.DriftwireTunnel), created if missing.
  2. Capabilities: Network Extensions on both, System Extension on the app.
  3. Profiles: a new Developer ID profile for each, against a Developer ID
     Application certificate, saved to --out and checked with macpack's own
     check (profile_problem) before anything else uses them.
  4. With --set-secrets: MV_APP_PROVISION_PROFILE_B64 and
     MV_TUNNEL_PROVISION_PROFILE_B64 on the repository (gh secret set).

It needs an App Store Connect API key with the Admin role (App Store Connect >
Users and Access > Integrations > App Store Connect API > Team Keys): the .p8
file, its Key ID and the Issuer ID. The key never leaves this machine: the
script signs a 20-minute token with it (ES256, via openssl) and sends only the
token. Run it yourself; it changes your Apple developer account.

  python3 tools/mac/provision.py --key ~/AuthKey_ABC123.p8 --key-id ABC123 \\
      --issuer-id 69a6de7e-... --dry-run       # what it would do
  python3 tools/mac/provision.py --key ... --key-id ... --issuer-id ... --set-secrets

RELEASING.md, "the Driftwire VPN's provisioning profiles".
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
import macpack  # noqa: E402  (profile_problem: the release's own check)

API = "https://api.appstoreconnect.apple.com/v1"
APP_BUNDLE_ID = "io.github.longtimeno-c.mediaviewer"
TUNNEL_SUFFIX = ".DriftwireTunnel"
APP_CAPS = ("NETWORK_EXTENSIONS", "SYSTEM_EXTENSION_INSTALL")
TUNNEL_CAPS = ("NETWORK_EXTENSIONS",)
DEVELOPER_ID_PROFILE = "MAC_APP_DIRECT"   # the API's name for a Developer ID profile
# Certificates made since 2021 are G2 (the newer intermediate); older ones keep the plain type.
DEVELOPER_ID_CERTS = ("DEVELOPER_ID_APPLICATION_G2", "DEVELOPER_ID_APPLICATION")
REPO = "longtimeno-c/mediaviewer"


# ---------------------------------------------------------------------------
# The token: ES256 over header.payload, signed by openssl with the .p8 key.
# ---------------------------------------------------------------------------

def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def der_to_raw(der: bytes, size: int = 32) -> bytes:
    """openssl's ECDSA signature (DER SEQUENCE of two INTEGERs) as JWS wants
    it: r and s, each `size` bytes, big-endian. Pure; tested."""
    def read_len(i: int) -> tuple[int, int]:
        n = der[i]
        if n < 0x80:
            return n, i + 1
        count = n & 0x7F
        return int.from_bytes(der[i + 1:i + 1 + count], "big"), i + 1 + count

    if not der or der[0] != 0x30:
        raise ValueError("not a DER signature")
    _, i = read_len(1)
    out = b""
    for _ in range(2):
        if der[i] != 0x02:
            raise ValueError("not a DER signature")
        n, i = read_len(i + 1)
        value = der[i:i + n].lstrip(b"\x00")
        if len(value) > size:
            raise ValueError("signature integer too long")
        out += value.rjust(size, b"\x00")
        i += n
    return out


def make_token(key_path: Path, key_id: str, issuer_id: str, now: int | None = None,
               sign: Callable[[Path, bytes], bytes] | None = None) -> str:
    now = int(time.time()) if now is None else now
    header = {"alg": "ES256", "kid": key_id, "typ": "JWT"}
    payload = {"iss": issuer_id, "iat": now, "exp": now + 20 * 60, "aud": "appstoreconnect-v1"}
    signing_input = (b64url(json.dumps(header, separators=(",", ":")).encode()) + "." +
                     b64url(json.dumps(payload, separators=(",", ":")).encode()))
    der = (sign or openssl_sign)(key_path, signing_input.encode())
    return signing_input + "." + b64url(der_to_raw(der))


def openssl_sign(key_path: Path, data: bytes) -> bytes:
    proc = subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(key_path)],
                          input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if proc.returncode != 0 or not proc.stdout:
        raise SystemExit(f"provision: openssl could not sign with {key_path.name} "
                         "(is it the .p8 file App Store Connect gave you?)")
    return proc.stdout


# ---------------------------------------------------------------------------
# The API
# ---------------------------------------------------------------------------

class Api:
    """GET / POST against App Store Connect. `send` is replaceable for tests."""

    def __init__(self, token: str, send: Callable[[str, str, dict | None], dict] | None = None):
        self.token = token
        self.send = send or self._send

    def _send(self, method: str, path: str, body: dict | None) -> dict:
        req = urllib.request.Request(API + path, method=method,
                                     data=json.dumps(body).encode() if body is not None else None)
        req.add_header("Authorization", f"Bearer {self.token}")
        req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                raw = resp.read()
        except urllib.error.HTTPError as e:
            detail = e.read().decode(errors="replace")
            try:
                errors = json.loads(detail).get("errors", [])
                detail = "; ".join(f"{x.get('title')}: {x.get('detail')}" for x in errors) or detail
            except ValueError:
                pass
            raise SystemExit(f"provision: {method} {path.split('?')[0]} failed ({e.code}): {detail}") from None
        return json.loads(raw) if raw else {}

    def get(self, path: str) -> dict:
        return self.send("GET", path, None)

    def post(self, path: str, body: dict) -> dict:
        return self.send("POST", path, body)


def find_bundle_id(api: Api, identifier: str) -> dict | None:
    # The filter matches by prefix too: keep the exact one.
    found = api.get(f"/bundleIds?filter[identifier]={identifier}&limit=200").get("data", [])
    exact = [b for b in found if b["attributes"]["identifier"] == identifier]
    return exact[0] if exact else None


def ensure_bundle_id(api: Api, identifier: str, name: str, dry: bool, say: Callable[[str], None]) -> dict:
    found = find_bundle_id(api, identifier)
    if found:
        say(f"App ID {identifier}: exists")
        return found
    say(f"App ID {identifier}: {'would create' if dry else 'creating'}")
    if dry:
        return {"id": None, "attributes": {"identifier": identifier}}
    return api.post("/bundleIds", {"data": {"type": "bundleIds", "attributes": {
        "identifier": identifier, "name": name, "platform": "MAC_OS"}}})["data"]


def ensure_capabilities(api: Api, bundle: dict, caps: tuple[str, ...], dry: bool,
                        say: Callable[[str], None]) -> bool:
    """True when a capability was added (the App ID's old profiles are then stale)."""
    ident = bundle["attributes"]["identifier"]
    have = set()
    if bundle["id"]:
        have = {c["attributes"]["capabilityType"]
                for c in api.get(f"/bundleIds/{bundle['id']}/bundleIdCapabilities").get("data", [])}
    added = False
    for cap in caps:
        if cap in have:
            say(f"  {ident}: {cap} on")
            continue
        say(f"  {ident}: {cap} {'would turn on' if dry else 'turning on'}")
        added = True
        if not dry:
            api.post("/bundleIdCapabilities", {"data": {
                "type": "bundleIdCapabilities", "attributes": {"capabilityType": cap},
                "relationships": {"bundleId": {"data": {"type": "bundleIds", "id": bundle["id"]}}}}})
    return added


def pick_certificate(api: Api, wanted: str | None, now: datetime) -> dict:
    """The Developer ID Application certificate to put in the profiles: the
    one named by --certificate (its serial number or id), or the only valid
    one. Several valid and none named: stop and list them."""
    certs = api.get(f"/certificates?filter[certificateType]={','.join(DEVELOPER_ID_CERTS)}&limit=200").get("data", [])

    def valid(c: dict) -> bool:
        exp = parse_date(c["attributes"].get("expirationDate"))
        return exp is None or exp > now

    certs = [c for c in certs if valid(c)]
    if wanted:
        named = [c for c in certs if wanted in (c["id"], c["attributes"].get("serialNumber"))]
        if not named:
            raise SystemExit(f"provision: no valid Developer ID Application certificate {wanted!r}")
        return named[0]
    if len(certs) == 1:
        return certs[0]
    if not certs:
        raise SystemExit("provision: the team has no valid Developer ID Application certificate")
    listing = "\n".join(f"  --certificate {c['attributes'].get('serialNumber')}  "
                        f"{c['attributes'].get('name')} (until {c['attributes'].get('expirationDate')})"
                        for c in certs)
    raise SystemExit("provision: several Developer ID Application certificates; name the one in "
                     "MV_MAC_CERT_P12_BASE64:\n" + listing)


def parse_date(text: str | None) -> datetime | None:
    """Apple's ISO 8601 ("2027-03-01T12:00:00.000+00:00", "...+0000", "...Z"), as UTC."""
    if not text:
        return None
    t = text.replace("Z", "+0000")
    for fmt in ("%Y-%m-%dT%H:%M:%S.%f%z", "%Y-%m-%dT%H:%M:%S%z"):
        try:
            return datetime.strptime(t, fmt).astimezone(timezone.utc)
        except ValueError:
            continue
    return None


def create_profile(api: Api, bundle: dict, cert: dict, name: str) -> bytes:
    data = api.post("/profiles", {"data": {
        "type": "profiles", "attributes": {"name": name, "profileType": DEVELOPER_ID_PROFILE},
        "relationships": {"bundleId": {"data": {"type": "bundleIds", "id": bundle["id"]}},
                          "certificates": {"data": [{"type": "certificates", "id": cert["id"]}]}}}})["data"]
    return base64.b64decode(data["attributes"]["profileContent"])


def team_of(cert: dict, given: str | None) -> str | None:
    """--team-id / $APPLE_TEAM_ID, else the "(TEAMID)" a certificate name may
    end with; None: the profile's own team is taken (its App ID still checked)."""
    if given:
        return given
    name = cert["attributes"].get("name") or ""
    if name.endswith(")") and "(" in name:
        return name[name.rindex("(") + 1:-1]
    return None


def run(args: argparse.Namespace, api: Api, say: Callable[[str], None] = print,
        read_profile: Callable[[Path], dict] = macpack.read_profile,
        set_secret: Callable[[str, bytes], None] | None = None) -> list[Path]:
    dry = args.dry_run
    now = datetime.now(timezone.utc)
    tunnel_id = args.bundle_id + TUNNEL_SUFFIX
    app = ensure_bundle_id(api, args.bundle_id, "MediaViewer", dry, say)
    tunnel = ensure_bundle_id(api, tunnel_id, "MediaViewer Driftwire Tunnel", dry, say)
    ensure_capabilities(api, app, APP_CAPS, dry, say)
    ensure_capabilities(api, tunnel, TUNNEL_CAPS, dry, say)
    cert = pick_certificate(api, args.certificate, now)
    team = team_of(cert, args.team_id)
    say(f"Certificate: {cert['attributes'].get('name')} (serial {cert['attributes'].get('serialNumber')})")
    if dry:
        say("Profiles: would create a Developer ID profile for each App ID (dry run: nothing changed)")
        return []
    out = Path(args.out).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    stamp = now.strftime("%Y-%m-%d %H%M")
    saved = []
    for bundle, needs, file, secret in (
            (app, macpack.APP_PROFILE_NEEDS, "MediaViewer_DeveloperID.provisionprofile",
             "MV_APP_PROVISION_PROFILE_B64"),
            (tunnel, macpack.TUNNEL_PROFILE_NEEDS, "DriftwireTunnel_DeveloperID.provisionprofile",
             "MV_TUNNEL_PROVISION_PROFILE_B64")):
        ident = bundle["attributes"]["identifier"]
        content = create_profile(api, bundle, cert, f"{ident} Developer ID {stamp}")
        path = out / file
        path.write_bytes(content)
        profile = read_profile(path)
        problem = macpack.profile_problem(profile, team or (profile.get("TeamIdentifier") or ["?"])[0],
                                          ident, needs, now.replace(tzinfo=None))
        if problem:
            raise SystemExit(f"provision: Apple's profile for {ident} fails the release's check: {problem}")
        say(f"Profile for {ident}: saved to {path}, checked")
        saved.append(path)
        if set_secret:
            set_secret(secret, base64.b64encode(content))
            say(f"  secret {secret} set on {REPO}")
    return saved


def gh_secret(name: str, value: bytes) -> None:
    proc = subprocess.run(["gh", "secret", "set", name, "--repo", REPO], input=value, check=False)
    if proc.returncode != 0:
        raise SystemExit(f"provision: gh secret set {name} failed")


def main(argv: list[str]) -> None:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--key", required=True, help="the App Store Connect API key (.p8)")
    p.add_argument("--key-id", required=True)
    p.add_argument("--issuer-id", required=True)
    p.add_argument("--bundle-id", default=APP_BUNDLE_ID)
    p.add_argument("--certificate", help="the Developer ID Application certificate's serial number or id, "
                                         "when the team has more than one")
    p.add_argument("--team-id", default=os.environ.get("APPLE_TEAM_ID") or None)
    p.add_argument("--out", default="~/Dev/.mv-toolchain/profiles", help="where the profiles are saved")
    p.add_argument("--dry-run", action="store_true", help="read only: say what it would change")
    p.add_argument("--set-secrets", action="store_true", help=f"also set both profile secrets on {REPO}")
    args = p.parse_args(argv)
    key = Path(args.key).expanduser()
    if not key.is_file():
        raise SystemExit(f"provision: {key} not found")
    api = Api(make_token(key, args.key_id, args.issuer_id))
    run(args, api, set_secret=gh_secret if args.set_secrets and not args.dry_run else None)


if __name__ == "__main__":
    main(sys.argv[1:])
