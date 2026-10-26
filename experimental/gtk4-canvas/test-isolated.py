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
import zipfile
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
    (config / "toolbar.ini").write_text("""[Floating Toolbox (experimental)]
toolbarFloat1=PEN,HIGHLIGHTER,LASER_POINTER,PDF_TOOL,UNDO
toolbarFloat2=ZOOM_SLIDER,DRAW_ELLIPSE,FORMAT_ALIGN_LEFT,FORMAT_ALIGN_CENTER,FORMAT_ALIGN_RIGHT,FORMAT_JUSTIFY
toolbarFloat3=TOOL_FILL,FINE,THICK,DRAW_RECTANGLE
toolbarFloat4=COLOR(10),COLOR(1),COLOR(8),COLOR(4),DRAW_ARROW
""", encoding="utf-8")
    settings_hash = hashlib.sha256(settings_path.read_bytes()).hexdigest()
    toolbar_hash = hashlib.sha256((config / "toolbar.ini").read_bytes()).hexdigest()

    for mode in ("edit", "reload"):
        output = directory / mode
        output.mkdir(parents=True, exist_ok=True)
        command = [str(args.binary.resolve()), "--config-dir", str(config), "--smoke-dir", str(output)]
        if os.environ.get("XOURNALPP_PROTOTYPE_GDB"):
            command = ["gdb", "--batch", "--return-child-result", "-ex", "run", "-ex", "bt", "--args",
                       *command]
        result = run(
            command,
            env=dict(os.environ, G_DEBUG="fatal-criticals", XOURNALPP_PREFERENCES_VERIFY_MODE=mode),
        )
        expected = "PASS: Adw Preferences editing" if mode == "edit" else "PASS: GTK 4 preferences survive application restart"
        if expected not in result:
            raise RuntimeError(f"GTK 4 Preferences verification did not finish for {mode}")
        if (hashlib.sha256(settings_path.read_bytes()).hexdigest() != settings_hash or
                hashlib.sha256((config / "toolbar.ini").read_bytes()).hexdigest() != toolbar_hash):
            raise RuntimeError("GTK 4 Preferences modified settings.xml or the imported toolbar.ini")

    for name in (
            "main-tool-rail-default", "preferences-dialog", "preferences-latex", "preferences-input",
            "preferences-behavior", "preferences-toolbar", "preferences-main-tool-rail",
            "preferences-edited", "preferences-reloaded"):
        path = directory / ("edit" if name != "preferences-reloaded" else "reload") / f"{name}.png"
        if not path.is_file() or path.stat().st_size < 1000:
            raise RuntimeError(f"Missing GTK 4 Preferences screenshot: {path}")
    stored = (config / "gtk4-canvas.ini").read_text(encoding="utf-8")
    for expected in ("theme=light", "pressure-sensitivity=false", "autosave-timeout-minutes=9",
                     "rotation-snap-tolerance=0.45", "snap-rotation=false", "snap-grid=false",
                     "snap-grid-size=24", "snap-grid-tolerance=0.4", "button-one=1", "button-two=5",
                     "[Pointer Highlight]", "enabled=true", "fill-color=2131899478", "radius=17",
                     "border-color=3232484847", "border-width=2",
                     "main-tool-rail=SMART_SELECT,TEXT,PEN,HAND", "row-1=SMART_SELECT,PEN,DRAW,PAGE_SPIN,LAYER",
                     "row-2=ZOOM_SLIDER,DRAW_DOUBLE_ARROW,DRAW_COORDINATE_SYSTEM,DRAW_SPLINE,SHAPE_RECOGNIZER,RULER",
                     "row-3=TOOL_FILL,FINE,DRAW_RECTANGLE,DRAW_ELLIPSE",
                     "row-4=DRAW_ARROW,FILL_OPACITY",
                     "[Tool Fill]", "pen-enabled=true", "pen-opacity=163", "highlighter-enabled=true",
                     "highlighter-opacity=102",
                     "[LaTeX]", "auto-check-dependencies=false", "use-external-editor=true",
                     "external-editor-auto-confirm=true", "temporary-file-extension=tex"):
        if expected not in stored:
            raise RuntimeError(f"GTK 4 Preferences file is missing {expected}")
    print("PASS: private GTK4 Preferences interaction, persistence and settings.xml preservation", flush=True)
    return str(directory)


