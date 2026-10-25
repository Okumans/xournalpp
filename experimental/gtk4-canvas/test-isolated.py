#!/usr/bin/env python3
"""Exercise GTK 4 on a private, software-rendered Wayland server, never the desktop."""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
import zlib


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


def crash_after_autosave(command, marker_path, env):
    marker_path.unlink(missing_ok=True)
    crash_env = dict(env, G_DEBUG="fatal-criticals", XOURNALPP_CRASH_AFTER_AUTOSAVE_MARKER=str(marker_path))
    process = subprocess.Popen(command, env=crash_env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 25.0
        while not marker_path.is_file():
            if process.poll() is not None:
                output, _ = process.communicate(timeout=5)
                raise RuntimeError(f"Crash helper exited early ({process.returncode}):\n{output}")
            if time.monotonic() > deadline:
                raise RuntimeError("Timed out waiting for periodic autosave before crash")
            time.sleep(0.05)
        autosave_path = Path(marker_path.read_text().strip())
        process.send_signal(signal.SIGKILL)
        process.communicate(timeout=10)
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate(timeout=5)
    if not autosave_path.is_file():
        raise RuntimeError(f"Autosave file missing after SIGKILL: {autosave_path}")
    return autosave_path


def verify_crash_recovery(args, evidence, copy, root):
    binary = str(args.binary.resolve())
    worker = str(args.worker.resolve())
    crash_dir = evidence / "crash-recovery"
    shutil.rmtree(crash_dir, ignore_errors=True)
    crash_dir.mkdir(parents=True, exist_ok=True)
    base_env = dict(os.environ, G_DEBUG="fatal-criticals")

    # 1. Unsaved blank notebook: periodic cache autosave -> SIGKILL -> blank startup Restore -> Save As.
    cache_autosave = crash_after_autosave(
        [binary, "--worker", worker, "--autosave-seconds", "1"],
        crash_dir / "blank.marker",
        base_env,
    )
    restored_blank = crash_dir / "restored-blank.xopp"
    run([binary, "--worker", worker], env=dict(base_env, XOURNALPP_RECOVERY_ACTION=f"restore:{restored_blank}"))
    if cache_autosave.exists():
        raise RuntimeError("Orphaned cache autosave remained after Restore + Save As")
    autosaves_dir = Path(os.environ["XDG_CACHE_HOME"]) / "xournalpp" / "autosaves"
    if autosaves_dir.is_dir() and list(autosaves_dir.glob("*.autosave.xopp")):
        raise RuntimeError("Adopted cache autosave was not removed after Save As")
    blank_xml = ET.fromstring(gzip.decompress(restored_blank.read_bytes()))
    strokes = blank_xml.findall(".//stroke")
    if len(strokes) != 1 or len(strokes[0].attrib["width"].split()) < 2:
        raise RuntimeError("Restored cache autosave lost its pressure-sensitive stroke")

    # 2. Associated native document: periodic .name.autosave.xopp -> SIGKILL -> reopen Recover -> Ctrl+S.
    saved_doc = crash_dir / "associated.xopp"
    shutil.copyfile(copy, saved_doc)
    saved_before_hash = hashlib.sha256(saved_doc.read_bytes()).hexdigest()
    sidecar = crash_after_autosave(
        [binary, "--document", str(saved_doc), "--worker", worker, "--autosave-seconds", "1"],
        crash_dir / "associated.marker",
        base_env,
    )
    if sidecar != crash_dir / ".associated.autosave.xopp":
        raise RuntimeError(f"Unexpected sidecar autosave path: {sidecar}")
    if hashlib.sha256(saved_doc.read_bytes()).hexdigest() != saved_before_hash:
        raise RuntimeError("Autosave or SIGKILL altered the saved primary document")
    run([binary, "--document", str(saved_doc), "--worker", worker],
        env=dict(base_env, XOURNALPP_RECOVERY_ACTION="recover"))
    if sidecar.exists() or Path(str(saved_doc) + "~").exists():
        raise RuntimeError("Recovered sidecar or temporary backup remained after normal Save")
    recovered_xml = ET.fromstring(gzip.decompress(saved_doc.read_bytes()))
    if len(recovered_xml.findall(".//stroke")) != len(root.findall(".//stroke")) + 1:
        raise RuntimeError("Cross-session sidecar recovery lost or duplicated strokes")
    if len(recovered_xml.findall(".//text")) != len(root.findall(".//text")):
        raise RuntimeError("Cross-session sidecar recovery lost native text")

    # 3. Emergency save (<XDG_CONFIG_HOME>/xournalpp/emergencysave.xopp): startup Restore and Delete flows.
    emergency = Path(os.environ["XDG_CONFIG_HOME"]) / "xournalpp" / "emergencysave.xopp"
    emergency_ink = crash_dir / "emergency.ink"
    emergency_ink.write_text("XOURNALPP_GTK4_INK_V1 1\n0 0 1 3 0 0 1 2\n40 40 .4\n90 60 .9\n")
    run([worker, "autosave", "", str(emergency), str(emergency_ink)], env=base_env)
    restored_emergency = crash_dir / "restored-emergency.xopp"
    run([binary, "--worker", worker], env=dict(base_env, XOURNALPP_RECOVERY_ACTION=f"restore:{restored_emergency}"))
    if emergency.exists() or len(ET.fromstring(gzip.decompress(restored_emergency.read_bytes())).findall(".//stroke")) != 1:
        raise RuntimeError("Emergency save restore did not consume emergencysave.xopp or preserve ink")
    run([worker, "autosave", "", str(emergency), str(emergency_ink)], env=base_env)
    run([binary, "--worker", worker], env=dict(base_env, XOURNALPP_RECOVERY_ACTION="delete"))
    if emergency.exists():
        raise RuntimeError("Delete file response did not remove emergencysave.xopp")
    print("PASS: cross-process SIGKILL autosave/emergency recovery (cache, sidecar, emergencysave.xopp)", flush=True)


def snapshot_tree(root_dir):
    entries = {}
    for item in sorted(root_dir.rglob("*")):
        rel = str(item.relative_to(root_dir))
        st = item.lstat()
        digest = hashlib.sha256(item.read_bytes()).hexdigest() if item.is_file() and not item.is_symlink() else "<dir>"
        entries[rel] = (st.st_mode, st.st_size, st.st_mtime_ns, digest)
    return entries


def verify_settings_import(args, evidence, copy):
    binary = str(args.binary.resolve())
    worker = str(args.worker.resolve())
    repo_root = Path(__file__).resolve().parents[2]
    xournalpp_gpl = repo_root / "palettes" / "xournalpp.gpl"
    settings_root = evidence / "settings-import"
    shutil.rmtree(settings_root, ignore_errors=True)
    settings_root.mkdir(parents=True, exist_ok=True)
    base_env = dict(os.environ, G_DEBUG="fatal-criticals")

    for mode_name, theme_variant, verify_mode in (
        ("light", "forceLight", "imported-light"),
        ("dark", "forceDark", "imported-dark"),
    ):
        cfg_dir = settings_root / f"config-{mode_name}"
        (cfg_dir / "palettes").mkdir(parents=True, exist_ok=True)
        shutil.copyfile(xournalpp_gpl, cfg_dir / "palettes" / "xournalpp.gpl")
        (cfg_dir / "settings.xml").write_text(f"""<?xml version="1.0" encoding="UTF-8"?>
<!--The Xournal++ settings file. Do not edit this file!-->
<settings>
  <property name="pressureSensitivity" value="true"/>
  <property name="minimumPressure" value="0.15"/>
  <property name="pressureMultiplier" value="1.2"/>
  <property name="snapRotationTolerance" value="0.55"/>
  <property name="zoomStep" value="25.00000000"/>
  <property name="zoomStepScroll" value="5.00000000"/>
  <property name="themeVariant" value="{theme_variant}"/>
  <property name="autosaveEnabled" value="false"/>
  <property name="autosaveTimeout" value="5"/>
  <property name="colorPalette" value="palettes/xournalpp.gpl"/>
  <data name="tools">
    <attribute name="current" type="string" value="highlighter"/>
    <data name="pen">
      <attribute name="color" type="hex" value="ff002e99"/>
      <attribute name="size" type="string" value="BIG"/>
    </data>
    <data name="highlighter">
      <attribute name="color" type="hex" value="ffffa154"/>
      <attribute name="size" type="string" value="VERY_BIG"/>
    </data>
    <data name="eraser">
      <attribute name="size" type="string" value="BIG"/>
      <attribute name="type" type="string" value="deleteStroke"/>
    </data>
  </data>
</settings>
""", encoding="utf-8")
        before_tree = snapshot_tree(cfg_dir)
        out_dir = settings_root / mode_name
        out_dir.mkdir(parents=True, exist_ok=True)
        doc_copy = out_dir / "input.xopp"
        shutil.copyfile(copy, doc_copy)
        out = run(
            [binary, "--config-dir", str(cfg_dir), "--document", str(doc_copy), "--worker", worker,
             "--smoke-dir", str(out_dir)],
            env=dict(base_env, XOURNALPP_SETTINGS_VERIFY_MODE=verify_mode),
        )
        if f"PASS: read-only settings.xml and .gpl palette import ({verify_mode})" not in out:
            raise RuntimeError(f"Settings import verification did not pass for {mode_name}")
        if snapshot_tree(cfg_dir) != before_tree:
            raise RuntimeError(f"Prototype mutated files inside --config-dir during {mode_name} import")
        for shot in ("settings-imported", "palette-popover", "palette-drawn"):
            shot_path = out_dir / f"{shot}.png"
            if not shot_path.is_file() or shot_path.stat().st_size < 1000:
                raise RuntimeError(f"Missing settings import screenshot: {shot_path}")
        saved_xml = ET.fromstring(gzip.decompress((out_dir / "settings-saved.xopp").read_bytes()))
        page0_strokes = saved_xml.find("page").findall(".//stroke")
        if len(page0_strokes) < 3:
            raise RuntimeError("Saved notebook after settings/palette import is missing expected strokes")
        hi_stroke, pen_stroke, purple_stroke = page0_strokes[-3], page0_strokes[-2], page0_strokes[-1]
        if hi_stroke.attrib.get("tool") != "highlighter" or abs(float(hi_stroke.attrib["width"]) - 30.0) > 1e-4:
            raise RuntimeError("Imported Highlighter VERY_BIG preset was not serialized to .xopp")
        pen_widths = list(map(float, pen_stroke.attrib["width"].split()))
        if pen_stroke.attrib.get("color") != "#002e99ff" or len(pen_widths) < 2 or abs(pen_widths[1] - 2.26 * 0.15) > 1e-3:
            raise RuntimeError("Imported Pen color #002e99 or minimumPressure=0.15 was not preserved in .xopp")
        if purple_stroke.attrib.get("color") != "#cd9ef7ff":
            raise RuntimeError("Selected .gpl Purple swatch (#cd9ef7) was not serialized in .xopp")

    if (settings_root / "light" / "settings-imported.png").read_bytes() == (
        settings_root / "dark" / "settings-imported.png"
    ).read_bytes():
        raise RuntimeError("forceLight and forceDark settings.xml produced identical window snapshots")

    corrupt_cfg = settings_root / "config-corrupt"
    corrupt_cfg.mkdir(parents=True, exist_ok=True)
    (corrupt_cfg / "settings.xml").write_text("<settings><data name=\"tools\"><attribute name=\"color\"", encoding="utf-8")
    (corrupt_cfg / "broken.gpl").write_text("NOT A GIMP PALETTE\n999 999 999 Bad\n", encoding="utf-8")
    before_corrupt = snapshot_tree(corrupt_cfg)
    corrupt_out = settings_root / "corrupt"
    corrupt_out.mkdir(parents=True, exist_ok=True)
    corrupt_doc = corrupt_out / "input.xopp"
    shutil.copyfile(copy, corrupt_doc)
    fallback_out = run(
        [binary, "--config-dir", str(corrupt_cfg), "--document", str(corrupt_doc), "--worker", worker,
         "--smoke-dir", str(corrupt_out)],
        env=dict(base_env, XOURNALPP_SETTINGS_VERIFY_MODE="fallback"),
    )
    if "PASS: corrupt settings.xml and .gpl fallback" not in fallback_out:
        raise RuntimeError("Corrupt settings.xml fallback check did not pass")
    if snapshot_tree(corrupt_cfg) != before_corrupt:
        raise RuntimeError("Prototype mutated corrupt configuration files during fallback")
    print("PASS: read-only settings.xml and .gpl palette import (forceLight, forceDark, corrupt fallback, zero config writes)",
          flush=True)


def verify_layer_management(args, evidence):
    root = ET.Element("xournal", {"creator": "Xournal++", "fileversion": "4"})
    ET.SubElement(root, "title").text = "GTK4 layer management"
    page = ET.SubElement(root, "page", {"width": "595", "height": "842"})
    ET.SubElement(page, "background", {"type": "solid", "color": "#ffffffff", "style": "plain"})
    lower = ET.SubElement(page, "layer", {"name": "Lower"})
    ET.SubElement(lower, "stroke", {"tool": "pen", "capStyle": "round", "style": "dash",
                                     "color": "#222222ff", "width": "2"}).text = "20 30 80 30"
    ET.SubElement(lower, "stroke", {"tool": "pen", "capStyle": "round", "color": "#112233ff",
                                     "width": "2"}).text = "20 45 80 45"
    upper = ET.SubElement(page, "layer", {"name": "Upper"})
    ET.SubElement(upper, "stroke", {"tool": "pen", "capStyle": "round", "fill": "128",
                                     "color": "#008800ff", "width": "2"}).text = "20 60 80 60 50 75"
    ET.SubElement(upper, "stroke", {"tool": "highlighter", "capStyle": "round",
                                     "color": "#ffff007f", "width": "8"}).text = "20 90 80 90"
    root.append(ET.fromstring(ET.tostring(page)))

    directory = evidence / "layer-management"
    directory.mkdir(parents=True, exist_ok=True)
    fixture = directory / "two-layers.xopp"
    fixture.write_bytes(gzip.compress(ET.tostring(root, encoding="utf-8", xml_declaration=True)))
    output = directory / "run"
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True, exist_ok=True)
    result = run(
        [str(args.binary.resolve()), "--document", str(fixture), "--worker", str(args.worker.resolve()),
         "--smoke-dir", str(output)],
        env=dict(os.environ, G_DEBUG="fatal-criticals", XOURNALPP_LAYER_VERIFY_MODE="1"),
    )
    if ("PASS: GTK layer selection, add, rename, duplicate, reorder, visibility, merge, undo and save"
            not in result):
        raise RuntimeError("Layer management verification did not finish")
    for screenshot in ("layer-stack", "layer-added", "layer-merged", "layer-hidden",
                       "layer-background-renamed", "layer-renamed", "layer-duplicated",
                       "layer-moved-down", "layers-hidden-all", "layers-shown-all", "layer-saved"):
        path = output / f"{screenshot}.png"
        if not path.is_file() or path.stat().st_size < 1000:
            raise RuntimeError(f"Missing or empty layer management screenshot: {path}")
    saved = ET.fromstring(gzip.decompress((output / "layer-saved.xopp").read_bytes()))
    pages = saved.findall("page")
    layers = pages[0].findall("layer")
    if (len(pages) != 2 or [layer.attrib.get("name") for layer in layers] !=
            ["Lower", "Upper renamed", "Duplicate"]):
        raise RuntimeError("Layer UI actions did not save the renamed and duplicated per-page layer stack")
    if pages[0].find("background").attrib.get("name") != "Paper":
        raise RuntimeError("Layer UI save did not persist the explicit background name")
    if [layer.attrib.get("name") for layer in pages[1].findall("layer")] != ["Lower", "Upper"]:
        raise RuntimeError("Layer UI actions changed the other page's layer stack")
    lower_strokes = layers[0].findall("stroke")
    upper_strokes = layers[1].findall("stroke")
    copied_strokes = layers[2].findall("stroke")
    if (len(lower_strokes) != 2 or lower_strokes[0].attrib.get("style") != "dash" or
            lower_strokes[1].attrib.get("tool") != "pen"):
        raise RuntimeError("Layer UI save lost or reordered non-top layer content")
    if (len(upper_strokes) != 3 or upper_strokes[0].attrib.get("fill") != "128" or
            upper_strokes[1].attrib.get("tool") != "highlighter" or
            upper_strokes[2].attrib.get("tool") != "pen"):
        raise RuntimeError("Layer UI save lost merged native content or new active-layer ink")
    if (len(copied_strokes) != 3 or copied_strokes[0].attrib.get("fill") != "128" or
            copied_strokes[1].attrib.get("tool") != "highlighter" or
            copied_strokes[2].attrib.get("tool") != "pen"):
        raise RuntimeError("Layer duplication did not copy native and editable layer content")
    print("PASS: private GTK4 layer UI interaction and native save/reopen semantics", flush=True)
    return str(output)


