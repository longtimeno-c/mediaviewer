#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""MediaViewer.app packaging (PR 20, plan/13 "macOS first install", plan/15).

  macpack.py assemble ...   build/MediaViewer.app, ad-hoc signed (CMake's
                            `mediaviewer_app` target runs this)
  macpack.py release ...    Developer ID sign, notarize + staple the app,
                            branded disk image with the GPL on mount,
                            notarize + staple the image, the Sparkle update
                            archive, and (given Sparkle's bin/) the signed
                            appcast

Only macOS tools are used (otool, install_name_tool, codesign, sips,
iconutil, ditto, hdiutil, xcrun notarytool/stapler) plus dmgbuild
(tools/mac/requirements.txt) for the disk image. The pure parsing helpers at
the top have no macOS dependency; tools/mac/test_macpack.py runs them.

Licensing (CLAUDE.md): FFmpeg, libheif, libde265 and LibRaw are dynamic-link
only. They land in Contents/Frameworks as separate dylibs, never merged into
the executable.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable, Iterable

APP_NAME = "MediaViewer"
APPEX_NAME = "MediaViewerThumbnails"
REPO_ROOT = Path(__file__).resolve().parents[2]

# ---------------------------------------------------------------------------
# Pure helpers (no macOS needed): tested by tools/mac/test_macpack.py.
# ---------------------------------------------------------------------------

_OTOOL_DEP = re.compile(r"^\s+(\S.*?) \(compatibility version")
_OTOOL_RPATH = re.compile(r"^\s+path (.+?) \(offset \d+\)")


def parse_otool_deps(output: str, own_id: str | None = None) -> list[str]:
    """Install names from `otool -L`. A dylib lists its own id first; drop it."""
    deps: list[str] = []
    for line in output.splitlines()[1:]:
        m = _OTOOL_DEP.match(line)
        if m and m.group(1) != own_id:
            deps.append(m.group(1))
    return deps


def parse_otool_rpaths(output: str) -> list[str]:
    """LC_RPATH entries from `otool -l`."""
    rpaths: list[str] = []
    lines = output.splitlines()
    for i, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH":
            for follow in lines[i + 1:i + 4]:
                m = _OTOOL_RPATH.match(follow)
                if m:
                    rpaths.append(m.group(1))
                    break
    return rpaths


def is_system_dep(name: str) -> bool:
    return name.startswith("/usr/lib/") or name.startswith("/System/")


def is_sparkle_dep(name: str) -> bool:
    return "Sparkle.framework/" in name


def resolve_dep(name: str, referrer_dir: str, executable_dir: str, rpaths: Iterable[str],
                extra_dirs: Iterable[str], exists: Callable[[str], bool] = os.path.exists) -> str | None:
    """Where on disk an install name points, the way dyld would find it.

    `referrer_dir` is the directory of the binary that carries the reference
    (its original location, before it was copied into the bundle).
    `extra_dirs` is the vcpkg lib dir, a last resort for @rpath names whose
    rpath was a build-tree path.
    """
    def expand(prefix_path: str) -> str:
        return (prefix_path.replace("@loader_path", referrer_dir)
                .replace("@executable_path", executable_dir))

    if name.startswith("@rpath/"):
        rest = name[len("@rpath/"):]
        for rp in list(rpaths) + list(extra_dirs):
            candidate = os.path.normpath(os.path.join(expand(rp), rest))
            if exists(candidate):
                return candidate
        return None
    if name.startswith("@loader_path/") or name.startswith("@executable_path/"):
        candidate = os.path.normpath(expand(name))
        return candidate if exists(candidate) else None
    if name.startswith("/"):
        return name if exists(name) else None
    return None


def bundled_name(install_name: str) -> str:
    """The file name a dependency gets in Contents/Frameworks."""
    return os.path.basename(install_name)


def check_bundle_refs(refs: dict[str, list[str]], frameworks: set[str]) -> list[str]:
    """Every reference must be the system, Sparkle, or @rpath/<a bundled dylib>."""
    problems: list[str] = []
    for binary, deps in sorted(refs.items()):
        for dep in deps:
            if is_system_dep(dep) or is_sparkle_dep(dep):
                continue
            if dep.startswith("@rpath/") and dep[len("@rpath/"):] in frameworks:
                continue
            problems.append(f"{binary}: {dep}")
    return problems


# ---------------------------------------------------------------------------
# macOS tool wrappers
# ---------------------------------------------------------------------------

def run(cmd: list[str], *, capture: bool = False, check: bool = True, stdin: str | None = None) -> str:
    print("+", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, check=False, text=True, input=stdin,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.STDOUT if capture else None)
    if check and proc.returncode != 0:
        if capture and proc.stdout:
            print(proc.stdout, file=sys.stderr)
        raise SystemExit(f"macpack: command failed ({proc.returncode}): {cmd[0]}")
    return proc.stdout or ""


def otool_id(path: Path) -> str | None:
    out = subprocess.run(["otool", "-D", str(path)], text=True, capture_output=True).stdout
    lines = [l for l in out.splitlines()[1:] if l.strip()]
    return lines[0].strip() if lines else None


def otool_deps(path: Path) -> list[str]:
    out = run(["otool", "-L", str(path)], capture=True)
    return parse_otool_deps(out, otool_id(path))


def otool_rpaths(path: Path) -> list[str]:
    return parse_otool_rpaths(run(["otool", "-l", str(path)], capture=True))


def is_macho(path: Path) -> bool:
    if not path.is_file() or path.is_symlink():
        return False
    with path.open("rb") as f:
        magic = f.read(4)
    return magic in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")


# ---------------------------------------------------------------------------
# assemble
# ---------------------------------------------------------------------------

def make_icns(png: Path, out: Path) -> None:
    """One mark (plan/13 Icon): 16-512 at @1x and @2x, from the same PNG as
    the Windows .ico."""
    with tempfile.TemporaryDirectory() as tmp:
        iconset = Path(tmp) / f"{APP_NAME}.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = size * scale
                suffix = "" if scale == 1 else "@2x"
                run(["sips", "-z", str(px), str(px), str(png), "--out",
                     str(iconset / f"icon_{size}x{size}{suffix}.png")], capture=True)
        run(["iconutil", "-c", "icns", str(iconset), "-o", str(out)])


def bundle_dylibs(app: Path, roots: list[tuple[Path, str]], dylib_dirs: list[str]) -> None:
    """Copy every non-system dylib the executables need into
    Contents/Frameworks and point every reference at @rpath/<name>.

    `roots` is (executable in the bundle, rpath to Frameworks from it).
    """
    frameworks = app / "Contents" / "Frameworks"
    frameworks.mkdir(parents=True, exist_ok=True)
    main_exe_dir = str(app / "Contents" / "MacOS")

    # (binary in bundle, its original directory for @loader_path, its rpaths)
    queue: list[tuple[Path, str, list[str]]] = []
    for exe, _ in roots:
        queue.append((exe, str(exe.parent), otool_rpaths(exe)))
    copied: dict[str, Path] = {}

    while queue:
        binary, origin_dir, rpaths = queue.pop(0)
        for dep in otool_deps(binary):
            if is_system_dep(dep) or is_sparkle_dep(dep):
                continue
            name = bundled_name(dep)
            if name not in copied:
                src = resolve_dep(dep, origin_dir, main_exe_dir, rpaths, dylib_dirs)
                if src is None:
                    raise SystemExit(f"macpack: cannot resolve {dep} (needed by {binary.name}); "
                                     "is MV_VCPKG_DYNAMIC_PREFIX right?")
                dst = frameworks / name
                real = Path(src).resolve()
                shutil.copyfile(real, dst)
                dst.chmod(0o755)
                copied[name] = dst
                run(["install_name_tool", "-id", f"@rpath/{name}", str(dst)])
                queue.append((dst, str(real.parent), otool_rpaths(dst)))
            if dep != f"@rpath/{name}":
                run(["install_name_tool", "-change", dep, f"@rpath/{name}", str(binary)])

    # rpaths: nothing may point outside the bundle (a build tree, a vcpkg dir).
    def reset_rpaths(binary: Path, wanted: str) -> None:
        for rp in otool_rpaths(binary):
            if rp != wanted:
                run(["install_name_tool", "-delete_rpath", rp, str(binary)])
        if wanted not in otool_rpaths(binary):
            run(["install_name_tool", "-add_rpath", wanted, str(binary)])

    for exe, rpath in roots:
        reset_rpaths(exe, rpath)
    for dylib in copied.values():
        reset_rpaths(dylib, "@loader_path")

    refs = {str(p.relative_to(app)): otool_deps(p) for p in [e for e, _ in roots] + list(copied.values())}
    problems = check_bundle_refs(refs, set(copied))
    if problems:
        raise SystemExit("macpack: references outside the bundle:\n  " + "\n  ".join(problems))


def copy_sparkle(framework: Path, app: Path) -> None:
    dst = app / "Contents" / "Frameworks" / "Sparkle.framework"
    run(["ditto", str(framework), str(dst)])
    # The app is not sandboxed, so Sparkle's XPC services are not needed
    # (Sparkle's documentation); removing them is fewer binaries to sign.
    xpc = dst / "Versions" / "B" / "XPCServices"
    if xpc.exists():
        shutil.rmtree(xpc)
    xpc_link = dst / "XPCServices"
    if xpc_link.is_symlink():
        xpc_link.unlink()


def cmd_assemble(args: argparse.Namespace) -> None:
    app = Path(args.app)
    if app.exists():
        shutil.rmtree(app)
    contents = app / "Contents"
    (contents / "MacOS").mkdir(parents=True)
    (contents / "Resources").mkdir()
    (contents / "Frameworks").mkdir()

    main_exe = contents / "MacOS" / APP_NAME
    shutil.copyfile(args.exe, main_exe)
    main_exe.chmod(0o755)
    shutil.copyfile(args.info_plist, contents / "Info.plist")
    (contents / "PkgInfo").write_text("APPL????")
    make_icns(Path(args.icon_png), contents / "Resources" / f"{APP_NAME}.icns")
    shutil.copyfile(args.font, contents / "Resources" / Path(args.font).name)
    # The licence and third-party notices ship with the binary they describe.
    shutil.copyfile(REPO_ROOT / "LICENSE", contents / "Resources" / "LICENSE.txt")
    shutil.copyfile(REPO_ROOT / "THIRD-PARTY.md", contents / "Resources" / "THIRD-PARTY.md")

    appex = contents / "PlugIns" / f"{APPEX_NAME}.appex" / "Contents"
    (appex / "MacOS").mkdir(parents=True)
    appex_exe = appex / "MacOS" / APPEX_NAME
    shutil.copyfile(args.appex_exe, appex_exe)
    appex_exe.chmod(0o755)
    shutil.copyfile(args.appex_plist, appex / "Info.plist")

    if args.sparkle:
        copy_sparkle(Path(args.sparkle), app)

    bundle_dylibs(app, [
        (main_exe, "@executable_path/../Frameworks"),
        (appex_exe, "@executable_path/../../../../Frameworks"),
    ], [args.dylib_dir])

    sign_app(app, identity="-", appex_entitlements=Path(args.appex_entitlements), hardened=False)
    print(f"macpack: {app} (ad-hoc signed; `macpack.py release` for a shippable build)")


# ---------------------------------------------------------------------------
# signing
# ---------------------------------------------------------------------------

def codesign(path: Path, identity: str, hardened: bool, entitlements: Path | None = None,
             preserve_entitlements: bool = False) -> None:
    cmd = ["codesign", "--force", "--sign", identity]
    if identity == "-":
        cmd.append("--timestamp=none")
    else:
        cmd.append("--timestamp")  # notarization requires a secure timestamp
    if hardened:
        cmd += ["--options", "runtime"]
    if entitlements:
        cmd += ["--entitlements", str(entitlements)]
    elif preserve_entitlements:
        cmd.append("--preserve-metadata=entitlements")
    cmd.append(str(path))
    run(cmd)


def sign_app(app: Path, identity: str, appex_entitlements: Path, hardened: bool) -> None:
    """Inside out: every nested binary before the bundle that seals it. No
    --deep: it signs everything with one set of options, which is wrong for
    the sandboxed extension."""
    frameworks = app / "Contents" / "Frameworks"
    for dylib in sorted(frameworks.glob("*.dylib")):
        codesign(dylib, identity, hardened)
    sparkle = frameworks / "Sparkle.framework"
    if sparkle.exists():
        version = sparkle / "Versions" / "B"
        codesign(version / "Autoupdate", identity, hardened)
        codesign(version / "Updater.app", identity, hardened, preserve_entitlements=True)
        codesign(sparkle, identity, hardened)
    appex = app / "Contents" / "PlugIns" / f"{APPEX_NAME}.appex"
    codesign(appex, identity, hardened, entitlements=appex_entitlements)
    codesign(app, identity, hardened)
    run(["codesign", "--verify", "--strict", "--deep", "--verbose=2", str(app)])


# ---------------------------------------------------------------------------
# release
# ---------------------------------------------------------------------------

def notarize(path: Path, profile: str) -> None:
    """`notarytool --wait` exits 0 for a rejected submission too; read the status."""
    out = run(["xcrun", "notarytool", "submit", str(path), "--keychain-profile", profile,
               "--wait", "--output-format", "json"], capture=True)
    status = json.loads(out).get("status")
    if status != "Accepted":
        raise SystemExit(f"macpack: notarization {status!r} for {path.name}; "
                         "`xcrun notarytool log <id>` has the reasons")


def make_dmg(app: Path, out: Path, volume_name: str) -> None:
    settings = REPO_ROOT / "packaging" / "macos" / "dmg_settings.py"
    run([sys.executable, "-m", "dmgbuild", "-s", str(settings),
         "-D", f"app={app}",
         "-D", f"license={REPO_ROOT / 'LICENSE'}",
         "-D", f"background={REPO_ROOT / 'packaging' / 'macos' / 'dmg-background.png'}",
         volume_name, str(out)])


def cmd_release(args: argparse.Namespace) -> None:
    app = Path(args.app)
    if not app.exists():
        raise SystemExit(f"macpack: {app} not found; build the mediaviewer_app target first")
    info = app / "Contents" / "Info.plist"
    version = run(["/usr/libexec/PlistBuddy", "-c", "Print :CFBundleShortVersionString", str(info)],
                  capture=True).strip()
    has_sparkle = (app / "Contents" / "Frameworks" / "Sparkle.framework").exists()
    if not has_sparkle and not args.allow_no_updater:
        raise SystemExit("macpack: this build has no updater (configure with "
                         "MV_SPARKLE_PUBLIC_ED_KEY). plan/13: the first build that reaches "
                         "anyone else's machine must be able to update. --allow-no-updater "
                         "overrides for a private test build.")

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    entitlements = REPO_ROOT / "packaging" / "macos" / "QuickLook.entitlements"

    # 1. Developer ID + hardened runtime, inside out.
    sign_app(app, identity=args.identity, appex_entitlements=entitlements, hardened=True)

    # 2. Notarize and staple the app itself, so a copy dragged out of the image
    #    opens offline too.
    if not args.skip_notarize:
        with tempfile.TemporaryDirectory() as tmp:
            zipped = Path(tmp) / f"{APP_NAME}.zip"
            run(["ditto", "-c", "-k", "--keepParent", str(app), str(zipped)])
            notarize(zipped, args.notary_profile)
        run(["xcrun", "stapler", "staple", str(app)])
        run(["spctl", "--assess", "--type", "execute", "--verbose=2", str(app)])

    # 3. First install: the branded disk image, GPL on mount (plan/13).
    dmg = out / f"{APP_NAME}-{version}.dmg"
    if dmg.exists():
        dmg.unlink()
    make_dmg(app, dmg, APP_NAME)
    codesign(dmg, args.identity, hardened=False)
    if not args.skip_notarize:
        notarize(dmg, args.notary_profile)
        run(["xcrun", "stapler", "staple", str(dmg)])
        run(["spctl", "--assess", "--type", "open", "--context", "context:primary-signature",
             "--verbose=2", str(dmg)])

    # 4. Every later update: Sparkle takes the stapled app as a zip, never the
    #    disk image (the licence agreement is a first-install page).
    updates = out / "updates"
    updates.mkdir(exist_ok=True)
    archive = updates / f"{APP_NAME}-{version}.zip"
    if archive.exists():
        archive.unlink()
    run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", str(app), str(archive)])

    # 5. The signed appcast. generate_appcast signs the enclosure and embeds a
    #    feed signature with the private key from the login keychain;
    #    SURequireSignedFeed makes the app refuse anything else.
    if has_sparkle and args.sparkle_bin:
        cmd = [str(Path(args.sparkle_bin) / "generate_appcast")]
        if args.download_url_prefix:
            cmd += ["--download-url-prefix", args.download_url_prefix]
        if args.phased_rollout_seconds:
            cmd += ["--phased-rollout-interval", str(args.phased_rollout_seconds)]
        cmd.append(str(updates))
        run(cmd)
        feed = (updates / "appcast.xml").read_text()
        if "sparkle-signatures" not in feed:
            raise SystemExit("macpack: appcast.xml carries no feed signature; "
                             "the app (SURequireSignedFeed) would reject it")
    print(f"macpack: first install {dmg}\nmacpack: update archive {archive}")


def main(argv: list[str]) -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    a = sub.add_parser("assemble", help="build MediaViewer.app from the CMake outputs")
    a.add_argument("--app", required=True)
    a.add_argument("--exe", required=True)
    a.add_argument("--appex-exe", required=True)
    a.add_argument("--info-plist", required=True)
    a.add_argument("--appex-plist", required=True)
    a.add_argument("--appex-entitlements", required=True)
    a.add_argument("--icon-png", required=True)
    a.add_argument("--font", required=True)
    a.add_argument("--dylib-dir", required=True)
    a.add_argument("--sparkle")
    a.set_defaults(func=cmd_assemble)

    r = sub.add_parser("release", help="sign, notarize, disk image, update archive, appcast")
    r.add_argument("--app", default="build/MediaViewer.app")
    r.add_argument("--identity", default=os.environ.get("MV_SIGN_IDENTITY"),
                   help='"Developer ID Application: …" (or $MV_SIGN_IDENTITY)')
    r.add_argument("--notary-profile", default=os.environ.get("MV_NOTARY_PROFILE"),
                   help="`xcrun notarytool store-credentials` profile (or $MV_NOTARY_PROFILE)")
    r.add_argument("--out-dir", default="build/release")
    r.add_argument("--skip-notarize", action="store_true", help="local dry run only")
    r.add_argument("--allow-no-updater", action="store_true")
    r.add_argument("--sparkle-bin", help="Sparkle's bin/ directory (generate_appcast)")
    r.add_argument("--download-url-prefix",
                   help="where the update zips are served, e.g. a GitHub release URL")
    r.add_argument("--phased-rollout-seconds", type=int, default=0,
                   help="Sparkle phased rollout interval (plan/13 staged rollout)")
    r.set_defaults(func=cmd_release)

    args = parser.parse_args(argv)
    if args.command == "release":
        if not args.identity:
            parser.error("--identity (or MV_SIGN_IDENTITY) is required")
        if not args.skip_notarize and not args.notary_profile:
            parser.error("--notary-profile (or MV_NOTARY_PROFILE) is required")
    args.func(args)


if __name__ == "__main__":
    main(sys.argv[1:])