def verify_plugins(args, evidence):
    directory = evidence / "plugins"
    shutil.rmtree(directory, ignore_errors=True)
    config = directory / "config"
    output = directory / "run"
    config.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    plugin_root = Path(__file__).resolve().parents[2] / "plugins"
    private_toolbar_root = directory / "private-plugins"
    private_toolbar = private_toolbar_root / "PrivateToolbar"
    private_toolbar.mkdir(parents=True, exist_ok=True)
    (private_toolbar / "plugin.ini").write_text(
        "[about]\ndescription=Private custom toolbar action\n"
        "[default]\nenabled=false\n[plugin]\nmainfile=main.lua\n",
        encoding="utf-8",
    )
    (private_toolbar / "main.lua").write_text(
        "function initUi()\n"
        "  app.registerPlaceholder('GTK4_TEST_STATUS', 'Private plugin status')\n"
        "  app.setPlaceholderValue('GTK4_TEST_STATUS', 'READY')\n"
        "  app.registerUi({callback='toggleGridSnapping', toolbarId='GTK4_TEST_ACTION', "
        "iconName='document-open'})\n"
        "end\n"
        "local invocations = 0\n"
        "function toggleGridSnapping()\n"
        "  local enabled = not app.getActionState('grid-snapping')\n"
        "  app.changeActionState('grid-snapping', enabled)\n"
        "  invocations = invocations + 1\n"
        "  app.setPlaceholderValue('GTK4_TEST_STATUS', invocations == 1 and 'CLICKED' or '')\n"
        "end\n",
        encoding="utf-8",
    )
    color_cycle = plugin_root / "ColorCycle"
    example = plugin_root / "Example"
    tool_shortcuts = plugin_root / "ToolShortcuts"
    toggle_grid = plugin_root / "ToggleGrid"
    highlight_position = plugin_root / "HighlightPosition"
    space_for_notes = plugin_root / "SpaceForNotes"
    image_actions = plugin_root / "ImageActions"
    fit_to_content = plugin_root / "FitToContent"
    export_plugin = plugin_root / "Export"
    quick_screenshot = plugin_root / "QuickScreenshot"
    fake_bin = directory / "fake-bin"
    fake_bin.mkdir(parents=True, exist_ok=True)
    screenshot_input = directory / "captured-region.png"
    pixel = b"\x00\x32\x78\xc8\xff"

    def png_chunk(kind, payload):
        crc = zlib.crc32(kind + payload) & 0xffffffff
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", crc)

    png_data = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(pixel))
        + png_chunk(b"IEND", b"")
    )
    screenshot_input.write_bytes(png_data)
    scrot_stub = fake_bin / "scrot"
    scrot_stub.write_text(
        "#!/usr/bin/python3\nimport shutil, sys\n"
        f"shutil.copyfile({str(screenshot_input)!r}, sys.argv[-1])\n",
        encoding="utf-8")
    scrot_stub.chmod(0o755)
    maim_stub = fake_bin / "maim"
    maim_stub.write_text(
        "#!/usr/bin/python3\nimport shutil, sys\n"
        f"shutil.copyfile({str(screenshot_input)!r}, sys.argv[-1])\n",
        encoding="utf-8")
    maim_stub.chmod(0o755)
    xclip_stub = fake_bin / "xclip"
    xclip_stub.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    xclip_stub.chmod(0o755)
    grim_stub = fake_bin / "grim"
    grim_stub.write_text(
        "#!/usr/bin/python3\nimport shutil, sys\n"
        f"shutil.copyfile({str(screenshot_input)!r}, sys.argv[-1])\n",
        encoding="utf-8")
    grim_stub.chmod(0o755)
    slurp_stub = fake_bin / "slurp"
    slurp_stub.write_text("#!/bin/sh\nprintf '0,0 1x1\\n'\n", encoding="utf-8")
    slurp_stub.chmod(0o755)
    make_image_actions_png(output / "image-actions-input.png")
    image_actions_test = args.binary.resolve().with_name("canvas-image-actions-test")
    if not image_actions_test.is_file():
        raise RuntimeError(f"Missing GTK 4 canvas image-action test binary: {image_actions_test}")
    image_actions_result = run([str(image_actions_test)], env=dict(os.environ, G_DEBUG="fatal-criticals"))
    image_actions_available = "PASS: GTK4 canvas image actions (libvips)" in image_actions_result
    enabled_plugins = (color_cycle, example, tool_shortcuts, toggle_grid, highlight_position, space_for_notes,
                       image_actions, fit_to_content, export_plugin, quick_screenshot, private_toolbar)
    plugin_config = (
        "[Plugins]\n"
        f"enabled={','.join(str(plugin.resolve()) for plugin in enabled_plugins)}\n"
        "disabled=\n"
        "[Floating Toolbox]\n"
        "row-1=Plugin::GTK4_TEST_ACTION,Plugin::GTK4_TEST_STATUS\n"
        "row-2=FORMAT_BOLD,FORMAT_ITALIC,FORMAT_UNDERLINE,FORMAT_STRIKETHROUGH,"
        "FORMAT_SUPERSCRIPT,FORMAT_SUBSCRIPT,FORMAT_ALIGN_LEFT,FORMAT_ALIGN_CENTER,"
        "FORMAT_ALIGN_RIGHT,FORMAT_JUSTIFY\n"
    )
    (config / "gtk4-canvas.ini").write_text(
        plugin_config, encoding="utf-8")
    result = run(
        [str(args.binary.resolve()), "--config-dir", str(config), "--smoke-dir", str(output),
         "--pdf",
         str(Path(__file__).resolve().parents[2] / "test/files/cjk/测试.pdf"), "--plugin-dir",
         str(private_toolbar_root), "--verify-plugins"],
        env=dict(os.environ, G_DEBUG="fatal-criticals",
                 PATH=str(fake_bin) + os.pathsep + os.environ.get("PATH", "")),
    )
    expected = (
        "PASS: private GTK4 ColorCycle, Example dialog, ToolShortcuts, Smart Select shortcut, "
        "HighlightPosition, QuickScreenshot region capture/clipboard, Export PDF/SVG/PNG, ImageActions, "
        "FitToContent page resizing, SpaceForNotes page insertion, "
        "ToggleGrid paper changes, and background undo/redo"
    )
    if expected not in result:
        raise RuntimeError("GTK 4 bundled plugin integration did not complete")
    if "PASS: GTK4 document Search across PDF and editable text" not in result:
        raise RuntimeError("GTK 4 document Search plugin action did not complete")
    if "PASS: GTK4 plugin toolbar action and placeholder registration, rendering, and updates" not in result:
        raise RuntimeError("GTK 4 private plugin toolbar action or placeholder did not render or update")
    screenshots = ["plugins-menu", "plugins-manager", "search-native-text", "position-highlight-on", "space-for-notes-pdf-gaps",
                   "fit-to-content-page", "quick-screenshot.png"]
    if image_actions_available:
        screenshots.append("image-actions-split")
    for screenshot in screenshots:
        path = output / screenshot if screenshot.endswith(".png") else output / f"{screenshot}.png"
        min_size = 1 if screenshot == "quick-screenshot.png" else 1000
        if not path.is_file() or path.stat().st_size < min_size:
            raise RuntimeError(f"Missing plugin integration screenshot: {path}")
    saved_settings = (config / "gtk4-canvas.ini").read_text(encoding="utf-8")
    for plugin in ("ColorCycle", "ToolShortcuts", "ToggleGrid", "HighlightPosition", "SpaceForNotes", "ImageActions",
                   "FitToContent", "Export", "QuickScreenshot"):
        plugin_path = (plugin_root / plugin).resolve()
        if str(plugin_path) not in saved_settings:
            raise RuntimeError(f"Plugin manager did not persist the enabled {plugin} path")
    if str(private_toolbar.resolve()) not in saved_settings:
        raise RuntimeError("Plugin manager did not preserve the enabled private toolbar plugin")
    if str(example.resolve()) in saved_settings:
        raise RuntimeError("Plugin manager did not persist the disabled default Example plugin state")
    if "[Pointer Highlight]" not in saved_settings or "enabled=false" not in saved_settings:
        raise RuntimeError("HighlightPosition did not persist its final disabled state")
    print("PASS: private GTK4 plugin manager, asynchronous Lua dialogs, and bundled callbacks", flush=True)
    return str(directory)


def verify_search(args, evidence):
    directory = evidence / "search"
    shutil.rmtree(directory, ignore_errors=True)
    config = directory / "config"
    output = directory / "run"
    config.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    tool_shortcuts = Path(__file__).resolve().parents[2] / "plugins/ToolShortcuts"
    (config / "gtk4-canvas.ini").write_text(
        "[Plugins]\n"
        f"enabled={tool_shortcuts.resolve()}\n"
        "disabled=\n",
        encoding="utf-8",
    )
    custom_pdf_search = args.search_pdf is not None
    pdf = args.search_pdf.resolve() if custom_pdf_search else \
        Path(__file__).resolve().parents[2] / "test/files/cjk/测试.pdf"
    command = [str(args.binary.resolve()), "--config-dir", str(config), "--smoke-dir", str(output),
               "--pdf", str(pdf), "--verify-search"]
    if custom_pdf_search:
        command.extend(["--verify-search-query", args.search_query,
                        "--verify-search-page", str(args.search_page)])
        if args.search_scan_all:
            command.append("--verify-search-scan-all")
    result = run(command, env=dict(os.environ, G_DEBUG="fatal-criticals"))
    if custom_pdf_search:
        marker = "PASS: GTK4 PDF Search overlay geometry matches Poppler page"
        screenshot = output / "search-pdf-geometry.png"
    else:
        marker = "PASS: GTK4 document Search across PDF and editable text"
        screenshot = output / "search-native-text.png"
    if marker not in result:
        raise RuntimeError("GTK 4 document Search integration did not complete")
    if args.search_scan_all and "SEARCH RESPONSIVENESS: pages=" not in result:
        raise RuntimeError("Long-document Search did not report its GTK event-loop responsiveness")
    if not screenshot.is_file() or screenshot.stat().st_size < 1000:
        raise RuntimeError(f"Missing document Search screenshot: {screenshot}")
    return {
        "fixture_pdf": str(pdf),
        "search_query": args.search_query if custom_pdf_search else None,
        "zero_based_page": args.search_page if custom_pdf_search else None,
        "search_scan_all_pages": args.search_scan_all,
        "screenshot": str(screenshot),
        "rendering": "Private headless Wayland via Cage",
    }


def verify_pen_line_style(args, evidence):
    directory = evidence / "pen-line-style"
    shutil.rmtree(directory, ignore_errors=True)
    config = directory / "config"
    output = directory / "run"
    config.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    result = run(
        [str(args.binary.resolve()), "--config-dir", str(config), "--smoke-dir", str(output),
         "--verify-pen-line-style"],
        env=dict(os.environ, G_DEBUG="fatal-criticals"),
    )
    if "PASS: GTK4 Pen named/custom line-style selector and plugin state" not in result:
        raise RuntimeError("GTK 4 Pen line-style selector integration did not complete")
    screenshot = output / "pen-line-style-dash.png"
    if not screenshot.is_file() or screenshot.stat().st_size < 1000:
        raise RuntimeError(f"Missing Pen line-style screenshot: {screenshot}")
    settings = config / "gtk4-canvas.ini"
    if not settings.is_file() or "line-style=plain" not in settings.read_text(encoding="utf-8"):
        raise RuntimeError("Pen line-style preference was not persisted after the selector reset")
    return {
        "screenshot": str(screenshot),
        "persisted_default": "plain",
        "rendering": "Private headless Wayland via Cage",
    }