def verify_preferences(args, evidence):
    directory = evidence / "preferences"
    shutil.rmtree(directory, ignore_errors=True)
    config = directory / "config"
    config.mkdir(parents=True, exist_ok=True)
    settings_path = config / "settings.xml"
    settings_path.write_text("""<?xml version="1.0" encoding="UTF-8"?>
<settings>
  <data name="buttonConfig">
    <data name="stylus"><attribute name="tool" type="string" value="highlighter"/></data>
    <data name="stylus2"><attribute name="tool" type="string" value="hand"/></data>
  </data>
</settings>
""", encoding="utf-8")
    settings_hash = hashlib.sha256(settings_path.read_bytes()).hexdigest()

    for mode in ("edit", "reload"):
        output = directory / mode
        output.mkdir(parents=True, exist_ok=True)
        result = run(
            [str(args.binary.resolve()), "--config-dir", str(config), "--smoke-dir", str(output)],
            env=dict(os.environ, G_DEBUG="fatal-criticals", XOURNALPP_PREFERENCES_VERIFY_MODE=mode),
        )
        expected = "PASS: Adw Preferences editing" if mode == "edit" else "PASS: GTK 4 preferences survive application restart"
        if expected not in result:
            raise RuntimeError(f"GTK 4 Preferences verification did not finish for {mode}")
        if hashlib.sha256(settings_path.read_bytes()).hexdigest() != settings_hash:
            raise RuntimeError("GTK 4 Preferences modified the original settings.xml")

    for name in ("preferences-dialog", "preferences-behavior", "preferences-edited", "preferences-reloaded"):
        path = directory / ("edit" if name != "preferences-reloaded" else "reload") / f"{name}.png"
        if not path.is_file() or path.stat().st_size < 1000:
            raise RuntimeError(f"Missing GTK 4 Preferences screenshot: {path}")
    stored = (config / "gtk4-canvas.ini").read_text(encoding="utf-8")
    for expected in ("theme=light", "pressure-sensitivity=false", "autosave-timeout-minutes=9",
                     "rotation-snap-tolerance=0.45", "snap-rotation=false", "button-one=1", "button-two=4"):
        if expected not in stored:
            raise RuntimeError(f"GTK 4 Preferences file is missing {expected}")
    print("PASS: private GTK4 Preferences interaction, persistence and settings.xml preservation", flush=True)
    return str(directory)


