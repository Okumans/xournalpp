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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--report", type=Path)
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
        ink.write_text("XOURNALPP_GTK4_INK_V1 3\n"
                       "0 0 1 4 1 0 0 3\n20 20 .25\n40 40 .5\n60 20 .75\n"
                       "1 1 0 14 1 1 0 2\n20 70 1\n100 70 1\n"
                       "0 0 0 3 0 0 1 1\n70 90 1\n")
        output = temporary / "copy.xopp"
        run("save-copy", snapshot, output, ink)
        saved = xml(output)
        before = xml(snapshot)
        for index, saved_page in enumerate(saved.findall("page")):
            layers = saved_page.findall("layer")
            appended = list(layers[-1])
            assert len(appended) == (2 if index == 0 else 1)
            for stroke in appended:
                layers[-1].remove(stroke)
            assert content(saved_page) == content(before.findall("page")[index])
            if index == 0:
                assert appended[0].attrib["tool"] == "pen"
                assert list(map(float, appended[0].attrib["width"].split())) == [4, 1, 2]
                assert len(appended[1].text.split()) == 4  # A duplicated dot.
            else:
                assert appended[0].attrib["tool"] == "highlighter"
                assert float(appended[0].attrib["width"]) == 14
        inspected = json.loads(run("inspect", output))
        assert inspected["pages"][0]["texts"] == original["pages"][0]["texts"]
        assert inspected["pages"][0]["strokes"] == 2
        run("render", output, temporary / "saved.pdf")

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
                                      "XOURNALPP_GTK4_INK_V1 1\n0 0 0 3 1 0 0 1\n-1 1 1\n")):
            ink.write_text(data)
            rejected = temporary / f"rejected-{index}.xopp"
            run("save-copy", snapshot, rejected, ink, succeeds=False)
            assert not rejected.exists()
        # Corrupt/recoverable input must not silently become a partial saved copy.
        corrupt = temporary / "corrupt.xopp"
        corrupt.write_bytes(gzip.compress(b'<xournal><page width="500" height="500">'))
        run("save-copy", corrupt, temporary / "recovered.xopp", empty, succeeds=False)
        assert not (temporary / "recovered.xopp").exists()

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