def write_beamer_pdf(path):
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R /PageLabels 7 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << >> /Contents 5 0 R >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << >> /Contents 6 0 R >>",
        b"<< /Length 0 >>\nstream\n\nendstream",
        b"<< /Length 0 >>\nstream\n\nendstream",
        b"<< /Nums [0 << /P (animation) >> 1 << /P (animation) >>] >>",
    ]
    data = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for number, body in enumerate(objects, 1):
        offsets.append(len(data))
        data.extend(f"{number} 0 obj\n".encode("ascii"))
        data.extend(body)
        data.extend(b"\nendobj\n")
    xref = len(data)
    data.extend(f"xref\n0 {len(objects) + 1}\n".encode("ascii"))
    data.extend(b"0000000000 65535 f \n")
    for offset in offsets[1:]:
        data.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    data.extend(f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\n".encode("ascii"))
    data.extend(f"startxref\n{xref}\n%%EOF\n".encode("ascii"))
    path.write_bytes(data)


def verify_beamer(args, evidence):
    directory = evidence / "beamer"
    shutil.rmtree(directory, ignore_errors=True)
    config = directory / "config"
    output = directory / "run"
    config.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    plugin = Path(__file__).resolve().parents[2] / "plugins/BeamerPresentation"
    pdf = directory / "animation.pdf"
    write_beamer_pdf(pdf)
    (config / "gtk4-canvas.ini").write_text(
        f"[Plugins]\nenabled={plugin.resolve()}\ndisabled=\n", encoding="utf-8")
    if not args.worker:
        raise RuntimeError("Beamer animation checks require --worker")
    result = run(
        [str(args.binary.resolve()), "--config-dir", str(config), "--smoke-dir", str(output),
         "--worker", str(args.worker.resolve()), "--pdf", str(pdf), "--verify-beamer"],
        env=dict(os.environ, G_DEBUG="fatal-criticals"),
    )
    if "PASS: private GTK4 Beamer animation advance, reverse, cleanup, and native reopen" not in result:
        raise RuntimeError("GTK4 Beamer animation integration did not complete")
    for screenshot in ("beamer-animation-start", "beamer-animation-forward", "beamer-animation-cleaned"):
        path = output / f"{screenshot}.png"
        if not path.is_file() or path.stat().st_size < 1000:
            raise RuntimeError(f"Missing Beamer integration screenshot: {path}")
    return str(directory)


def verify_layer_actions(args, evidence):
    directory = evidence / "layer-actions"
    shutil.rmtree(directory, ignore_errors=True)
    config = directory / "config"
    output = directory / "run"
    config.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    plugin = Path(__file__).resolve().parents[2] / "plugins/LayerActions"
    pdf = directory / "layer-actions.pdf"
    write_beamer_pdf(pdf)
    (config / "gtk4-canvas.ini").write_text(
        f"[Plugins]\nenabled={plugin.resolve()}\ndisabled=\n", encoding="utf-8")
    if not args.worker:
        raise RuntimeError("LayerActions checks require --worker")
    result = run(
        [str(args.binary.resolve()), "--config-dir", str(config), "--smoke-dir", str(output),
         "--worker", str(args.worker.resolve()), "--pdf", str(pdf), "--verify-layer-actions"],
        env=dict(os.environ, G_DEBUG="fatal-criticals"),
    )
    expected = (
        "PASS: private GTK4 LayerActions annotated-page confirmation, clone, hide, add, undo/redo, and native reopen"
    )
    if expected not in result:
        raise RuntimeError("GTK4 LayerActions integration did not complete")
    for screenshot in ("layer-actions-cloned", "layer-actions-complete"):
        path = output / f"{screenshot}.png"
        if not path.is_file() or path.stat().st_size < 1000:
            raise RuntimeError(f"Missing LayerActions integration screenshot: {path}")
    return str(directory)


def verify_latex(args, evidence):
    directory = evidence / "latex"
    shutil.rmtree(directory, ignore_errors=True)
    config = directory / "config"
    output = directory / "run"
    config.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture.resolve()
    fixture_hash = hashlib.sha256(fixture.read_bytes()).hexdigest()
    if not args.worker:
        raise RuntimeError("LaTeX editor checks require --worker")
    result = run(
        [str(args.binary.resolve()), "--config-dir", str(config), "--smoke-dir", str(output),
         "--worker", str(args.worker.resolve()), "--document", str(fixture), "--verify-latex"],
        env=dict(os.environ, G_DEBUG="fatal-criticals"),
    )
    expected = "PASS: private GTK4 LaTeX compile, insert, edit, undo/redo and native round trip"
    if expected not in result:
        raise RuntimeError("GTK4 LaTeX editor integration did not complete")
    screenshot = output / "latex-formula-inserted.png"
    saved = output / "latex-round-trip.xopp"
    if not screenshot.is_file() or screenshot.stat().st_size < 1000:
        raise RuntimeError(f"Missing LaTeX insertion screenshot: {screenshot}")
    if not saved.is_file() or saved.stat().st_size < 1000:
        raise RuntimeError(f"Missing LaTeX native round-trip document: {saved}")
    if hashlib.sha256(fixture.read_bytes()).hexdigest() != fixture_hash:
        raise RuntimeError("LaTeX editor verification modified its source fixture")
    report = {"fixture": str(fixture), "fixture_sha256": fixture_hash,
              "saved_document": str(saved), "screenshot": str(screenshot),
              "rendering": "Private headless Wayland via Cage"}

    external_directory = directory / "external-editor"
    external_config = external_directory / "config"
    external_output = external_directory / "run"
    external_config.mkdir(parents=True, exist_ok=True)
    external_output.mkdir(parents=True, exist_ok=True)
    fake_editor = external_directory / "fake latex editor"
    marker = external_directory / "editor-invocations.txt"
    fake_editor.write_text(r"""#!/bin/sh
for source do
    :
done
[ -f "$source" ] || exit 2
case "$source" in
    *.tex) ;;
    *) exit 3 ;;
esac
printf '\\frac{1}{2}\n' > "$source"
printf '%s\n' "$source" >> "$XOURNALPP_LATEX_EDITOR_MARKER"
""", encoding="utf-8")
    fake_editor.chmod(0o755)
    (external_config / "gtk4-canvas.ini").write_text(
        "[LaTeX]\n"
        "use-external-editor=true\n"
        f"external-editor-command='{fake_editor}' --wait\n"
        "external-editor-auto-confirm=false\n"
        "temporary-file-extension=tex\n",
        encoding="utf-8",
    )
    external_result = run(
        [str(args.binary.resolve()), "--config-dir", str(external_config), "--smoke-dir", str(external_output),
         "--worker", str(args.worker.resolve()), "--document", str(fixture), "--verify-latex"],
        env=dict(os.environ, G_DEBUG="fatal-criticals", XOURNALPP_LATEX_EDITOR_MARKER=str(marker)),
    )
    if expected not in external_result:
        raise RuntimeError("GTK4 external-editor LaTeX integration did not complete")
    invocations = marker.read_text(encoding="utf-8").splitlines() if marker.is_file() else []
    if len(invocations) != 2 or any(Path(path).name != "edit.tex" for path in invocations):
        raise RuntimeError(f"External editor did not receive the expected temporary source twice: {invocations}")
    external_saved = external_output / "latex-round-trip.xopp"
    if not external_saved.is_file() or external_saved.stat().st_size < 1000:
        raise RuntimeError("External-editor LaTeX round-trip document is missing")
    report["external_editor"] = {
        "invocations": len(invocations),
        "temporary_extension": ".tex",
        "saved_document": str(external_saved),
    }
    print("PASS: private GTK4 LaTeX editor and vector XOPP round trip", flush=True)
    return report


def verify_page_layout(args, evidence):
    run_dir = evidence / "page-layout"
    config_dir = evidence / "config"
    run_dir.mkdir(parents=True, exist_ok=True)
    config_dir.mkdir(parents=True, exist_ok=True)
    output = run(
        [str(args.binary.resolve()), "--config-dir", str(config_dir), "--smoke-dir", str(run_dir),
         "--verify-page-layout"],
        env=dict(os.environ, G_DEBUG="fatal-criticals"),
    )
    marker = "PASS: GTK4 page layout actions, Preferences, grid geometry, navigation and persistence"
    if marker not in output:
        raise RuntimeError("Page layout verification did not finish")

    screenshots = [run_dir / "page-layout-grid.png", run_dir / "page-layout-preferences.png",
                   run_dir / "page-layout-menu.png"]
    for screenshot in screenshots:
        if not screenshot.is_file() or screenshot.stat().st_size < 1000:
            raise RuntimeError(f"Missing page layout screenshot: {screenshot}")

    preferences = config_dir / "gtk4-canvas.ini"
    saved_preferences = preferences.read_text() if preferences.is_file() else ""
    expected_layout = ("[Layout]", "columns=2", "fixed-rows=false", "vertical=true", "paired-pages=false",
                       "paired-page-offset=1", "right-to-left=false", "bottom-to-top=false")
    if not all(value in saved_preferences for value in expected_layout):
        raise RuntimeError("Page Layout Preferences did not persist the edited GTK 4 layout settings")
    report = {
        "screenshots": [str(path) for path in screenshots],
        "preferences": str(preferences),
        "checks": ["row and column geometry", "paired page offset", "reading directions", "page hit testing",
                   "Preferences controls", "menu action synchronization", "layout persistence"],
        "rendering": "Private headless Wayland via Cage",
    }
    print("PASS: private GTK4 page layout menu, page grid, hit testing and preferences", flush=True)
    return report


def verify_scroll_motion(args, evidence):
    run_dir = evidence / "scroll-motion"
    run_dir.mkdir(parents=True, exist_ok=True)
    output = run(
        [str(args.binary.resolve()), "--smoke-dir", str(run_dir), "--verify-scroll-motion"],
        env=dict(os.environ, G_DEBUG="fatal-criticals"),
    )
    marker = "PASS: GTK4 touchpad axis intent, page-grid bounds and momentum"
    if marker not in output:
        raise RuntimeError("GTK4 touchpad scroll motion verification did not finish")

    screenshots = [run_dir / f"{name}.png" for name in
                   ("scroll-start", "scroll-gesture", "scroll-momentum", "scroll-horizontal-grid", "scroll-boundary")]
    for screenshot in screenshots:
        if not screenshot.is_file() or screenshot.stat().st_size < 1000:
            raise RuntimeError(f"Missing touchpad scroll screenshot: {screenshot}")

    return {
        "screenshots": [str(path) for path in screenshots],
        "checks": ["vertical axis lock", "low-speed horizontal grid pan", "release momentum", "page-grid edge clamp"],
        "input_coverage": "Synthetic GTK controller signals; physical touchpad behavior remains unverified",
        "rendering": "Private headless Wayland via Cage",
    }


def verify_cursor_feedback(args, evidence):
    run_dir = evidence / "cursor-feedback"
    run_dir.mkdir(parents=True, exist_ok=True)
    output = run(
        [str(args.binary.resolve()), "--smoke-dir", str(run_dir), "--verify-cursor-feedback"],
        env=dict(os.environ, G_DEBUG="fatal-criticals"),
    )
    marker = "PASS: GTK4 contextual tool, text, selection, transform and pan cursors"
    if marker not in output:
        raise RuntimeError("GTK4 cursor feedback verification did not finish")

    return {
        "checks": ["text and eraser tools", "Smart Select text hover", "selection move and resize handles",
                   "rotation handle", "temporary hand and active pan"],
        "input_coverage": "Synthetic GTK pointer-motion signals; cursor objects read from the mapped canvas",
        "rendering": "Private headless Wayland via Cage",
    }


def verify_smart_select_performance(args, evidence):
    run_dir = evidence / "smart-select-performance"
    run_dir.mkdir(parents=True, exist_ok=True)
    command = [str(args.binary.resolve()), "--smoke-dir", str(run_dir), "--verify-smart-select-performance"]
    if args.smart_select_pdf:
        command.extend(["--pdf", str(args.smart_select_pdf.resolve())])
    output = run(
        command,
        env=dict(os.environ, G_DEBUG="fatal-criticals"),
    )
    marker = "PASS: GTK4 Smart Select drag raster-cache frame latency"
    if marker not in output:
        raise RuntimeError("GTK4 Smart Select performance verification did not finish")

    screenshots = [run_dir / "smart-select-performance-ink.png",
                   run_dir / "smart-select-performance-selected.png",
                   run_dir / "smart-select-performance-moved.png"]
    for screenshot in screenshots:
        if not screenshot.is_file() or screenshot.stat().st_size < 1000:
            raise RuntimeError(f"Missing Smart Select performance screenshot: {screenshot}")

    match = re.search(
        r"Smart Select drag raster-cache frame latency median_ms=([0-9.]+) p95_ms=([0-9.]+) max_ms=([0-9.]+)",
        output,
    )
    if not match:
        raise RuntimeError("Smart Select performance verification did not report frame latency")
    ink_match = re.search(
        r"live ink frame latency pen_median_ms=([0-9.]+) pen_p95_ms=([0-9.]+) pen_max_ms=([0-9.]+) "
        r"pen_cache_bytes_copied=(\d+) highlighter_median_ms=([0-9.]+) highlighter_p95_ms=([0-9.]+) "
        r"highlighter_max_ms=([0-9.]+) highlighter_cache_bytes_copied=(\d+)", output)
    if not ink_match:
        raise RuntimeError("Ink drawing verification did not report live frame latency")
    checks = ["40 pen strokes and 40 highlighter strokes", "128 points per stroke",
              "GTK Smart Select drag-controller path", "24 painted preview frames",
              "translated cache pixel parity in ink-renderer test"]
    if args.smart_select_pdf:
        checks.append("Pages sidebar visible with its initial thumbnail queue drained")
    return {
        "screenshots": [str(path) for path in screenshots],
        "document": str(args.smart_select_pdf.resolve()) if args.smart_select_pdf else "synthetic page",
        "ink_frame_latency_ms": {
            "pen": {"median": float(ink_match.group(1)), "p95": float(ink_match.group(2)),
                    "max": float(ink_match.group(3))},
            "highlighter": {"median": float(ink_match.group(5)), "p95": float(ink_match.group(6)),
                            "max": float(ink_match.group(7))},
        },
        "ink_cache_bytes_copied": {"pen": int(ink_match.group(4)), "highlighter": int(ink_match.group(8))},
        "frame_latency_ms": {"median": float(match.group(1)), "p95": float(match.group(2)),
                              "max": float(match.group(3))},
        "checks": checks,
        "input_coverage": "Synthetic GTK gesture-controller signals; no physical input claim",
        "rendering": "Private headless Wayland via Cage",
    }


def verify_presentation_mode(args, evidence):
    run_dir = evidence / "presentation-mode"
    config_dir = evidence / "presentation-config"
    run_dir.mkdir(parents=True, exist_ok=True)
    config_dir.mkdir(parents=True, exist_ok=True)
    output = run(
        [str(args.binary.resolve()), "--config-dir", str(config_dir), "--smoke-dir", str(run_dir),
         "--verify-presentation-mode"],
        env=dict(os.environ, G_DEBUG="fatal-criticals"),
    )
    marker = "PASS: GTK4 Presentation Mode per-page fit, navigation, zoom lock, fullscreen and view Preferences"
    if marker not in output:
        raise RuntimeError("Presentation Mode verification did not finish")

    screenshots = [run_dir / "presentation-mode.png", run_dir / "presentation-preferences.png",
                   run_dir / "presentation-mode-exit.png"]
    for screenshot in screenshots:
        if not screenshot.is_file() or screenshot.stat().st_size < 1000:
            raise RuntimeError(f"Missing Presentation Mode screenshot: {screenshot}")

    preferences = config_dir / "gtk4-canvas.ini"
    saved_preferences = preferences.read_text() if preferences.is_file() else ""
    expected_settings = ("presentation-mode=false", "[View Modes]", "presentation-go-fullscreen=true",
                         "presentation-show-menubar=false", "presentation-show-toolbar=true",
                         "presentation-show-sidebar=false")
    if not all(value in saved_preferences for value in expected_settings):
        raise RuntimeError("Presentation Mode or its view settings did not persist")

    report = {
        "screenshots": [str(path) for path in screenshots],
        "preferences": str(preferences),
        "checks": ["Presentation Mode action", "per-page fit and centering", "keyboard navigation",
                   "zoom lock", "fullscreen action", "view preset Preferences", "settings persistence"],
        "rendering": "Private headless Wayland via Cage",
    }
    print("PASS: private GTK4 Presentation Mode navigation, fit, fullscreen and view preferences", flush=True)
    return report


def write_page_stress_pdf(path, page_count):
    objects = [None] * (4 + page_count * 2)
    objects[1] = b"<< /Type /Catalog /Pages 2 0 R >>"
    page_ids = [4 + 2 * index for index in range(page_count)]
    objects[2] = (b"<< /Type /Pages /Kids [" + b" ".join(
        f"{page_id} 0 R".encode() for page_id in page_ids
    ) + f"] /Count {page_count} >>".encode())
    objects[3] = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"
    for index, page_id in enumerate(page_ids):
        content_id = page_id + 1
        text = f"BT /F1 14 Tf 72 700 Td (Stress page {index + 1}) Tj ET".encode()
        objects[page_id] = (
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            b"/Resources << /Font << /F1 3 0 R >> >> /Contents "
            + f"{content_id} 0 R >>".encode()
        )
        objects[content_id] = f"<< /Length {len(text)} >>\nstream\n".encode() + text + b"\nendstream"

    pdf = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for object_id in range(1, len(objects)):
        offsets.append(len(pdf))
        pdf.extend(f"{object_id} 0 obj\n".encode())
        pdf.extend(objects[object_id])
        pdf.extend(b"\nendobj\n")
    xref_offset = len(pdf)
    pdf.extend(f"xref\n0 {len(objects)}\n".encode())
    pdf.extend(b"0000000000 65535 f \n")
    for offset in offsets[1:]:
        pdf.extend(f"{offset:010d} 00000 n \n".encode())
    pdf.extend(
        f"trailer\n<< /Size {len(objects)} /Root 1 0 R >>\nstartxref\n{xref_offset}\n%%EOF\n".encode()
    )
    path.write_bytes(pdf)


def verify_sidebar_performance(args, evidence):
    run_dir = evidence / "sidebar-performance"
    config_dir = evidence / "sidebar-performance-config"
    run_dir.mkdir(parents=True, exist_ok=True)
    config_dir.mkdir(parents=True, exist_ok=True)
    page_count = 10000
    pdf = run_dir / f"{page_count}-pages.pdf"
    write_page_stress_pdf(pdf, page_count)
    output = run(
        [str(args.binary.resolve()), "--config-dir", str(config_dir), "--pdf", str(pdf),
         "--smoke-dir", str(run_dir), "--verify-sidebar-performance"],
        env=dict(os.environ, G_DEBUG="fatal-criticals"),
    )
    marker = "PASS: GTK4 pages sidebar realizes a bounded number of thumbnails for 10000 PDF pages"
    if marker not in output:
        raise RuntimeError("Large-PDF sidebar verification did not finish")

    screenshot = run_dir / "large-pdf-sidebar.png"
    if not screenshot.is_file() or screenshot.stat().st_size < 1000:
        raise RuntimeError(f"Missing large-PDF sidebar screenshot: {screenshot}")
    report = {
        "page_count": page_count,
        "screenshot": str(screenshot),
        "checks": ["virtualized page rows", "bounded mapped thumbnails",
                   "sidebar rendering without the main canvas PDF cache",
                   "nearest-visible-page first dispatch", "offscreen thumbnail cancellation",
                   "scroll to last page",
                   "selection navigates from last row", "smooth zoom reuses and refreshes raster",
                   "sidebar open screenshot"],
        "rendering": "Private headless Wayland via Cage",
    }
    print("PASS: private GTK4 large-PDF sidebar virtualization", flush=True)
    return report


def verify_sidebar_startup(args, evidence):
    pdf = args.sidebar_startup_pdf.resolve()
    if not pdf.is_file():
        raise RuntimeError(f"Sidebar startup PDF does not exist: {pdf}")
    run_dir = evidence / "sidebar-startup"
    config_dir = evidence / "sidebar-startup-config"
    run_dir.mkdir(parents=True, exist_ok=True)
    config_dir.mkdir(parents=True, exist_ok=True)
    (config_dir / "gtk4-canvas.ini").write_text("[General]\nshow-sidebar=true\n", encoding="utf-8")
    output = run(
        [str(args.binary.resolve()), "--config-dir", str(config_dir), "--pdf", str(pdf),
         "--smoke-dir", str(run_dir), "--verify-sidebar-startup"],
        env=dict(os.environ, G_DEBUG="fatal-criticals"),
    )
    marker = "PASS: GTK4 sidebar renders its first visible thumbnail without input"
    if marker not in output:
        raise RuntimeError("No-input sidebar startup verification did not finish")
    screenshot = run_dir / "sidebar-startup-no-input.png"
    if not screenshot.is_file() or screenshot.stat().st_size < 1000:
        raise RuntimeError(f"Missing no-input sidebar screenshot: {screenshot}")
    scroll_screenshot = run_dir / "sidebar-fast-scroll-stop.png"
    if not scroll_screenshot.is_file() or scroll_screenshot.stat().st_size < 1000:
        raise RuntimeError(f"Missing fast-scroll sidebar screenshot: {scroll_screenshot}")
    scroll_marker = "PASS: GTK4 sidebar renders visible thumbnails after fast scrolling stops"
    if scroll_marker not in output:
        raise RuntimeError("Visible thumbnails did not settle after fast sidebar scrolling")
    report = {
        "fixture_pdf": str(pdf),
        "screenshot": str(screenshot),
        "fast_scroll_screenshot": str(scroll_screenshot),
        "checks": ["first visible page thumbnail renders and redraws without input",
                   "visible thumbnails finish after fast scroll while the canvas page stays unchanged"],
        "rendering": "Private headless Wayland via Cage",
    }
    print("PASS: private GTK4 sidebar startup thumbnail repaint", flush=True)
    return report


def verify_text_clipboard(args, evidence):
    if not args.worker:
        raise RuntimeError("Text clipboard verification requires --worker")
    fixture = args.fixture.resolve()
    before = hashlib.sha256(fixture.read_bytes()).hexdigest()
    output_dir = evidence / "text-clipboard"
    output_dir.mkdir(parents=True, exist_ok=True)
    command = [str(args.binary.resolve()), "--document", str(fixture), "--worker",
               str(args.worker.resolve()), "--smoke-dir", str(output_dir), "--verify-text-clipboard"]
    output = run(command, env=dict(os.environ, G_DEBUG="fatal-criticals"))
    if "PASS: rich text/link and mixed clipboard copy/cut/paste/duplicate" not in output:
        raise RuntimeError("Text clipboard checks did not finish")
    if hashlib.sha256(fixture.read_bytes()).hexdigest() != before:
        raise RuntimeError("Text clipboard verification modified the native fixture")
    return {"fixture": str(fixture), "clipboard_targets": ["application/xournal", "image/png", "image/svg",
            "image/svg+xml", "text/plain;charset=utf-8"],
            "rendering": "Private headless Wayland via Cage"}


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


def make_image_actions_png(path):
    width, height = 40, 32

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            white = x == 0 or x == width - 1 or y == 0 or y == height - 1 or 18 <= x < 22 or 14 <= y < 18
            if white:
                rows.extend((255, 255, 255, 255))
            else:
                rows.extend((32 + x * 3, 40 + y * 4, 150 + (x + y) % 80, 255))
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
    if ("PASS: image insertion, resize, move, rotate, native save/reopen, PDF export, PNG "
            "paste, and multi-image native copy/cut/paste, duplicate and grouped undo/redo" not in result):
        raise RuntimeError("GTK image insertion verification did not finish")
    for name in ("image-inserted", "image-resized", "image-rotated", "image-reopened", "image-png-pasted",
                 "image-multi-pasted"):
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
    print("PASS: private GTK4 image editing, native and PNG clipboard, multi-image actions and PDF export", flush=True)
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
    if ("PASS: GTK visible-layer rectangle/lasso selection, Smart Select edge hit, grid snap, "
            "resize snap, rotation-aware resize and edge-pan, "
            "Text-tool mouse/stylus move/resize "
            "and formatting, vertical-space direction/preview/undo, "
            "Select Object and Hand modes, selection-preserving pan, hidden-layer fallback, "
            "remembered tool state, and shortcuts"
            not in result):
        raise RuntimeError("GTK visible-layer selection mode verification did not finish")
    for name in ("selection-visible-layer-rectangle", "selection-visible-layer-lasso",
                 "selection-hidden-layer-fallback", "smart-select-edge-hit", "smart-select-grid-snapped",
                 "smart-select-edge-pan",
                 "smart-select-rotated-frame", "smart-select-rotated-resize",
                 "selection-text-object", "selection-text-moved",
                 "text-tool-edited", "text-tool-moved", "text-tool-resized", "text-tool-stylus-moved",
                 "vertical-space-preview", "vertical-space-applied"):
        screenshot = output / f"{name}.png"
        if not screenshot.is_file() or screenshot.stat().st_size < 1000:
            raise RuntimeError(f"Missing or empty visible-layer selection screenshot: {screenshot}")
    if hashlib.sha256(fixture.read_bytes()).hexdigest() != fixture_hash:
        raise RuntimeError("Visible-layer selection fixture changed during the GUI check")
    print("PASS: private GTK4 visible-layer selection menu, mode state and shortcut behavior", flush=True)
    return str(output)


def verify_pdf_text_selection(args, evidence):
    directory = evidence / "pdf-text-selection"
    fixture_dir = directory / "fixture"
    output = directory / "run"
    shutil.rmtree(directory, ignore_errors=True)
    fixture_dir.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)

    repo_root = Path(__file__).resolve().parents[2]
    source_xopp = repo_root / "test" / "files" / "cjk" / "测试.xopp"
    source_pdf = repo_root / "test" / "files" / "cjk" / "测试.pdf"
    fixture = fixture_dir / source_xopp.name
    pdf = fixture_dir / source_pdf.name
    shutil.copyfile(source_xopp, fixture)
    shutil.copyfile(source_pdf, pdf)
    fixture_hash = hashlib.sha256(fixture.read_bytes()).hexdigest()
    pdf_hash = hashlib.sha256(pdf.read_bytes()).hexdigest()

    result = run(
        [str(args.binary.resolve()), "--document", str(fixture), "--smoke-dir", str(output)],
        env=dict(os.environ, G_DEBUG="fatal-criticals", XOURNALPP_PDF_TEXT_SELECTION_VERIFY_MODE="1"),
    )
    expected = (
        "PASS: GTK Smart Select PDF delegation, threshold and modifier routing, linear/rectangle/word/line "
        "selection, toolbar placement and dismissal/reopen, clipboard, marker presets and opacity, annotations, "
        "undo/redo and native save"
    )
    if expected not in result:
        raise RuntimeError("GTK PDF text selection verification did not finish")

    for name in (
        "smart-select-pdf-text", "smart-select-shift-rectangle",
        "pdf-text-linear-selection", "pdf-text-toolbar-reopened", "pdf-text-rectangle-selection",
        "pdf-text-word-selection",
        "pdf-text-line-selection", "pdf-text-highlight", "pdf-text-underline", "pdf-text-strikethrough",
        "pdf-text-highlight-presets", "pdf-text-highlight-recolored", "pdf-text-marker-opacity-control",
    ):
        screenshot = output / f"{name}.png"
        if not screenshot.is_file() or screenshot.stat().st_size < 1000:
            raise RuntimeError(f"Missing PDF text selection screenshot: {screenshot}")

    saved = output / "pdf-text-annotations.xopp"
    root = ET.fromstring(gzip.decompress(saved.read_bytes()))
    strokes = root.findall(".//stroke")
    markers = [stroke for stroke in strokes if stroke.attrib.get("tool") == "highlighter"]
    if not any(stroke.attrib.get("fill") == "60" and stroke.attrib.get("capStyle") == "butt"
               for stroke in markers):
        raise RuntimeError("Saved PDF highlights lost their opacity or butt caps")
    if not any(stroke.attrib.get("fill") == "230" and stroke.attrib.get("capStyle") == "butt"
               for stroke in markers):
        raise RuntimeError("Saved PDF underlines or strikethroughs lost their opacity or butt caps")
    if len(root.findall("page")) != 2 or len(root.findall(".//text")) != 3:
        raise RuntimeError("PDF annotation save lost native pages or foreground text")
    if hashlib.sha256(fixture.read_bytes()).hexdigest() != fixture_hash:
        raise RuntimeError("PDF text selection verification modified its XOPP fixture")
    if hashlib.sha256(pdf.read_bytes()).hexdigest() != pdf_hash:
        raise RuntimeError("PDF text selection verification modified its PDF background")

    print("PASS: private GTK4 PDF selection, actions and native PDF-backed XOPP save", flush=True)
    return str(output)