def make_png(path, width=32, height=24):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend((32 + x * 6, 80 + y * 5, 210 if (x + y) % 2 else 250, 255))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    payload = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    path.write_bytes(payload)


def verify_images(args, evidence):
    directory = evidence / "image-insertion"
    directory.mkdir(parents=True, exist_ok=True)
    fixture = directory / "blank.xopp"
    root = ET.Element("xournal", {"creator": "Xournal++", "fileversion": "4"})
    ET.SubElement(root, "title").text = "GTK4 image insertion"
    page = ET.SubElement(root, "page", {"width": "595", "height": "842"})
    ET.SubElement(page, "background", {"type": "solid", "color": "#ffffffff", "style": "plain"})
    ET.SubElement(page, "layer", {"name": "Layer 1"})
    fixture.write_bytes(gzip.compress(ET.tostring(root, encoding="utf-8", xml_declaration=True)))
    image = directory / "fixture.png"
    make_png(image)
    output = directory / "run"
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True, exist_ok=True)
    result = run(
        [str(args.binary.resolve()), "--document", str(fixture), "--worker", str(args.worker.resolve()),
         "--smoke-dir", str(output)],
        env=dict(os.environ, G_DEBUG="fatal-criticals", XOURNALPP_IMAGE_VERIFY_MODE="insert-resize-save-reopen",
                 XOURNALPP_IMAGE_VERIFY_SOURCE=str(image)),
    )
    if ("PASS: image insertion, resize, move, rotate, native save/reopen, PDF export, "
            "editable native image cut/paste, PNG paste and undo/redo" not in result):
        raise RuntimeError("GTK image insertion verification did not finish")
    for name in ("image-inserted", "image-resized", "image-rotated", "image-reopened", "image-png-pasted"):
        screenshot = output / f"{name}.png"
        if not screenshot.is_file() or screenshot.stat().st_size < 1000:
            raise RuntimeError(f"Missing or empty image insertion screenshot: {screenshot}")
    saved = ET.fromstring(gzip.decompress((output / "image-saved.xopp").read_bytes()))
    images = saved.findall(".//image")
    if len(images) != 1:
        raise RuntimeError(f"Native image save contains {len(images)} images instead of one")
    stored = saved.find(".//image")
    if stored is None or not stored.text or len(stored.text.strip()) < 16:
        raise RuntimeError("Native image save lost its encoded image data")
    matrix = list(map(float, stored.attrib.get("matrix", "").split()))
    if len(matrix) != 6 or abs(matrix[1]) < 0.1 or abs(matrix[2]) < 0.1:
        raise RuntimeError("Native image save lost its rotated affine transform")
    exported = output / "image-exported.pdf"
    if not exported.is_file() or not exported.read_bytes().startswith(b"%PDF-"):
        raise RuntimeError("Image PDF export is missing or invalid")
    image_list = run(["pdfimages", "-list", str(exported)])
    if not any(re.match(r"^\s*\d+\s+\d+\s+image\s", line) for line in image_list.splitlines()):
        raise RuntimeError("Exported PDF does not contain the inserted image")
    print("PASS: private GTK4 image insertion, resize, move, rotate, native persistence and PDF export", flush=True)
    return str(output)


