#!/usr/bin/env python3
"""Exercise GTK 4 on a private, software-rendered Wayland server, never the desktop."""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


def run(command, *, env=None, private_group=False):
    process = subprocess.Popen(command, env=env, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, start_new_session=private_group)
    try:
        output, _ = process.communicate(timeout=90)
    except subprocess.TimeoutExpired:
        if private_group:
            os.killpg(process.pid, signal.SIGKILL)
        else:
            process.kill()
        process.communicate()
        raise
    print(output, end="", flush=True)
    if process.returncode:
        raise RuntimeError(f"Command exited {process.returncode}: {command[0]}")
    return output


def inside(args):
    evidence = args.evidence.resolve()
    (evidence / "results.json").unlink(missing_ok=True)
    fixture = args.fixture.resolve()
    original_hash = hashlib.sha256(fixture.read_bytes()).hexdigest()
    # Add a second page to a disposable COPY to exercise page changes.
    root = ET.fromstring(gzip.decompress(fixture.read_bytes()))
    root.append(ET.fromstring(ET.tostring(root.find("page"))))
    copy = evidence / "fixture.xopp"
    copy.write_bytes(gzip.compress(ET.tostring(root, encoding="utf-8", xml_declaration=True)))
    pdf = evidence / "fixture.pdf"
    run([str(args.renderer.resolve()), f"--create-pdf={pdf}", str(copy)])
    results = {}
    for dark in (False, True):
        name = 'dark' if dark else 'light'
        output = evidence / name
        screenshots = ("initial", "ink", "tools", "zoom", "narrow", "sidebar")
        if args.worker:
            screenshots += ("document-dirty", "document-saved", "unsaved", "open-error", "reopened",
                            "save-prompt", "saved-again", "discard-prompt", "normal-saved", "save-conflict",
                            "conflict-resolved")
            for filename in ("saved-copy.xopp", "saved-again.xopp", "normal-save.xopp"):
                (output / filename).unlink(missing_ok=True)
        for screenshot in screenshots:
            (output / f"{screenshot}.png").unlink(missing_ok=True)
        env = dict(os.environ, G_DEBUG="fatal-criticals")
        command = [str(args.binary.resolve()), "--smoke-dir", str(output)]
        if args.worker:
            command.extend(["--document", str(copy), "--worker", str(args.worker.resolve())])
        else:
            command.extend(["--pdf", str(pdf)])
        if dark:
            command.append("--dark")
        if os.environ.get("XOURNALPP_PROTOTYPE_GDB"):
            command = ["gdb", "--batch", "--return-child-result", "-ex", "run", "-ex", "bt", "--args", *command]
        results[name] = run(command, env=env)
        if "PASS: GTK4" not in results[name]:
            raise RuntimeError("Prototype did not finish its feature checks")
        if args.worker:
            if "PASS: document actions" not in results[name]:
                raise RuntimeError("Document action checks did not finish")
            first = ET.fromstring(gzip.decompress((output / "saved-copy.xopp").read_bytes()))
            second = ET.fromstring(gzip.decompress((output / "saved-again.xopp").read_bytes()))
            if len(first.findall(".//stroke")) != 4 or len(second.findall(".//stroke")) != 4:
                raise RuntimeError("Saved/reopened copies duplicated or lost session ink")
            normal = ET.fromstring(gzip.decompress((output / "normal-save.xopp").read_bytes()))
            if len(normal.findall(".//stroke")) != 5 or (output / "normal-save.xopp~").exists():
                raise RuntimeError("Normal Save lost or duplicated ink, or retained a successful-save backup")
            if len(first.findall(".//text")) != len(root.findall(".//text")):
                raise RuntimeError("Saving session ink lost original text")
            width = list(map(float, second.findall(".//stroke")[-1].attrib["width"].split()))
            if len(width) < 2 or abs(width[1] - width[0] * 0.25) > 0.00001:
                raise RuntimeError("Native save lost stylus pressure mode or width")
        for screenshot in screenshots:
            path = output / f"{screenshot}.png"
            if not path.is_file() or path.stat().st_size < 1000:
                raise RuntimeError(f"Missing or empty screenshot: {path}")
        if (output / "initial.png").read_bytes() == (output / "ink.png").read_bytes():
            raise RuntimeError("Committed ink did not change the window snapshot")
        initial_width = struct.unpack(">I", (output / "initial.png").read_bytes()[16:20])[0]
        narrow_width = struct.unpack(">I", (output / "narrow.png").read_bytes()[16:20])[0]
        if not 0 < narrow_width < initial_width:
            raise RuntimeError("Screenshot did not capture a narrower viewport")
    if hashlib.sha256(fixture.read_bytes()).hexdigest() != original_hash:
        raise RuntimeError("Input fixture changed")
    report = {
        "checks": results,
        "fixture": str(fixture),
        "fixture_sha256": original_hash,
        "renderer_sha256": hashlib.sha256(args.renderer.read_bytes()).hexdigest(),
        "document_worker_sha256": hashlib.sha256(args.worker.read_bytes()).hexdigest() if args.worker else None,
        "input_coverage": "Synthetic controller signals; no physical hardware events",
        "rendering": "Private headless Wayland, Pixman and Cairo; device scale 1; explicit 2x raster-cache test",
    }
    (evidence / "results.json").write_text(json.dumps(report, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--renderer", type=Path, required=True,
                        help="Existing GTK 3 xournalpp executable, used only for CLI export")
    parser.add_argument("--worker", type=Path, help="Open native XOPP through the separate document worker")
    parser.add_argument("--fixture", type=Path,
                        default=Path(__file__).resolve().parents[2] / "test/files/benchmark/typed-text.xopp")
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=True)
    if args.inside:
        inside(args)
        return

    for tool in ("cage", "dbus-run-session"):
        if not shutil.which(tool):
            raise RuntimeError(f"Isolated testing requires {tool}; no desktop fallback is allowed")
    with tempfile.TemporaryDirectory(prefix="xournalpp-gtk4-test-") as directory:
        private = Path(directory)
        env = dict(os.environ)
        # Remove every inherited connection to the user's graphical session.
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DBUS_SESSION_BUS_ADDRESS",
                    "SWAYSOCK", "NIRI_SOCKET", "GTK_THEME", "GDK_SCALE", "GDK_DPI_SCALE"):
            env.pop(key, None)
        for variable, name in (("XDG_RUNTIME_DIR", "runtime"), ("XDG_CONFIG_HOME", "config"),
                               ("XDG_CACHE_HOME", "cache"), ("XDG_DATA_HOME", "data"),
                               ("XDG_STATE_HOME", "state"), ("TMPDIR", "tmp")):
            path = private / name
            path.mkdir(mode=0o700)
            env[variable] = str(path)
        env.update(WLR_BACKENDS="headless", WLR_HEADLESS_OUTPUTS="1", WLR_RENDERER="pixman",
                   GSK_RENDERER="cairo", GDK_BACKEND="wayland", GTK_A11Y="none", NO_AT_BRIDGE="1",
                   LIBGL_ALWAYS_SOFTWARE="1", GSETTINGS_BACKEND="memory")
        bus_config = private / "bus.conf"
        bus_config.write_text("""<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=""" + str(private / "runtime") + """</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*"/>
    <allow receive_sender="*"/>
    <allow own="*"/>
  </policy>
</busconfig>
""")
        # No service directories: tests must not activate portals, keyrings, or desktop services.
        command = ["dbus-run-session", f"--config-file={bus_config}", "--", "cage", "--", sys.executable, str(Path(__file__).resolve()),
                   "--inside", "--binary", str(args.binary.resolve()), "--renderer", str(args.renderer.resolve()),
                   "--fixture", str(args.fixture.resolve()), "--evidence", str(args.evidence.resolve())]
        if args.worker:
            command.extend(["--worker", str(args.worker.resolve())])
        output = run(command, env=env, private_group=True)
        (args.evidence / "headless.log").write_text(output)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