def verify_pdf_text_toolbar(args, evidence):
    directory = evidence / "pdf-text-toolbar"
    fixture_dir = directory / "fixture"
    output = directory / "run"
    shutil.rmtree(directory, ignore_errors=True)
    fixture_dir.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)

    source_pdf = Path(__file__).resolve().parents[2] / "test" / "files" / "cjk" / "测试.pdf"
    pdf = fixture_dir / source_pdf.name
    shutil.copyfile(source_pdf, pdf)
    pdf_hash = hashlib.sha256(pdf.read_bytes()).hexdigest()

    result = run(
        [str(args.binary.resolve()), "--pdf", str(pdf), "--smoke-dir", str(output)],
        env=dict(os.environ, G_DEBUG="fatal-criticals", XOURNALPP_PDF_TEXT_SELECTION_VERIFY_MODE="toolbar-only"),
    )
    expected = "PASS: GTK PDF text toolbar placement, input/zoom dismissal, and selection reopen"
    if expected not in result:
        raise RuntimeError("GTK PDF text toolbar verification did not finish")

    screenshots = [output / f"{name}.png" for name in
                   ("pdf-text-toolbar-placement", "pdf-text-rectangle-preview", "pdf-text-toolbar-reopened")]
    for screenshot in screenshots:
        if not screenshot.is_file() or screenshot.stat().st_size < 1000:
            raise RuntimeError(f"Missing PDF text toolbar screenshot: {screenshot}")
    if hashlib.sha256(pdf.read_bytes()).hexdigest() != pdf_hash:
        raise RuntimeError("PDF toolbar verification modified its input fixture")

    report = {"pdf": str(pdf), "pdf_sha256": pdf_hash,
              "screenshots": [str(path) for path in screenshots],
              "input_coverage": "Synthetic mouse controller signals; no physical hardware events",
              "rendering": "Private headless Wayland via Cage"}
    print("PASS: private GTK4 PDF toolbar placement, zoom dismissal and selection reopen", flush=True)
    return report