def verify_selection_modes(args, evidence):
    directory = evidence / "selection-modes"
    shutil.rmtree(directory, ignore_errors=True)
    output = directory / "run"
    output.mkdir(parents=True, exist_ok=True)
    if not args.worker:
        raise RuntimeError("Visible-layer selection GUI checks require the document worker")
    fixture = directory / "selection-demo.xopp"
    root = ET.Element("xournal", {"creator": "Xournal++", "fileversion": "4"})
    ET.SubElement(root, "title").text = "Visible-layer selection"
    page = ET.SubElement(root, "page", {"width": "595", "height": "842"})
    ET.SubElement(page, "background", {"type": "solid", "color": "#ffffffff", "style": "plain"})
    lower = ET.SubElement(page, "layer", {"name": "Lower"})
    ET.SubElement(lower, "stroke", {"tool": "pen", "capStyle": "round", "color": "#303030ff",
                                      "width": "3"}).text = "60 120 190 120"
    ET.SubElement(lower, "stroke", {"tool": "pen", "capStyle": "round", "color": "#303030ff",
                                      "width": "3"}).text = "60 145 190 145"
    upper = ET.SubElement(page, "layer", {"name": "Upper"})
    ET.SubElement(upper, "stroke", {"tool": "pen", "capStyle": "round", "color": "#d44f25ff",
                                      "width": "4"}).text = "60 175 190 175"
    ET.SubElement(upper, "stroke", {"tool": "pen", "capStyle": "round", "color": "#d44f25ff",
                                      "width": "4"}).text = "60 200 190 200"
    ET.SubElement(upper, "text", {"font": "Sans", "size": "12", "x": "75", "y": "208",
                                    "color": "#202020ff"}).text = "Upper layer text selection"
    fixture.write_bytes(gzip.compress(ET.tostring(root, encoding="utf-8", xml_declaration=True)))
    fixture_hash = hashlib.sha256(fixture.read_bytes()).hexdigest()
    result = run(
        [str(args.binary.resolve()), "--document", str(fixture), "--worker", str(args.worker.resolve()),
         "--smoke-dir", str(output)],
        env=dict(os.environ, G_DEBUG="fatal-criticals", XOURNALPP_SELECTION_VERIFY_MODE="visible-layers"),
    )
    if ("PASS: GTK visible-layer rectangle/lasso selection, Select Object and Hand modes, selection-preserving pan, "
            "hidden-layer fallback, remembered tool state, and shortcuts"
            not in result):
        raise RuntimeError("GTK visible-layer selection mode verification did not finish")
    for name in ("selection-visible-layer-rectangle", "selection-visible-layer-lasso",
                 "selection-hidden-layer-fallback", "selection-text-object", "selection-text-moved"):
        screenshot = output / f"{name}.png"
        if not screenshot.is_file() or screenshot.stat().st_size < 1000:
            raise RuntimeError(f"Missing or empty visible-layer selection screenshot: {screenshot}")
    if hashlib.sha256(fixture.read_bytes()).hexdigest() != fixture_hash:
        raise RuntimeError("Visible-layer selection fixture changed during the GUI check")
    print("PASS: private GTK4 visible-layer selection menu, mode state and shortcut behavior", flush=True)
    return str(output)


