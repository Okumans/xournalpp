#!/usr/bin/env python3
"""Check the real document bridge using disposable files, with no desktop session."""

import argparse
import copy
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ET
import zipfile


def xml(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            data = archive.read("content.xml")
    else:
        data = gzip.decompress(path.read_bytes())
    return ET.fromstring(data)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def content(element):
    # Formatting whitespace between XML elements is not document content.
    return (element.tag, sorted(element.attrib.items()),
            element.text if element.tag in ("text", "stroke", "image", "teximage") else
            (element.text or "").strip(),
            [content(child) for child in element])


def read_ink_transfer(path):
    lines = path.read_text().splitlines()
    magic, count = lines[0].split()
    assert magic in ("XOURNALPP_GTK4_INK_V3", "XOURNALPP_GTK4_INK_V4")
    has_appearance = magic == "XOURNALPP_GTK4_INK_V4"
    packets = []
    cursor = 1
    for _ in range(int(count)):
        values = lines[cursor].split()
        cursor += 1
        page, layer_id, origin, order, tool, pressure = map(int, values[:6])
        width = float(values[6])
        if has_appearance:
            fill, cap_style = map(int, values[7:9])
            color = tuple(map(float, values[9:12]))
            point_count = int(values[12])
        else:
            fill, cap_style = -1, 0
            color = tuple(map(float, values[7:10]))
            point_count = int(values[10])
        points = [tuple(map(float, lines[cursor + index].split())) for index in range(point_count)]
        cursor += point_count
        packets.append({"page": page, "layer_id": layer_id, "origin": origin, "order": order,
                        "tool": tool, "pressure": pressure, "width": width,
                        "fill": fill, "cap_style": cap_style, "color": color, "points": points})
    assert cursor == len(lines)
    return packets


def write_ink_v3(path, packets):
    lines = [f"XOURNALPP_GTK4_INK_V3 {len(packets)}"]
    for packet in packets:
        values = (packet["page"], packet["layer_id"], packet["origin"], packet["order"], packet["tool"],
                  packet["pressure"], packet["width"], *packet["color"], len(packet["points"]))
        lines.append(" ".join(map(str, values)))
        lines.extend(" ".join(map(str, point)) for point in packet["points"])
    path.write_text("\n".join(lines) + "\n")


def write_layers(path, initial_count, selected, background_visible, layers, background_name=None):
    parts = ["XOURNALPP_GTK4_LAYERS_V2 1",
             f"{initial_count} {selected} {int(background_visible)} {len(layers)}",
             f"{int(background_name is not None)} {len(background_name.encode('utf-8')) if background_name is not None else 0}"]
    if background_name is not None:
        parts.append(background_name)
    for name, visible, origins in layers:
        encoded = name.encode("utf-8")
        parts.append(f"{int(visible)} {len(encoded)} {len(origins)}")
        parts.extend(map(str, origins))
        parts.append(encoded.decode("utf-8"))
    path.write_bytes(("\n".join(parts) + "\n").encode("utf-8"))


def read_layers(path):
    data = path.read_bytes()
    offset = 0

    def read_line():
        nonlocal offset
        end = data.index(b"\n", offset)
        value = data[offset:end].decode("ascii")
        offset = end + 1
        return value

    magic, count = read_line().split()
    assert magic in ("XOURNALPP_GTK4_LAYERS_V1", "XOURNALPP_GTK4_LAYERS_V2")
    version2 = magic == "XOURNALPP_GTK4_LAYERS_V2"
    pages = []
    for _ in range(int(count)):
        initial_count, selected, background, layer_count = map(int, read_line().split())
        background_name = None
        if version2:
            has_background_name, name_bytes = map(int, read_line().split())
            assert has_background_name in (0, 1) and (has_background_name or name_bytes == 0)
            if has_background_name:
                background_name = data[offset:offset + name_bytes].decode("utf-8")
                offset += name_bytes
                assert data[offset:offset + 1] == b"\n"
                offset += 1
        layers = []
        for _ in range(layer_count):
            visible, name_bytes, origin_count = map(int, read_line().split())
            origins = [int(read_line()) for _ in range(origin_count)]
            name = data[offset:offset + name_bytes].decode("utf-8")
            offset += name_bytes
            assert data[offset:offset + 1] == b"\n"
            offset += 1
            layers.append({"name": name, "visible": bool(visible), "origins": origins})
        pages.append({"initial_count": initial_count, "selected": selected,
                      "background_visible": bool(background), "background_name": background_name,
                      "layers": layers})
    assert offset == len(data)
    return pages


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--clipboard-test", type=Path, help="selection-transforms-test executable for native codec interoperability")
    args = parser.parse_args()
    worker = str(args.worker.resolve())
    fixtures = args.fixtures.resolve()
    hashes = {p: digest(p) for p in fixtures.rglob("*") if p.is_file()}
    timings = []
    with tempfile.TemporaryDirectory(prefix="gtk4-document-test-") as directory:
        temporary = Path(directory)
        env = dict(os.environ)
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DBUS_SESSION_BUS_ADDRESS"):
            env.pop(key, None)
        for key in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "TMPDIR"):
            path = temporary / key
            path.mkdir(mode=0o700)
            env[key] = str(path)
        env.update(G_DEBUG="fatal-criticals", GSETTINGS_BACKEND="memory")

        def run(*arguments, succeeds=True):
            started = time.perf_counter()
            result = subprocess.run([worker, *map(str, arguments)], env=env, capture_output=True,
                                    text=True, timeout=60)
            timings.append({"operation": arguments[0], "input": Path(arguments[1]).name,
                            "expected_success": succeeds, "seconds": time.perf_counter() - started})
            if (result.returncode == 0) != succeeds:
                raise AssertionError(f"Unexpected worker result: {arguments}\n{result.stdout}{result.stderr}")
            return result.stdout

        empty = temporary / "empty.ink"
        empty.write_text("XOURNALPP_GTK4_INK_V1 0\n")
        blank = temporary / "blank.xopp"
        run("open", "", blank, temporary / "blank.pdf")
        assert json.loads(run("inspect", blank))["pages"] == [
            {"width": 595, "height": 842, "layers": 1, "strokes": 0, "texts": 0, "images": 0}]
        source = temporary / "source.xopp"
        root = xml(fixtures / "benchmark/typed-text.xopp")
        page = root.find("page")
        # Include Unicode, inline formatting and a second layer/page.
        rich = ET.SubElement(page.find("layer"), "text", {
            "font": "Sans", "size": "12", "x": "30", "y": "30", "color": "#000000ff",
            "styles": "0-5:U2FucyBCb2xk:16:4278190335:1;5-8:U2Fucw==:12:-:4"})
        rich.text = "Helloé!"
        ET.SubElement(page, "layer")
        root.append(copy.deepcopy(page))
        source.write_bytes(gzip.compress(ET.tostring(root)))
        snapshot, pdf = temporary / "snapshot.xopp", temporary / "preview.pdf"
        run("open", source, snapshot, pdf)
        assert pdf.read_bytes().startswith(b"%PDF-")
        assert xml(snapshot).find("page/layer/text[@styles]").text == rich.text
        assert xml(snapshot).find("page/layer/text[@styles]").attrib["styles"] == rich.attrib["styles"]
        original = json.loads(run("inspect", snapshot))
        assert len(original["pages"]) == 2 and original["pages"][0]["layers"] == 2

        ink = temporary / "strokes.ink"
        ink.write_text("XOURNALPP_GTK4_INK_V1 4\n"
                       "0 0 1 4 1 0 0 3\n20 20 .25\n40 40 .5\n60 20 .75\n"
                       "1 1 0 14 1 1 0 2\n20 70 1\n100 70 1\n"
                       "0 0 0 3 0 0 1 1\n70 90 1\n"
                       "0 2 1 8.5 0 0 0 2\n30 50 .2\n90 50 .8\n")
        output = temporary / "copy.xopp"
        run("save-copy", snapshot, output, ink)
        saved = xml(output)
        before = xml(snapshot)
        for index, saved_page in enumerate(saved.findall("page")):
            layers = saved_page.findall("layer")
            appended = list(layers[-1])
            assert len(appended) == (3 if index == 0 else 1)
            for stroke in appended:
                layers[-1].remove(stroke)
            assert content(saved_page) == content(before.findall("page")[index])
            if index == 0:
                assert appended[0].attrib["tool"] == "pen"
                assert list(map(float, appended[0].attrib["width"].split())) == [4, 1, 2]
                assert len(appended[1].text.split()) == 4  # A duplicated dot.
                assert appended[2].attrib["tool"] == "eraser"
                assert appended[2].attrib["color"] == "#ffffffff"
                assert float(appended[2].attrib["width"]) == 8.5
            else:
                assert appended[0].attrib["tool"] == "highlighter"
                assert float(appended[0].attrib["width"]) == 14
        inspected = json.loads(run("inspect", output))
        assert inspected["pages"][0]["texts"] == original["pages"][0]["texts"]
        assert inspected["pages"][0]["strokes"] == 3
        run("render", output, temporary / "saved.pdf")

        # Editable rich text joins strokes in the native top-layer order and
        # survives extraction, session save, and XML serialization unchanged.
        rich_native = temporary / "rich-native.xopp"
        rich_root = ET.Element("xournal", {"creator": "Xournal++", "fileversion": "4"})
        ET.SubElement(rich_root, "title").text = "Rich text extraction"
        rich_page = ET.SubElement(rich_root, "page", {"width": "595", "height": "842"})
        ET.SubElement(rich_page, "background", {"type": "solid", "color": "#ffffffff", "style": "plain"})
        rich_layer = ET.SubElement(rich_page, "layer", {"name": "Top"})
        ET.SubElement(rich_layer, "stroke", {"tool": "pen", "capStyle": "round", "color": "#112233ff",
                                               "width": "2"}).text = "10 20 50 20"
        rich_text = ET.SubElement(rich_layer, "text", {
            "font": "Sans", "size": "12", "x": "30", "y": "45", "color": "#000000ff",
            "styles": "0-4:U2FucyBCb2xk:16:4278190335:1;4-11:U2Fucw==:12:-:8"})
        rich_text.text = "Rich π text"
        ET.SubElement(rich_layer, "stroke", {"tool": "highlighter", "capStyle": "round", "color": "#ffff007f",
                                               "width": "8"}).text = "10 70 50 70"
        rich_native.write_bytes(gzip.compress(ET.tostring(rich_root, encoding="utf-8", xml_declaration=True)))
        rich_snap = temporary / "rich-snap.xopp"
        rich_pdf = temporary / "rich-snap.pdf"
        rich_ink = temporary / "rich.ink"
        rich_transfer = temporary / "rich.text"
        run("open", rich_native, rich_snap, rich_pdf, rich_ink, rich_transfer)
        assert rich_transfer.read_bytes().startswith(b"XOURNALPP_GTK4_TEXT_V3 1\n")
        assert "Rich π text".encode() in rich_transfer.read_bytes()
        assert rich_ink.read_text().splitlines()[0] == "XOURNALPP_GTK4_INK_V4 2"
        rich_saved = temporary / "rich-saved.xopp"
        run("save-copy", rich_snap, rich_saved, rich_ink, rich_transfer)
        assert content(xml(rich_saved).find("page")) == content(xml(rich_native).find("page"))
        rich_layer_items = xml(rich_saved).findall("page/layer")[-1]
        assert [item.tag for item in rich_layer_items] == ["stroke", "text", "stroke"]
        rich_text_roundtrip = rich_layer_items[1]
        assert rich_text_roundtrip.text == "Rich π text"
        assert rich_text_roundtrip.attrib["styles"] == rich_text.attrib["styles"]

        normal = temporary / "normal.xopp"
        normal_ink = temporary / "normal.ink"
        normal_ink.write_text("XOURNALPP_GTK4_INK_V1 1\n0 0 0 3 0 0 1 2\n30 40 1\n80 90 1\n")
        expected = run("version", normal).strip()
        version = run("save", snapshot, normal, normal_ink, expected).strip()
        assert version == run("version", normal).strip()
        assert len(xml(normal).findall(".//stroke")) == 1
        normal.chmod(0o600)
        version = run("save", snapshot, normal, normal_ink, version).strip()
        assert len(xml(normal).findall(".//stroke")) == 1, "Repeated normal Save duplicated ink"
        assert normal.stat().st_mode & 0o777 == 0o600
        backup = Path(str(normal) + "~")
        assert not backup.exists(), "Successful Save left its temporary backup"
        changed = xml(normal)
        ET.SubElement(changed.find("page/layer"), "text", {
            "font": "Sans", "size": "12", "x": "40", "y": "50", "color": "#000000ff"}).text = "External edit"
        normal.write_bytes(gzip.compress(ET.tostring(changed)))
        external_hash = digest(normal)
        run("save", snapshot, normal, normal_ink, version, succeeds=False)
        assert digest(normal) == external_hash and not backup.exists()
        fresh = run("version", normal).strip()
        backup.write_bytes(b"recoverable existing backup")
        run("save", snapshot, normal, normal_ink, fresh, succeeds=False)
        assert digest(normal) == external_hash and backup.read_bytes() == b"recoverable existing backup"
        backup.unlink()
        alias = temporary / "normal-link.xopp"
        alias.symlink_to(normal)
        run("save", snapshot, alias, normal_ink, fresh, succeeds=False)
        assert alias.is_symlink() and digest(normal) == external_hash

        # Autosave must only write recovery filenames, use .swap replacement, preserve primary + backup,
        # and report SAVE_AS when opened directly.
        run("autosave", snapshot, temporary / "not-autosave.xopp", normal_ink, succeeds=False)
        assert not (temporary / "not-autosave.xopp").exists()
        sidecar_autosave = temporary / ".normal.autosave.xopp"
        backup.write_bytes(b"preserved primary backup")
        run("autosave", snapshot, sidecar_autosave, normal_ink)
        assert sidecar_autosave.is_file() and not Path(str(sidecar_autosave) + ".swap").exists()
        assert digest(normal) == external_hash and backup.read_bytes() == b"preserved primary backup"
        assert len(xml(sidecar_autosave).findall(".//stroke")) == 1
        second_autosave_ink = temporary / "autosave2.ink"
        second_autosave_ink.write_text("XOURNALPP_GTK4_INK_V1 2\n"
                                       "0 0 1 4 1 0 0 2\n25 35 .3\n75 85 .8\n"
                                       "1 1 0 14 1 1 0 2\n30 60 1\n90 60 1\n")
        run("autosave", snapshot, sidecar_autosave, second_autosave_ink)
        assert len(xml(sidecar_autosave).findall(".//stroke")) == 2
        assert not Path(str(sidecar_autosave) + ".swap").exists()
        assert digest(normal) == external_hash and backup.read_bytes() == b"preserved primary backup"
        backup.unlink()
        autosave_link = temporary / ".linked.autosave.xopp"
        autosave_link.symlink_to(normal)
        run("autosave", snapshot, autosave_link, normal_ink, succeeds=False)
        assert autosave_link.is_symlink() and digest(normal) == external_hash
        autosave_link.unlink()
        recovered_snap = temporary / "recovered-snap.xopp"
        recovered_pdf = temporary / "recovered-snap.pdf"
        assert run("open", sidecar_autosave, recovered_snap, recovered_pdf) == "SAVE_AS\n"
        emergency_file = Path(env["XDG_CONFIG_HOME"]) / "xournalpp" / "emergencysave.xopp"
        run("autosave", snapshot, emergency_file, normal_ink)
        assert emergency_file.is_file()
        assert run("open", emergency_file, temporary / "emerg-snap.xopp", temporary / "emerg-snap.pdf") == "SAVE_AS\n"
        cache_autosave = Path(env["XDG_CACHE_HOME"]) / "xournalpp" / "autosaves" / "4242.autosave.xopp"
        run("autosave", snapshot, cache_autosave, second_autosave_ink)
        assert cache_autosave.is_file() and len(xml(cache_autosave).findall(".//stroke")) == 2

        saved_hash = digest(output)
        run("save-copy", snapshot, output, ink, succeeds=False)
        assert digest(output) == saved_hash
        link = temporary / "link.xopp"
        link.symlink_to(output)
        run("save-copy", snapshot, link, ink, succeeds=False)
        assert link.is_symlink() and digest(output) == saved_hash
        broken = temporary / "broken.xopp"
        broken.symlink_to(temporary / "missing")
        run("save-copy", snapshot, broken, ink, succeeds=False)
        assert broken.is_symlink()
        for index, data in enumerate(("invalid", "XOURNALPP_GTK4_INK_V1 0 trailing",
                                      "XOURNALPP_GTK4_INK_V1 1\n0 0 0 3 1 0 0 1000001\n",
                                      "XOURNALPP_GTK4_INK_V1 1\n99 0 0 3 1 0 0 1\n1 1 1\n",
                                      "XOURNALPP_GTK4_INK_V1 1\n0 3 0 3 1 0 0 1\n1 1 1\n",
                                      "XOURNALPP_GTK4_INK_V1 1\n0 0 0 3 1 0 0 1\nnan 1 1\n")):
            ink.write_text(data)
            rejected = temporary / f"rejected-{index}.xopp"
            run("save-copy", snapshot, rejected, ink, succeeds=False)
            assert not rejected.exists()
        # Native transformations can leave finite ink outside a page. It must
        # remain editable after save/reopen, rather than fail or be flattened.
        offpage_ink = temporary / "offpage.ink"
        offpage_ink.write_text("XOURNALPP_GTK4_INK_V1 1\n0 0 1 4 1 0 0 2\n-20 -30 .25\n900 950 .75\n")
        offpage = temporary / "offpage.xopp"
        run("save-copy", snapshot, offpage, offpage_ink)
        extracted_offpage = temporary / "offpage-extracted.ink"
        run("open", offpage, temporary / "offpage-base.xopp", temporary / "offpage.pdf", extracted_offpage)
        assert "-20 -30 0.25" in extracted_offpage.read_text() and "900 950" in extracted_offpage.read_text()
        if args.clipboard_test:
            encoded = temporary / "clipboard.bin"
            decoded = temporary / "clipboard-native.bin"
            subprocess.run([str(args.clipboard_test.resolve()), "--encode", str(encoded)], env=env, check=True, timeout=10)
            run("clipboard-roundtrip", encoded, decoded)
            subprocess.run([str(args.clipboard_test.resolve()), "--verify", str(decoded)], env=env, check=True, timeout=10)
            encoded.write_bytes(encoded.read_bytes()[:-1])
            run("clipboard-roundtrip", encoded, temporary / "rejected-clipboard.bin", succeeds=False)
            assert not (temporary / "rejected-clipboard.bin").exists()
        # Corrupt/recoverable input must not silently become a partial saved copy.
        corrupt = temporary / "corrupt.xopp"
        corrupt.write_bytes(gzip.compress(b'<xournal><page width="500" height="500">'))
        run("save-copy", corrupt, temporary / "recovered.xopp", empty, succeeds=False)
        assert not (temporary / "recovered.xopp").exists()

        # 5-argument open extracts the contiguous editable stroke suffix from each layer,
        # preserving z-order relative to underlying strokes/text, other layers, filled/dashed/non-round-cap
        # strokes, and audio-attached strokes, and round-trips losslessly on save.
        mixed = temporary / "mixed.xopp"
        mixed_root = ET.Element("xournal", {"creator": "Xournal++", "fileversion": "4"})
        ET.SubElement(mixed_root, "title").text = "Mixed z-order extraction check"
        mixed_page = ET.SubElement(mixed_root, "page", {"width": "595", "height": "842"})
        ET.SubElement(mixed_page, "background", {"type": "solid", "color": "#ffffffff", "style": "plain"})
        lower_layer = ET.SubElement(mixed_page, "layer", {"name": "Lower"})
        ET.SubElement(lower_layer, "stroke", {"tool": "pen", "capStyle": "round", "color": "#112233ff",
                                              "width": "2"}).text = "10 10 50 10"
        top_layer = ET.SubElement(mixed_page, "layer", {"name": "Top"})
        ET.SubElement(top_layer, "stroke", {"tool": "pen", "capStyle": "round", "color": "#445566ff",
                                            "width": "2"}).text = "10 20 50 20"
        ET.SubElement(top_layer, "text", {"font": "Sans", "size": "12", "x": "10", "y": "25",
                                          "color": "#000000ff"}).text = "Text above early stroke"
        ET.SubElement(top_layer, "stroke", {"tool": "pen", "capStyle": "round", "style": "dash",
                                            "color": "#000000ff", "width": "2"}).text = "10 40 50 40"
        ET.SubElement(top_layer, "stroke", {"tool": "pen", "capStyle": "round", "fill": "128",
                                            "color": "#008800ff", "width": "2"}).text = "10 50 50 50 30 70"
        ET.SubElement(top_layer, "stroke", {"tool": "pen", "capStyle": "square",
                                            "color": "#880000ff", "width": "2"}).text = "10 80 50 80"
        ET.SubElement(top_layer, "stroke", {"tool": "pen", "capStyle": "round",
                                            "color": "#ff0000ff", "width": "4 1 2"}).text = "20 100 40 120 60 100"
        ET.SubElement(top_layer, "stroke", {"tool": "highlighter", "capStyle": "round",
                                            "color": "#ffff007f", "width": "8.5"}).text = "20 140 120 140"
        ET.SubElement(top_layer, "stroke", {"tool": "eraser", "capStyle": "round",
                                            "color": "#ffffffff", "width": "12"}).text = "30 160 90 160"
        mixed.write_bytes(gzip.compress(ET.tostring(mixed_root, encoding="utf-8", xml_declaration=True)))
        mixed_snap = temporary / "mixed-snap.xopp"
        mixed_pdf = temporary / "mixed-snap.pdf"
        mixed_ink = temporary / "mixed.ink"
        existing_ink = temporary / "existing.ink"
        existing_ink.write_text("do not overwrite")
        run("open", mixed, temporary / "bad-snap.xopp", temporary / "bad-snap.pdf", existing_ink, succeeds=False)
        assert existing_ink.read_text() == "do not overwrite" and not (temporary / "bad-snap.xopp").exists()
        assert run("open", mixed, mixed_snap, mixed_pdf, mixed_ink) == "NATIVE_SAVE\n"
        mixed_lines = mixed_ink.read_text().splitlines()
        assert mixed_lines[0] == "XOURNALPP_GTK4_INK_V4 5"
        mixed_packets = read_ink_transfer(mixed_ink)
        assert [(packet["layer_id"], packet["cap_style"]) for packet in mixed_packets] == [
            (1, 0), (2, 2), (2, 0), (2, 0), (2, 0)]
        snap_info = json.loads(run("inspect", mixed_snap))["pages"][0]
        assert snap_info["layers"] == 2 and snap_info["strokes"] == 3 and snap_info["texts"] == 1
        mixed_roundtrip = temporary / "mixed-roundtrip.xopp"
        run("save-copy", mixed_snap, mixed_roundtrip, mixed_ink)
        rt_strokes = xml(mixed_roundtrip).findall("page/layer")[-1].findall("stroke")
        assert len(rt_strokes) == 7
        assert rt_strokes[-3].attrib["tool"] == "pen" and list(map(float, rt_strokes[-3].attrib["width"].split())) == [4, 1, 2]
        assert rt_strokes[-2].attrib["tool"] == "highlighter" and float(rt_strokes[-2].attrib["width"]) == 8.5
        assert rt_strokes[-1].attrib["tool"] == "eraser" and rt_strokes[-1].attrib["color"] == "#ffffffff"
        assert [content(p) for p in xml(mixed_roundtrip).findall("page")] == [
            content(p) for p in xml(mixed).findall("page")]

        # The layered protocol extracts editable suffixes from every foreground
        # layer, then reconstructs each source layer and a merged stack without
        # flattening native objects or duplicating transferred ink.
        layered = temporary / "layered.xopp"
        layered_root = ET.Element("xournal", {"creator": "Xournal++", "fileversion": "4"})
        ET.SubElement(layered_root, "title").text = "Layer-aware round trip"
        layered_page = ET.SubElement(layered_root, "page", {"width": "595", "height": "842"})
        ET.SubElement(layered_page, "background", {"type": "solid", "color": "#ffffffff", "style": "plain"})
        layered_lower = ET.SubElement(layered_page, "layer", {"name": "Lower"})
        ET.SubElement(layered_lower, "stroke", {"tool": "pen", "capStyle": "round", "style": "dash",
                                                  "color": "#222222ff", "width": "2"}).text = "10 10 50 10"
        ET.SubElement(layered_lower, "stroke", {"tool": "pen", "capStyle": "round", "color": "#112233ff",
                                                  "width": "2"}).text = "10 20 50 20"
        layered_upper = ET.SubElement(layered_page, "layer", {"name": "Upper"})
        ET.SubElement(layered_upper, "stroke", {"tool": "pen", "capStyle": "round", "fill": "128",
                                                  "color": "#008800ff", "width": "2"}).text = "10 30 50 30 30 50"
        ET.SubElement(layered_upper, "stroke", {"tool": "highlighter", "capStyle": "round",
                                                  "color": "#ffff007f", "width": "8"}).text = "10 60 50 60"
        layered.write_bytes(gzip.compress(ET.tostring(layered_root, encoding="utf-8", xml_declaration=True)))
        layered_snap = temporary / "layered-snapshot.xopp"
        layered_pdf = temporary / "layered-preview.pdf"
        layered_ink = temporary / "layered.ink"
        layered_text = temporary / "layered.text"
        layered_state = temporary / "layered.dat"
        run("open", layered, layered_snap, layered_pdf, layered_ink, layered_text, layered_state)
        layer_pages = read_layers(layered_state)
        assert len(layer_pages) == 1 and layer_pages[0]["initial_count"] == 2
        assert [(layer["name"], layer["origins"]) for layer in layer_pages[0]["layers"]] == [
            ("Lower", [0]), ("Upper", [1])]
        layered_packets = read_ink_transfer(layered_ink)
        assert [(packet["layer_id"], packet["origin"]) for packet in layered_packets] == [(1, 0), (2, 1)]
        assert json.loads(run("inspect", layered_snap))["pages"][0]["strokes"] == 2

        updated_packets = copy.deepcopy(layered_packets)
        updated_packets.extend([
            {"page": 0, "layer_id": 1, "origin": 0, "order": 2, "tool": 0, "pressure": 0,
             "width": 2.5, "color": (0.1, 0.4, 0.8), "points": [(15, 80, 1), (45, 80, 1)]},
            {"page": 0, "layer_id": 2, "origin": 1, "order": 2, "tool": 0, "pressure": 0,
             "width": 3.0, "color": (0.8, 0.2, 0.1), "points": [(15, 90, 1), (45, 90, 1)]},
        ])
        updated_ink = temporary / "layered-updated.ink"
        write_ink_v3(updated_ink, updated_packets)
        layered_saved = temporary / "layered-saved.xopp"
        run("save-copy", layered_snap, layered_saved, updated_ink, layered_text, layered_state)
        saved_page = xml(layered_saved).find("page")
        saved_layers = saved_page.findall("layer")
        assert [layer.attrib.get("name") for layer in saved_layers] == ["Lower", "Upper"]
        lower_items = saved_layers[0].findall("stroke")
        upper_items = saved_layers[1].findall("stroke")
        assert len(lower_items) == 3 and lower_items[0].attrib.get("style") == "dash"
        assert lower_items[1].attrib["tool"] == "pen" and float(lower_items[1].attrib["width"]) == 2
        assert lower_items[2].attrib["tool"] == "pen" and float(lower_items[2].attrib["width"]) == 2.5
        assert len(upper_items) == 3 and upper_items[0].attrib.get("fill") == "128"
        assert upper_items[1].attrib["tool"] == "highlighter" and float(upper_items[1].attrib["width"]) == 8
        assert upper_items[2].attrib["tool"] == "pen" and float(upper_items[2].attrib["width"]) == 3

        merged_state = temporary / "layered-merged.dat"
        write_layers(merged_state, 2, 1, True, [("Merged", True, [0, 1])])
        merged_packets = copy.deepcopy(layered_packets)
        for packet in merged_packets:
            packet["layer_id"] = 1
        merged_ink = temporary / "layered-merged.ink"
        write_ink_v3(merged_ink, merged_packets)
        merged_file = temporary / "layered-merged.xopp"
        run("save-copy", layered_snap, merged_file, merged_ink, layered_text, merged_state)
        merged_layers = xml(merged_file).findall("page/layer")
        assert len(merged_layers) == 1 and merged_layers[0].attrib.get("name") == "Merged"
        merged_items = merged_layers[0].findall("stroke")
        assert [item.attrib.get("style") or item.attrib.get("fill") or item.attrib["tool"]
                for item in merged_items] == ["dash", "pen", "128", "highlighter"]

        # Audio-linked strokes must remain in snapshot.xopp rather than losing fn/ts attributes.
        audio_snap = temporary / "audio-snap.xopp"
        audio_pdf = temporary / "audio-snap.pdf"
        audio_ink = temporary / "audio.ink"
        run("open", fixtures / "packaged_xopp/audioAttachment/new.xopp", audio_snap, audio_pdf, audio_ink)
        assert audio_ink.read_text().strip() == "XOURNALPP_GTK4_INK_V4 0"
        assert json.loads(run("inspect", audio_snap))["pages"][0]["strokes"] == 1

        # Real 13,064-stroke handwritten notebook extracts completely and round-trips without duplication.
        hw_snap = temporary / "hw-snap.xopp"
        hw_pdf = temporary / "hw-snap.pdf"
        hw_ink = temporary / "hw.ink"
        run("open", fixtures / "benchmark/handwritten-text.xopp", hw_snap, hw_pdf, hw_ink)
        assert hw_ink.read_text().splitlines()[0] == "XOURNALPP_GTK4_INK_V4 13064"
        assert json.loads(run("inspect", hw_snap))["pages"][0]["strokes"] == 0
        hw_roundtrip = temporary / "hw-roundtrip.xopp"
        run("save-copy", hw_snap, hw_roundtrip, hw_ink)
        assert json.loads(run("inspect", hw_roundtrip))["pages"][0]["strokes"] == 13064

        # Page mutations (insert-page / delete-page) and atomic export-pdf with live ink.
        page_ops_ink = temporary / "page-ops.ink"
        page_ops_ink.write_text("XOURNALPP_GTK4_INK_V1 3\n"
                                "0 0 1 3.5 0.1 0.2 0.8 2\n30 40 0.3\n120 40 0.9\n"
                                "1 1 0 8.5 1 1 0 2\n40 80 1\n180 80 1\n"
                                "2 2 1 12 0 0 0 2\n50 110 1\n140 110 1\n")
        ins_dir = temporary / "inserted-rev"
        ins_dir.mkdir()
        ins_snap = ins_dir / "snapshot.xopp"
        ins_pdf = ins_dir / "preview.pdf"
        run("insert-page", snapshot, ins_snap, ins_pdf, "99", succeeds=False)
        assert not ins_snap.exists()
        run("insert-page", snapshot, ins_snap, ins_pdf, "1")
        ins_pages = json.loads(run("inspect", ins_snap))["pages"]
        assert len(ins_pages) == 3 and ins_pages[2]["layers"] == 1 and ins_pages[2]["strokes"] == 0
        assert ins_pages[2]["width"] == ins_pages[1]["width"] and ins_pages[2]["height"] == ins_pages[1]["height"]

        exported_pdf = temporary / "exported.pdf"
        run("export-pdf", ins_snap, temporary / "not-a-pdf.xopp", page_ops_ink, succeeds=False)
        run("export-pdf", ins_snap, exported_pdf, page_ops_ink)
        assert exported_pdf.read_bytes().startswith(b"%PDF-") and exported_pdf.stat().st_size > 1000
        exported_pdf.chmod(0o600)
        run("export-pdf", ins_snap, exported_pdf, page_ops_ink)
        assert exported_pdf.stat().st_mode & 0o777 == 0o600
        pdf_link = temporary / "exported-link.pdf"
        pdf_link.symlink_to(exported_pdf)
        exported_before = digest(exported_pdf)
        run("export-pdf", ins_snap, pdf_link, empty, succeeds=False)
        assert pdf_link.is_symlink() and digest(exported_pdf) == exported_before

        del_dir = temporary / "deleted-rev"
        del_dir.mkdir()
        run("delete-page", ins_snap, del_dir / "snapshot.xopp", del_dir / "preview.pdf", "1")
        assert len(json.loads(run("inspect", del_dir / "snapshot.xopp"))["pages"]) == 2
        single_del = temporary / "single-del"
        single_del.mkdir()
        run("delete-page", blank, single_del / "snapshot.xopp", single_del / "preview.pdf", "0", succeeds=False)
        assert not (single_del / "snapshot.xopp").exists()

        # Inserting a note page into an attached-PDF document must insert a Lined paper page and keep bg.pdf intact.
        pdf_bg_open = temporary / "pdf-bg-open"
        pdf_bg_open.mkdir()
        run("open", fixtures / "packaged_xopp/pdfBackground/new.xopp",
            pdf_bg_open / "snapshot.xopp", pdf_bg_open / "preview.pdf")
        pdf_bg_ins = temporary / "pdf-bg-ins"
        pdf_bg_ins.mkdir()
        run("insert-page", pdf_bg_open / "snapshot.xopp", pdf_bg_ins / "snapshot.xopp",
            pdf_bg_ins / "preview.pdf", "0")
        assert (pdf_bg_ins / "snapshot.xopp.bg.pdf").is_file()
        bg_nodes = [p.find("background").attrib for p in xml(pdf_bg_ins / "snapshot.xopp").findall("page")]
        assert bg_nodes[0]["type"] == "pdf" and bg_nodes[1]["type"] == "solid" and bg_nodes[1]["style"] == "lined"

        # Exercise both legacy sidecars and packaged attachments across directories.
        for index, name in enumerate(("pdfBackground/old.xopp", "pdfBackground/new.xopp",
                                      "imgBackground/old.xopp", "imgBackground/new.xopp",
                                      "imgAttachment/new.xopp", "audioAttachment/new.xopp",
                                      "layer.xopp", "text.xopp")):
            folder = temporary / f"assets-{index}"
            folder.mkdir()
            input_path = fixtures / "packaged_xopp" / name
            snapshot_path = folder / "snapshot.xopp"
            rendered_path = folder / "preview.pdf"
            run("open", input_path, snapshot_path, rendered_path)
            assert json.loads(run("inspect", input_path)) == json.loads(run("inspect", snapshot_path))
            assert rendered_path.stat().st_size > 500
            if name.startswith("pdfBackground"):
                assert (folder / "snapshot.xopp.bg.pdf").is_file()
            if name.startswith("imgBackground"):
                assert list(folder.glob("snapshot.xopp.bg_*.png"))
            second = folder / "copy.xopp"
            run("save-copy", snapshot_path, second, empty)
            assert [content(p) for p in xml(snapshot_path).findall("page")] == [
                content(p) for p in xml(second).findall("page")]
            before_save = run("version", second).strip()
            after_save = run("save", snapshot_path, second, empty, before_save).strip()
            assert after_save == run("version", second).strip()
            assert [content(p) for p in xml(snapshot_path).findall("page")] == [
                content(p) for p in xml(second).findall("page")]
            assert not Path(str(second) + "~").exists()
            if name.startswith("pdfBackground"):
                autosave_with_pdf = folder / ".copy.autosave.xopp"
                run("autosave", snapshot_path, autosave_with_pdf, normal_ink)
                assert (folder / ".copy.autosave.xopp.bg.pdf").is_file()
                pdf_sidecar = folder / "copy.xopp.bg.pdf"
                saved_hash = digest(second)
                pdf_sidecar.write_bytes(pdf_sidecar.read_bytes() + b"\n% external PDF edit\n")
                changed_pdf = digest(pdf_sidecar)
                run("save", snapshot_path, second, empty, after_save, succeeds=False)
                assert digest(second) == saved_hash and digest(pdf_sidecar) == changed_pdf

        # A plain PDF remains external and resolves after saving in a different folder.
        external = temporary / "external"
        external.mkdir()
        external_copy = external / "copy.xopp"
        run("save-copy", pdf, external_copy, empty)
        run("render", external_copy, external / "preview.pdf")
        # Publishing must reject sidecar collisions and leave the colliding asset intact.
        collision = temporary / "collision.xopp"
        sidecar = temporary / "collision.xopp.bg.pdf"
        sidecar.write_bytes(b"existing attachment")
        run("save-copy", fixtures / "packaged_xopp/pdfBackground/old.xopp", collision, empty, succeeds=False)
        assert not collision.exists() and sidecar.read_bytes() == b"existing attachment"
        assert not list(temporary.rglob(".xournalpp-gtk4-save-*"))
        assert all(digest(p) == value for p, value in hashes.items()), "Repository fixture modified"
    report = {"result": "passed", "operations": timings,
              "scope": "Native load/save/autosave/render, text/styles/layers, ink, attachments, recovery SAVE_AS guard, rejection and source preservation",
              "timing_note": "Single warm-cache correctness run; process startup included; not a speed comparison"}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: document worker ({len(timings)} operations), original fixtures unchanged")


if __name__ == "__main__":
    main()