def verify_links(args, evidence):
    directory = evidence / "link-tool"
    output = directory / "run"
    shutil.rmtree(directory, ignore_errors=True)
    output.mkdir(parents=True, exist_ok=True)

    source_hash = hashlib.sha256(args.fixture.read_bytes()).hexdigest()
    root = ET.fromstring(gzip.decompress(args.fixture.read_bytes()))
    for old_link in root.findall(".//link"):
        parent = next((candidate for candidate in root.iter() if old_link in list(candidate)), None)
        if parent is not None:
            parent.remove(old_link)
    page = root.find("page")
    layer = page.findall("layer")[-1]
    link = ET.SubElement(layer, "link", {
        "align": "left", "font": "Sans", "size": "12", "matrix": "1 0 0 1 140 130",
        "color": "#ff00ffff", "url": "https://source.example.org/"})
    link.text = "Source link"
    fixture = directory / "link-demo.xopp"
    fixture.write_bytes(gzip.compress(ET.tostring(root, encoding="utf-8", xml_declaration=True)))

    result = run(
        [str(args.binary.resolve()), "--document", str(fixture), "--worker", str(args.worker.resolve()),
         "--smoke-dir", str(output)],
        env=dict(os.environ, G_DEBUG="fatal-criticals", XOURNALPP_LINK_VERIFY_MODE="1"),
    )
    if "PASS: GTK native links create, edit, activate, undo/redo and save" not in result:
        raise RuntimeError("GTK native Link tool verification did not finish")

    for name in ("link-edited", "link-created", "link-saved"):
        screenshot = output / f"{name}.png"
        if not screenshot.is_file() or screenshot.stat().st_size < 1000:
            raise RuntimeError(f"Missing Link tool screenshot: {screenshot}")

    saved = output / "links-saved.xopp"
    saved_root = ET.fromstring(gzip.decompress(saved.read_bytes()))
    links = saved_root.findall(".//link")
    edited = next((item for item in links if item.text == "Edited GTK4 link"), None)
    created = next((item for item in links if item.text == "Contact"), None)
    if edited is None or edited.attrib.get("url") != "https://migration.example/phase29":
        raise RuntimeError("Native save did not retain edits to the existing link text and URL")
    if created is None or created.attrib.get("url") != "mailto:migration@example.org":
        raise RuntimeError("Native save did not retain the newly created mail link")
    if hashlib.sha256(args.fixture.read_bytes()).hexdigest() != source_hash:
        raise RuntimeError("Link tool verification modified its source fixture")

    report = {"saved_document": str(saved), "native_link_count": len(links),
              "screenshots": [str(output / f"{name}.png") for name in
                              ("link-edited", "link-created", "link-saved")],
              "rendering": "Private headless Wayland via Cage"}
    print("PASS: private GTK4 Link tool create/edit/activate and native save", flush=True)
    return report