def inside(args):
    evidence = args.evidence.resolve()
    if args.selection_only:
        selection_evidence = verify_selection_modes(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"selection-modes": selection_evidence},
             "fixture": str(evidence / "selection-modes" / "selection-demo.xopp"),
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.image_only:
        image_evidence = verify_images(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"image-insertion": image_evidence}, "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.layer_only:
        layer_evidence = verify_layer_management(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"layer-management": layer_evidence}, "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
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
        screenshots = ("initial", "ink", "tools", "eraser-split", "eraser-whiteout", "zoom", "drawing-controls",
                       "shape-tools", "shape-recognized", "spline-tools", "narrow", "sidebar")
        if args.worker:
            screenshots += ("text-editor", "text-edited", "document-dirty", "document-saved", "unsaved", "open-error", "reopened",
                            "save-prompt", "saved-again", "discard-prompt", "normal-saved", "save-conflict",
                            "conflict-resolved", "autosaved", "autosave-prompt", "autosave-recovered",
                            "autosave-saved", "native-loaded", "native-erased", "native-saved",
                            "continuous-pages", "goto-page-dialog", "page-inserted", "selection",
                            "selection-moved", "selection-deleted", "lasso-selection", "selection-resized",
                            "selection-rotated", "selection-pasted", "selection-transferred", "smart-selection")
            for filename in ("saved-copy.xopp", "saved-again.xopp", "normal-save.xopp",
                             "autosave-note.xopp", ".autosave-note.autosave.xopp",
                             "discard-autosave.xopp", ".discard-autosave.autosave.xopp",
                             "recover-autosave.xopp", ".recover-autosave.autosave.xopp",
                             "eraser-modes.xopp", "native-edited.xopp",
                             "exported-notes.pdf", "multipage-edited.xopp", "after-delete-page.xopp",
                             "selection-moved.xopp", "selection-deleted.xopp", "selection-transforms.xopp"):
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
            if "PASS: rectangle selection" not in results[name]:
                raise RuntimeError("Selection checks did not finish")
            first = ET.fromstring(gzip.decompress((output / "saved-copy.xopp").read_bytes()))
            second = ET.fromstring(gzip.decompress((output / "saved-again.xopp").read_bytes()))
            if len(first.findall(".//stroke")) != 4 or len(second.findall(".//stroke")) != 4:
                raise RuntimeError("Saved/reopened copies duplicated or lost session ink")
            normal = ET.fromstring(gzip.decompress((output / "normal-save.xopp").read_bytes()))
            if len(normal.findall(".//stroke")) != 5 or (output / "normal-save.xopp~").exists():
                raise RuntimeError("Normal Save lost or duplicated ink, or retained a successful-save backup")
            if len(first.findall(".//text")) != len(root.findall(".//text")) + 1:
                raise RuntimeError("Saving session ink lost native text or the new rich text item")
            width = list(map(float, second.findall(".//stroke")[-1].attrib["width"].split()))
            if len(width) < 2 or abs(width[1] - width[0] * 0.25) > 0.00001:
                raise RuntimeError("Native save lost stylus pressure mode or width")
            recovered = ET.fromstring(gzip.decompress((output / "recover-autosave.xopp").read_bytes()))
            if len(recovered.findall(".//stroke")) != 7 or (output / ".recover-autosave.autosave.xopp").exists():
                raise RuntimeError("Recovered autosave lost strokes or retained its sidecar after Save")
            eraser_saved = ET.fromstring(gzip.decompress((output / "eraser-modes.xopp").read_bytes()))
            eraser_strokes = eraser_saved.findall(".//stroke")
            if len(eraser_strokes) != 10:
                raise RuntimeError(f"Expected 10 strokes after saving split pen + whiteout, got {len(eraser_strokes)}")
            whiteout_stroke = eraser_strokes[-1]
            if (whiteout_stroke.attrib.get("tool") != "eraser" or
                    whiteout_stroke.attrib.get("color") != "#ffffffff" or
                    abs(float(whiteout_stroke.attrib.get("width", "0")) - 12.0) > 1e-6):
                raise RuntimeError("Native save did not serialize whiteout stroke as opaque white tool='eraser'")
            if (len(eraser_strokes[-3].attrib.get("width", "").split()) < 2 or
                    len(eraser_strokes[-2].attrib.get("width", "").split()) < 2):
                raise RuntimeError("Split pressure-sensitive pen segments lost pressure data on native save")
            native_edited = ET.fromstring(gzip.decompress((output / "native-edited.xopp").read_bytes()))
            edited_strokes = native_edited.findall(".//stroke")
            if len(edited_strokes) != 9 or (output / "native-edited.xopp~").exists():
                raise RuntimeError(f"Expected 9 strokes after editing reopened native strokes, got {len(edited_strokes)}")
            if any(s.attrib.get("tool") == "eraser" for s in edited_strokes):
                raise RuntimeError("Deleted preloaded native whiteout stroke remained in saved .xopp")
            if len(native_edited.findall(".//text")) != len(root.findall(".//text")) + 1:
                raise RuntimeError("Editing native strokes lost existing or newly created document text")
            if not any(text.text == "GTK rich π!" and "styles" in text.attrib for text in first.findall(".//text")):
                raise RuntimeError("Saving the GtkTextView editor lost its UTF-8 text or rich style")
            if (len(edited_strokes[-2].attrib.get("width", "").split()) < 2 or
                    len(edited_strokes[-1].attrib.get("width", "").split()) < 2):
                raise RuntimeError("Splitting a preloaded native pressure stroke lost pressure widths on save")
            exported_pdf = output / "exported-notes.pdf"
            exported_bytes = exported_pdf.read_bytes()
            if not exported_bytes.startswith(b"%PDF-") or exported_pdf.stat().st_size < 1500:
                raise RuntimeError("Exported PDF is missing or invalid")
            multipage = ET.fromstring(gzip.decompress((output / "multipage-edited.xopp").read_bytes()))
            mp_pages = multipage.findall("page")
            if len(mp_pages) != 3 or len(mp_pages[1].findall(".//stroke")) != 2 or len(mp_pages[2].findall(".//stroke")) != 1:
                raise RuntimeError("3-page edited document did not preserve strokes on Pages 2 and 3")
            after_delete = ET.fromstring(gzip.decompress((output / "after-delete-page.xopp").read_bytes()))
            moved = ET.fromstring(gzip.decompress((output / "selection-moved.xopp").read_bytes()))
            deleted = ET.fromstring(gzip.decompress((output / "selection-deleted.xopp").read_bytes()))
            moved_strokes, deleted_strokes = moved.findall(".//stroke"), deleted.findall(".//stroke")
            if len(moved_strokes) != len(deleted_strokes) + 1:
                raise RuntimeError("Selection delete lost or duplicated native objects")
            selected_stroke = moved.find("page").findall(".//stroke")[-1]
            coords = list(map(float, selected_stroke.text.split()))
            if abs(coords[0] - 320) > 1e-5 or abs(coords[1] - 440) > 1e-5:
                raise RuntimeError("Selection move did not persist native page coordinates")
            widths = list(map(float, selected_stroke.attrib["width"].split()))
            if len(widths) < 2 or abs(widths[1] - widths[0] * .25) > 1e-5:
                raise RuntimeError("Selection move changed native pressure widths")
            if [ET.tostring(t) for t in moved.findall(".//text")] != [ET.tostring(t) for t in deleted.findall(".//text")]:
                raise RuntimeError("Selection operations changed original native text")
            transformed = ET.fromstring(gzip.decompress((output / "selection-transforms.xopp").read_bytes()))
            transformed_pages = transformed.findall("page")
            source_stroke = transformed_pages[0].findall(".//stroke")[-1]
            pasted_stroke = transformed_pages[1].findall(".//stroke")[-1]
            source_points = list(map(float, source_stroke.text.split()))
            pasted_points = list(map(float, pasted_stroke.text.split()))
            # The original 100x80 stroke becomes 120x150 after uniform resize
            # and a quarter turn. Cross-page paste preserves relative geometry.
            if abs(source_points[2]-source_points[0]+30) > 1e-5 or abs(source_points[3]-source_points[1]-150) > 1e-5:
                raise RuntimeError("Native resize/rotation geometry mismatch")
            for index in range(2,len(source_points)):
                axis=index%2
                if abs((source_points[index]-source_points[axis])-(pasted_points[index]-pasted_points[axis]))>1e-5:
                    raise RuntimeError("Clipboard/cross-page transfer changed relative geometry")
            if source_stroke.attrib["width"] != pasted_stroke.attrib["width"]:
                raise RuntimeError("Clipboard/cross-page transfer changed pressure widths")
            if [ET.tostring(t) for t in transformed.findall(".//text")] != [ET.tostring(t) for t in moved.findall(".//text")]:
                raise RuntimeError("Transforms changed native text")
            ad_pages = after_delete.findall("page")
            if len(ad_pages) != 2 or len(ad_pages[1].findall(".//stroke")) != 1:
                raise RuntimeError("Deleting Page 2 did not shift Page 3 and its stroke cleanly to Page 2")
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
    if args.worker:
        verify_crash_recovery(args, evidence, copy, root)
        verify_settings_import(args, evidence, copy)
        results["layer-management"] = verify_layer_management(args, evidence)
        results["image-insertion"] = verify_images(args, evidence)
    results["preferences"] = verify_preferences(args, evidence)
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
    parser.add_argument("--layer-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--image-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--selection-only", action="store_true", help=argparse.SUPPRESS)
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
        if args.layer_only:
            command.append("--layer-only")
        if args.image_only:
            command.append("--image-only")
        if args.selection_only:
            command.append("--selection-only")
        output = run(command, env=env, private_group=True)
        (args.evidence / "headless.log").write_text(output)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