def verify_laser_pointers(args, evidence):
    binary = str(args.binary.resolve())
    worker = str(args.worker.resolve())
    output = evidence / "laser-pointer" / "run"
    shutil.rmtree(output.parent, ignore_errors=True)
    output.mkdir(parents=True, exist_ok=True)
    source_hash = hashlib.sha256(args.fixture.read_bytes()).hexdigest()

    result = run(
        [binary, "--document", str(args.fixture.resolve()), "--worker", worker,
         "--smoke-dir", str(output)],
        env=dict(os.environ, G_DEBUG="fatal-criticals", XOURNALPP_LASER_VERIFY_MODE="1"),
    )
    if "PASS: GTK laser pointer pen/highlighter trails fade without entering document history" not in result:
        raise RuntimeError("GTK laser pointer verification did not finish")

    screenshots = [output / f"{name}.png" for name in
                   ("laser-pen", "laser-highlighter", "laser-fading", "laser-faded")]
    for screenshot in screenshots:
        if not screenshot.is_file() or screenshot.stat().st_size < 1000:
            raise RuntimeError(f"Missing laser pointer screenshot: {screenshot}")

    source_root = ET.fromstring(gzip.decompress(args.fixture.read_bytes()))
    saved = output / "laser-pointer-saved.xopp"
    saved_root = ET.fromstring(gzip.decompress(saved.read_bytes()))
    if len(saved_root.findall(".//stroke")) != len(source_root.findall(".//stroke")):
        raise RuntimeError("Transient laser strokes were serialized into the native document")
    if hashlib.sha256(args.fixture.read_bytes()).hexdigest() != source_hash:
        raise RuntimeError("Laser pointer verification modified its source fixture")

    report = {"saved_document": str(saved), "native_stroke_count": len(saved_root.findall(".//stroke")),
              "screenshots": [str(path) for path in screenshots],
              "rendering": "Private headless Wayland via Cage"}
    print("PASS: private GTK4 laser pen/highlighter input, fade, and transient save behavior", flush=True)
    return report


def read_xopp_xml(path):
    data = path.read_bytes()
    if data[:2] == b"\x1f\x8b":
        return ET.fromstring(gzip.decompress(data))
    with zipfile.ZipFile(path) as archive:
        return ET.fromstring(archive.read("content.xml"))


def verify_play_object(args, evidence):
    repository = Path(__file__).resolve().parents[2]
    source = repository / "test/files/packaged_xopp/audioAttachment/new.xopp"
    output = evidence / "play-object"
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True, exist_ok=True)
    fixture = output / "audio-linked.xopp"
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()

    with zipfile.ZipFile(source) as original:
        content = ET.fromstring(original.read("content.xml"))
        layer = content.find("./page/layer")
        if layer is None:
            raise RuntimeError("The packaged audio fixture has no foreground layer")
        ET.SubElement(layer, "stroke", {"tool": "pen", "color": "#e02020ff", "width": "14"}).text = \
            "40 300 500 300"
        xml_bytes = ET.tostring(content, encoding="utf-8", xml_declaration=True)
        with zipfile.ZipFile(fixture, "w") as modified:
            for item in original.infolist():
                modified.writestr(item, xml_bytes if item.filename == "content.xml" else original.read(item))

    fixture_hash = hashlib.sha256(fixture.read_bytes()).hexdigest()
    run_dir = output / "run"
    run_dir.mkdir(parents=True, exist_ok=True)
    result = run(
        [str(args.binary.resolve()), "--document", str(fixture), "--worker", str(args.worker.resolve()),
         "--smoke-dir", str(run_dir)],
        env=dict(os.environ, G_DEBUG="fatal-criticals", XOURNALPP_PLAY_OBJECT_VERIFY_MODE="1",
                 XOURNALPP_AUDIO_SINK="fakesink sync=true"),
    )
    if "PASS: GTK Play Object hit testing, playback controls, fading, and native save" not in result:
        raise RuntimeError("Play Object verification did not finish")

    screenshots = [run_dir / f"{name}.png" for name in
                   ("play-object-faded", "play-object-normal", "play-object-faded-again")]
    for screenshot in screenshots:
        if not screenshot.is_file() or screenshot.stat().st_size < 1000:
            raise RuntimeError(f"Missing Play Object screenshot: {screenshot}")

    source_root = read_xopp_xml(fixture)
    saved_root = read_xopp_xml(run_dir / "play-object-saved.xopp")
    source_audio = [(stroke.attrib.get("fn"), stroke.attrib.get("ts"))
                    for stroke in source_root.findall(".//stroke") if stroke.attrib.get("fn")]
    saved_audio = [(stroke.attrib.get("fn"), stroke.attrib.get("ts"))
                   for stroke in saved_root.findall(".//stroke") if stroke.attrib.get("fn")]
    if len(source_audio) != len(saved_audio) or [timestamp for _, timestamp in source_audio] != \
            [timestamp for _, timestamp in saved_audio]:
        raise RuntimeError("Native save lost an audio-linked stroke or its timestamp")
    if len(saved_root.findall(".//stroke")) != len(source_root.findall(".//stroke")):
        raise RuntimeError("Play Object selection changed the native stroke content")
    saved_audio_files = [filename for filename, _ in saved_audio if filename]
    if len(saved_audio_files) != len(saved_audio) or any(
            not Path(filename).is_file() or Path(filename).stat().st_size == 0 for filename in saved_audio_files):
        raise RuntimeError("Native save left an audio-linked object without its recording file")
    if hashlib.sha256(fixture.read_bytes()).hexdigest() != fixture_hash or \
            hashlib.sha256(source.read_bytes()).hexdigest() != source_hash:
        raise RuntimeError("Play Object verification modified its source fixture")

    report = {"saved_document": str(run_dir / "play-object-saved.xopp"),
              "audio_link_count": len(saved_audio), "screenshots": [str(path) for path in screenshots],
              "playback_sink": "GStreamer fakesink", "rendering": "Private headless Wayland via Cage"}
    print("PASS: private GTK4 Play Object hit testing, playback controls, fading and native preservation", flush=True)
    return report


def inside(args):
    evidence = args.evidence.resolve()
    if args.smart_select_performance_only:
        smart_select_evidence = verify_smart_select_performance(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"smart-select-performance": smart_select_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.scroll_only:
        scroll_evidence = verify_scroll_motion(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"touchpad-scroll": scroll_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.cursor_only:
        cursor_evidence = verify_cursor_feedback(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"cursor-feedback": cursor_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.sidebar_startup_only:
        startup_evidence = verify_sidebar_startup(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"sidebar-startup": startup_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.sidebar_performance_only:
        sidebar_evidence = verify_sidebar_performance(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"sidebar-performance": sidebar_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.presentation_only:
        presentation_evidence = verify_presentation_mode(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"presentation-mode": presentation_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.page_layout_only:
        layout_evidence = verify_page_layout(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"page-layout": layout_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.latex_only:
        latex_evidence = verify_latex(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"latex-editor": latex_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.clipboard_only:
        clipboard_evidence = verify_text_clipboard(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"text-clipboard": clipboard_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.layer_actions_only:
        layer_actions_evidence = verify_layer_actions(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"layer-actions": layer_actions_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.beamer_only:
        beamer_evidence = verify_beamer(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"beamer-presentation": beamer_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.plugins_only:
        plugin_evidence = verify_plugins(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"lua-plugins": plugin_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.search_only:
        search_evidence = verify_search(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"document-search": search_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.pen_line_style_only:
        line_style_evidence = verify_pen_line_style(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"pen-line-style": line_style_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.play_object_only:
        play_evidence = verify_play_object(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"play-object": play_evidence}, "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.laser_only:
        laser_evidence = verify_laser_pointers(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"laser-pointer-tools": laser_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.link_only:
        link_evidence = verify_links(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"link-tool": link_evidence}, "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.preferences_only:
        preferences_evidence = verify_preferences(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"preferences": preferences_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.pdf_text_selection_only:
        pdf_evidence = verify_pdf_text_selection(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"pdf-text-selection": pdf_evidence},
             "fixture": str(evidence / "pdf-text-selection" / "fixture" / "测试.xopp"),
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
    if args.pdf_toolbar_only:
        toolbar_evidence = verify_pdf_text_toolbar(args, evidence)
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"pdf-text-toolbar": toolbar_evidence},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
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
        screenshots = ("initial", "ink", "tools", "shift-strokes", "eraser-shift", "eraser-split", "eraser-whiteout", "zoom", "drawing-controls",
                       "shape-tools", "shape-recognized", "spline-tools", "narrow", "sidebar")
        if args.worker:
            screenshots += ("text-editor", "text-editor-initial", "text-editor-caret", "text-edited", "document-dirty", "document-saved", "unsaved", "open-error", "reopened",
                            "save-prompt", "saved-again", "discard-prompt", "normal-saved", "save-conflict",
                            "conflict-resolved", "autosaved", "autosave-prompt", "autosave-recovered",
                            "autosave-saved", "native-loaded", "native-erased", "native-saved",
                            "continuous-pages", "goto-page-dialog", "page-inserted", "selection",
                            "smart-text-selected", "selection-moved", "selection-deleted", "lasso-selection",
                            "selection-resized",
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
        command.extend(["--document", str(copy)])
        if args.worker:
            command.extend(["--worker", str(args.worker.resolve())])
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
    if args.canvas_smoke_only:
        (evidence / "results.json").write_text(json.dumps(
            {"checks": {"canvas-smoke": results},
             "rendering": "Private headless Wayland via Cage"},
            indent=2) + "\n")
        return
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
    parser.add_argument("--worker", type=Path, help="Optional legacy document-worker fallback")
    parser.add_argument("--fixture", type=Path,
                        default=Path(__file__).resolve().parents[2] / "test/files/benchmark/typed-text.xopp")
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--layer-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--image-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--selection-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--pdf-text-selection-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--pdf-toolbar-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--link-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--laser-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--play-object-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--preferences-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--plugins-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--search-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--search-pdf", type=Path, help="PDF path for a focused Search geometry check")
    parser.add_argument("--search-query", help="Query to search on --search-page")
    parser.add_argument("--search-page", type=int, default=0,
                        help="Zero-based PDF page for --search-query (default: 0)")
    parser.add_argument("--search-scan-all", action="store_true",
                        help="Wait for and measure a full page-by-page search scan")
    parser.add_argument("--scroll-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--cursor-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--smart-select-performance-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--smart-select-pdf", type=Path, help="PDF to use for the ink performance check")
    parser.add_argument("--canvas-smoke-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--pen-line-style-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--beamer-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--layer-actions-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--latex-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--clipboard-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--page-layout-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--presentation-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--sidebar-performance-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--sidebar-startup-only", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--sidebar-startup-pdf", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=True)
    if args.link_only and not args.worker:
        raise RuntimeError("Native Link tool checks require --worker")
    if args.laser_only and not args.worker:
        raise RuntimeError("Laser pointer document round-trip checks require --worker")
    if args.play_object_only and not args.worker:
        raise RuntimeError("Play Object verification requires --worker")
    if args.layer_actions_only and not args.worker:
        raise RuntimeError("LayerActions verification requires --worker")
    if args.latex_only and not args.worker:
        raise RuntimeError("LaTeX editor verification requires --worker")
    if args.clipboard_only and not args.worker:
        raise RuntimeError("Text clipboard verification requires --worker")
    if args.sidebar_startup_only and not args.sidebar_startup_pdf:
        raise RuntimeError("Sidebar startup verification requires --sidebar-startup-pdf")
    if args.smart_select_pdf and (not args.smart_select_performance_only or not args.smart_select_pdf.is_file()):
        raise RuntimeError("--smart-select-pdf requires --smart-select-performance-only and an existing PDF")
    if args.search_query and not args.search_pdf:
        raise RuntimeError("--search-query requires --search-pdf")
    if args.search_pdf and (not args.search_query or args.search_page < 0 or not args.search_pdf.is_file()):
        raise RuntimeError("Custom Search requires an existing PDF, a query and a nonnegative page")
    if args.search_pdf and not args.search_only:
        raise RuntimeError("--search-pdf can only be used with --search-only")
    if args.search_scan_all and not args.search_pdf:
        raise RuntimeError("--search-scan-all requires --search-pdf and --search-query")
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
        if args.pdf_text_selection_only:
            command.append("--pdf-text-selection-only")
        if args.pdf_toolbar_only:
            command.append("--pdf-toolbar-only")
        if args.link_only:
            command.append("--link-only")
        if args.laser_only:
            command.append("--laser-only")
        if args.play_object_only:
            command.append("--play-object-only")
        if args.preferences_only:
            command.append("--preferences-only")
        if args.plugins_only:
            command.append("--plugins-only")
        if args.search_only:
            command.append("--search-only")
        if args.search_pdf:
            command.extend(["--search-pdf", str(args.search_pdf.resolve())])
        if args.search_query:
            command.extend(["--search-query", args.search_query])
        if args.search_page != 0:
            command.extend(["--search-page", str(args.search_page)])
        if args.search_scan_all:
            command.append("--search-scan-all")
        if args.scroll_only:
            command.append("--scroll-only")
        if args.cursor_only:
            command.append("--cursor-only")
        if args.smart_select_performance_only:
            command.append("--smart-select-performance-only")
        if args.smart_select_pdf:
            command.extend(["--smart-select-pdf", str(args.smart_select_pdf.resolve())])
        if args.canvas_smoke_only:
            command.append("--canvas-smoke-only")
        if args.pen_line_style_only:
            command.append("--pen-line-style-only")
        if args.beamer_only:
            command.append("--beamer-only")
        if args.layer_actions_only:
            command.append("--layer-actions-only")
        if args.latex_only:
            command.append("--latex-only")
        if args.clipboard_only:
            command.append("--clipboard-only")
        if args.page_layout_only:
            command.append("--page-layout-only")
        if args.presentation_only:
            command.append("--presentation-only")
        if args.sidebar_performance_only:
            command.append("--sidebar-performance-only")
        if args.sidebar_startup_only:
            command.extend(["--sidebar-startup-only", "--sidebar-startup-pdf",
                            str(args.sidebar_startup_pdf.resolve())])
        output = run(command, env=env, private_group=True)
        (args.evidence / "headless.log").write_text(output)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
