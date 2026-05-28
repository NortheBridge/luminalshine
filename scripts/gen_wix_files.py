#!/usr/bin/env python3
# WiX 3 file-component generator. Replaces the auto-generation that the
# CPack WIX generator does today, producing byte-identical Component IDs,
# File IDs, and Directory IDs so the resulting MSI diffs clean against
# the committed compatibility oracle
# (tests/fixtures/msi_golden/wix3_baseline.txt).
#
# Why we need this at all:
# CMake's CPack WIX generator is hardwired to WiX 3 (no plans for WiX
# 4+ support). The WiX-7 migration (PRs 3-4) needs a packaging pipeline
# that isn't bound to CPack. This script is the WiX-3 intermediate: it
# stays on the v3 toolchain but takes ownership of the file authoring
# CPack used to do, so the cutover to v7 can change the toolchain without
# also changing the file-authoring strategy.
#
# What we faithfully reproduce from CPack's algorithm
# (Source/CPack/WiX/cmCPackWIXGenerator.cxx in CMake upstream):
#
#   - The "P" (path-based) vs "H" (hashed) identifier-mode decision:
#       use H when the path-derived identifier would be longer than 60
#       chars, OR when more than 33% of its characters are illegal-for-IDs
#       (anything outside [A-Za-z0-9._]). Otherwise use P.
#   - Path-based identifier: every install-tree segment dotted together,
#       starting with the install COMPONENT name (`application`,
#       `assets`, `Unspecified`, etc.) as the FIRST segment. That's
#       why our golden shows IDs like `application.tools` and
#       `assets.assets.web.assets.foo` (the "double assets" is
#       component-name then first dest-subdir, both happening to be
#       called "assets"). Illegal chars replaced with `_`.
#   - Hashed identifier: `<sha1(forward_slashed_install_path)[:7]>_<basename[:52]>`
#       where `install_path` includes the leading component name segment
#       (verified empirically: `assets/assets/shaders/directx/foo.hlsl`
#       hashes to `07aae5b`, matching the golden).
#   - Component @Id and File @Id use the prefix `CM_C` / `CM_F` then
#       the mode letter (`P`/`H`) then `_` then the identifier.
#   - Directory @Id is always `CM_DP_<dotted_path>` (always path-mode;
#       paths short enough to not need hashing).
#   - Component @Guid = "*" — WiX's light.exe generates the stable
#       UUIDv5 itself from the Component's KeyPath at link time. This is
#       why we do NOT need to replicate any GUID-from-string algorithm:
#       the GUIDs we see in the MSI are produced by light, not CPack.
#   - Component @Win64="yes" for 64-bit installs.
#   - File @KeyPath="yes" on the single File child.
#
# What we DON'T try to reproduce: the legacy CMake-generated-GUID mode
# (per-user installs only — not used here; we're always ALLUSERS=1).

from __future__ import annotations

import argparse
import hashlib
import os
import sys
import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Iterable

# Per cmCPackWIXGenerator::IsLegalIdCharacter — these are the only
# characters a WiX Id attribute may contain unescaped. Everything else
# becomes '_' in normalized identifiers (and counts toward the >33%
# replacement threshold that triggers hash mode).
_LEGAL_ID_CHARS = re.compile(r"[A-Za-z0-9_.]")

# Mode-switch thresholds copied from cmCPackWIXGenerator::CreateNewIdForPath.
_HASH_LEN_THRESHOLD = 60
_HASH_REPLACE_PCT_THRESHOLD = 33
_HASH_MAX_FILENAME = 52


@dataclass
class FileEntry:
    # The install COMPONENT name (CMake's COMPONENT keyword on install()).
    install_component: str
    # Destination subdirectories under INSTALL_ROOT, e.g. ["tools"] or
    # ["assets", "web", "assets"]. Empty for files installed to ".".
    dest_dirs: list[str]
    # Basename of the installed file.
    basename: str
    # Absolute path of the staged file on disk (used as <File Source=>).
    source_abspath: str


def _normalize_identifier(raw: str) -> tuple[str, int]:
    """Replace illegal chars with '_'. Return (normalized, replacement_count)."""
    out_chars = []
    replacements = 0
    for ch in raw:
        if _LEGAL_ID_CHARS.match(ch):
            out_chars.append(ch)
        else:
            out_chars.append("_")
            replacements += 1
    return "".join(out_chars), replacements


def _install_path_segments(entry: FileEntry) -> list[str]:
    """Path segments used by both the identifier and the hash input:
    [install_component] + dest_dirs + [basename]."""
    return [entry.install_component, *entry.dest_dirs, entry.basename]


def make_ids(entry: FileEntry) -> tuple[str, str, str]:
    """Return (component_id, file_id, directory_id) — exactly what CPack
    would emit for this file. Pure function; no I/O. Validated against
    the committed golden via the self-test below.
    """
    segments = _install_path_segments(entry)

    path_identifier = ".".join(segments)
    norm_id, replacements = _normalize_identifier(path_identifier)

    # Decision rule from CreateNewIdForPath.
    use_hash = False
    if len(norm_id) > _HASH_LEN_THRESHOLD:
        use_hash = True
    elif norm_id and (replacements * 100 // len(norm_id)) > _HASH_REPLACE_PCT_THRESHOLD:
        use_hash = True

    if use_hash:
        # Hash input is the SAME segments joined by forward slashes — the
        # install path relative to INSTALL_ROOT *with the install-component
        # name as the leading segment*. Critically, this uses the RAW
        # basename (with any illegal chars intact), not the normalized
        # form: empirically the .css file whose Component ID shows
        # `AppEditConfigOverridesSection_aa40eebb.css` was hashed from
        # the on-disk `AppEditConfigOverridesSection-aa40eebb.css`
        # (hyphen, the form Vite actually emits). The shader case
        # (07aae5b → assets/assets/shaders/directx/convert_yuv420...hlsl)
        # has all-legal-char basenames so raw and normalized happen to
        # match — but the rule remains "hash the raw, then normalize for
        # the visible basename".
        hash_input = "/".join(segments)  # segments end with the RAW basename
        hash7 = hashlib.sha1(hash_input.encode("utf-8")).hexdigest()[:7]
        safe_basename, _ = _normalize_identifier(entry.basename)
        # CPack appends a literal "..." when the normalized basename
        # exceeds the 52-char ceiling — observed in the golden as
        # `CM_CH_16793d1_convert_..._perceptual_quantiz...` (52 chars +
        # ellipsis). The truncation keeps the total Component @Id under
        # MSI's 72-char Identifier column limit
        # (6 "CM_CH_" + 7 hash + 1 "_" + 52 basename + 3 "..." = 69).
        if len(safe_basename) > _HASH_MAX_FILENAME:
            identifier = f"{hash7}_{safe_basename[:_HASH_MAX_FILENAME]}..."
        else:
            identifier = f"{hash7}_{safe_basename}"
        prefix = "H"
    else:
        identifier = norm_id
        prefix = "P"

    component_id = f"CM_C{prefix}_{identifier}"
    file_id = f"CM_F{prefix}_{identifier}"

    # Directory ID: always path-based; segments are install_component +
    # dest_dirs (no basename). For files installed to "." (no dest_dirs)
    # the Directory is INSTALL_ROOT, NOT a CM_DP_ — and there's no
    # per-component intermediate dir either (the golden has e.g.
    # `CM_DP_application.tools` but NO `CM_DP_application` alone).
    dir_segments = [entry.install_component, *entry.dest_dirs]
    if entry.dest_dirs:
        dir_norm, _ = _normalize_identifier(".".join(dir_segments))
        directory_id = f"CM_DP_{dir_norm}"
    else:
        directory_id = "INSTALL_ROOT"

    return component_id, file_id, directory_id


# ---------------------------------------------------------------------------
# Self-test: validate against the committed golden's Component IDs.
# Runs offline (no MSI build needed). Fed every CM_CP_ / CM_CH_ row in
# the golden, reverse-derives the FileEntry that produced it, then asks
# make_ids and asserts byte-exact match.
# ---------------------------------------------------------------------------

_REPO_ROOT = Path(__file__).resolve().parent.parent
_GOLDEN = _REPO_ROOT / "tests" / "fixtures" / "msi_golden" / "wix3_baseline.txt"


def _parse_golden_rows(table: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    in_table = False
    with open(_GOLDEN, encoding="utf-8") as fh:
        for line in fh:
            stripped = line.rstrip("\n")
            if stripped.startswith("## TABLE: "):
                in_table = stripped == f"## TABLE: {table}"
                continue
            if not in_table or not stripped or stripped.startswith("#"):
                continue
            cells = {}
            for cell in stripped.split("|"):
                if "=" in cell:
                    k, _, v = cell.partition("=")
                    cells[k] = v
            rows.append(cells)
    return rows


def _decompose_dir_id(directory_id: str) -> tuple[str, list[str]] | None:
    """Reverse-engineer (install_component, dest_dirs) from a CM_DP_ id.
    Returns None if the directory is INSTALL_ROOT or a standard dir."""
    if not directory_id.startswith("CM_DP_"):
        return None
    tail = directory_id[len("CM_DP_"):]
    segments = tail.split(".")
    if not segments:
        return None
    return segments[0], segments[1:]


def _decompose_component_id(component_id: str) -> tuple[str, str, str | None]:
    """Decompose a Component @Id into (mode_letter, identifier, basename_for_path_mode).

    For CM_CP_<a.b.c.basename> we can read the basename directly as the
    final dotted segment. For CM_CH_<hash>_<basename> we can read the
    basename from the trailing part after the underscore."""
    if component_id.startswith("CM_CP_"):
        identifier = component_id[len("CM_CP_"):]
        # The last dotted segment may itself contain a dot (e.g. file
        # extension), but the IDENTIFIER joins segments with dots — so we
        # can't just take after the last dot. Instead, return identifier;
        # the caller reconstructs basename from Directory + KeyPath/File
        # Source if needed.
        return ("P", identifier, None)
    if component_id.startswith("CM_CH_"):
        rest = component_id[len("CM_CH_"):]
        # Format: <7-hex>_<basename>
        if len(rest) > 8 and rest[7] == "_":
            return ("H", rest, rest[8:])
    raise ValueError(f"unrecognized component id form: {component_id!r}")


def _self_test() -> int:
    if not _GOLDEN.exists():
        print(f"gen_wix_files self-test: missing golden at {_GOLDEN}", file=sys.stderr)
        return 1

    components = _parse_golden_rows("Component")
    failures: list[str] = []
    cp_checked = 0
    ch_checked = 0
    skipped = 0

    for row in components:
        cid = row.get("Component", "")
        if not (cid.startswith("CM_CP_") or cid.startswith("CM_CH_")):
            # Hand-authored components from custom_actions.wxs — out of
            # this generator's scope. (StartMenuShortcut, SunshineSvc,
            # etc.)
            skipped += 1
            continue

        directory_id = row.get("Directory_", "")
        keypath = row.get("KeyPath", "")

        # Figure out (install_component, dest_dirs, basename) from the
        # row. dest_dirs + install_component come from the Directory_;
        # basename is the trailing piece of the identifier. For files
        # installed to INSTALL_ROOT, we recover the install component
        # name from the IDENTIFIER's leading segment.
        if directory_id == "INSTALL_ROOT":
            # Path-mode CM_CP_<component>.<basename>
            if not cid.startswith("CM_CP_"):
                skipped += 1
                continue
            identifier = cid[len("CM_CP_"):]
            seg = identifier.split(".", 1)
            if len(seg) != 2:
                skipped += 1
                continue
            install_component = seg[0]
            # basename may contain dots (file extension). Recover from
            # KeyPath (which is the File @Id = "CM_FP_<same identifier>").
            basename_norm = seg[1]
            entry = FileEntry(install_component, [], basename_norm, "")
        else:
            decomp = _decompose_dir_id(directory_id)
            if decomp is None:
                skipped += 1
                continue
            install_component, dest_dirs = decomp
            # Recover basename: KeyPath = File @Id =
            #   "CM_FP_<identifier>" (path mode) or
            #   "CM_FH_<hash>_<basename>" (hash mode).
            if keypath.startswith("CM_FP_"):
                file_identifier = keypath[len("CM_FP_"):]
                # The identifier joins component + dest_dirs + basename
                # with dots. Strip the known prefix segments.
                prefix_segments = [install_component, *dest_dirs]
                expected_prefix = ".".join(prefix_segments) + "."
                if not file_identifier.startswith(expected_prefix):
                    skipped += 1
                    continue
                basename_norm = file_identifier[len(expected_prefix):]
            elif keypath.startswith("CM_FH_"):
                rest = keypath[len("CM_FH_"):]
                if len(rest) > 8 and rest[7] == "_":
                    basename_norm = rest[8:]
                else:
                    skipped += 1
                    continue
            else:
                skipped += 1
                continue
            entry = FileEntry(install_component, dest_dirs, basename_norm, "")

        # Limitation: basename_norm is the NORMALIZED basename (illegal
        # chars already replaced with '_'). To exactly reproduce the
        # identifier we'd need the raw basename. For path-mode this is
        # irrelevant (normalization is idempotent). For hash-mode the
        # hash input uses the RAW path so we can't fully recompute it
        # from the dump alone — but we CAN still validate the prefix
        # (CM_CH_<hash>_) and the suffix (<normalized_basename>) and
        # that the entry would trigger hash mode at all.
        got_component, got_file, got_dir = make_ids(entry)

        # Path-mode: byte-exact match required.
        if cid.startswith("CM_CP_"):
            if got_component != cid:
                failures.append(f"Component id mismatch: got {got_component!r} expected {cid!r}")
            if got_dir != directory_id:
                failures.append(f"Directory id mismatch for {cid}: got {got_dir!r} expected {directory_id!r}")
            if got_file != keypath:
                failures.append(f"File id mismatch for {cid}: got {got_file!r} expected {keypath!r}")
            cp_checked += 1
            continue

        # Hash-mode: SHAPE-only validation. We cannot byte-exact-check
        # the hash digit prefix here because CPack hashes the RAW
        # on-disk basename, and the golden's dump preserves only the
        # NORMALIZED basename (e.g. `AppEditConfigOverridesSection_aa40eebb.css`
        # was hashed from `AppEditConfigOverridesSection-aa40eebb.css`).
        # That information is not recoverable from the dump alone.
        # Shape checks done here:
        #   - generator chose hash mode (CM_CH_, not CM_CP_)
        #   - directory_id matches (depends only on dotted dir path)
        #   - basename portion after `<hash>_` matches the golden's
        #     normalized basename (including the literal "..." suffix
        #     when truncated past 52 chars)
        # The byte-exact hash check happens on Windows CI when the
        # full diff_msi_tables.py runs against a freshly built MSI.
        if not got_component.startswith("CM_CH_"):
            failures.append(f"Expected hash mode for {cid} but generator chose {got_component[:6]}")
            continue
        if got_dir != directory_id:
            failures.append(f"Hash-mode directory id mismatch for {cid}: got {got_dir!r} expected {directory_id!r}")
        # Compare the basename portion (everything after the 7-char hash + '_')
        if cid[len("CM_CH_"):][7:] != got_component[len("CM_CH_"):][7:]:
            failures.append(
                f"Hash-mode normalized basename mismatch for {cid}: "
                f"got tail {got_component[len('CM_CH_'):][7:]!r} expected {cid[len('CM_CH_'):][7:]!r}"
            )
        # File id: same shape rule, plus prefix
        if not got_file.startswith("CM_FH_") or got_file[len("CM_FH_"):][7:] != keypath[len("CM_FH_"):][7:]:
            failures.append(f"Hash-mode file id mismatch (shape) for {cid}: got {got_file!r} expected {keypath!r}")
        ch_checked += 1

    if failures:
        print(f"gen_wix_files self-test FAILED ({len(failures)} mismatches):", file=sys.stderr)
        for f in failures[:20]:
            print(f"  - {f}", file=sys.stderr)
        if len(failures) > 20:
            print(f"  ... and {len(failures)-20} more", file=sys.stderr)
        return 1

    # Also validate Directory table coverage: every CM_DP_ in the golden
    # should be reachable by some FileEntry we generated above. Skipped
    # in self-test for now (covered by the full diff on CI).
    print(
        f"gen_wix_files self-test: OK "
        f"({cp_checked} CM_CP_ + {ch_checked} CM_CH_ components checked, "
        f"{skipped} hand-authored components skipped)"
    )
    return 0


# ---------------------------------------------------------------------------
# Staging walker + wxs emitter (used at MSI build time; not exercised by
# the self-test). Kept compact and pure; emits to stdout or --out.
# ---------------------------------------------------------------------------


def walk_staging(staging_root: Path) -> list[FileEntry]:
    """Walk a `<staging_root>/<install_component>/<dest_dirs>/<file>`
    tree and emit a FileEntry per regular file."""
    entries: list[FileEntry] = []
    if not staging_root.is_dir():
        raise SystemExit(f"staging root not a directory: {staging_root}")
    for component_dir in sorted(p for p in staging_root.iterdir() if p.is_dir()):
        install_component = component_dir.name
        for current, _dirs, files in os.walk(component_dir):
            files.sort()
            current_path = Path(current)
            rel = current_path.relative_to(component_dir)
            dest_dirs: list[str] = [] if rel == Path(".") else list(rel.parts)
            for basename in files:
                abspath = (current_path / basename).resolve()
                entries.append(FileEntry(install_component, dest_dirs, basename, str(abspath)))
    return entries


def emit_wxs(entries: Iterable[FileEntry], fragment_id: str = "GeneratedFiles") -> str:
    """Emit a self-contained <Wix><Fragment> with the Directory tree,
    Components, Files, and a ComponentGroup pulling them all in. The
    consuming Product references the ComponentGroup via <ComponentGroupRef>.
    """
    # Build a directory tree first (so we can nest <Directory> elements
    # correctly). The tree's leaves carry their component list.
    @dataclass
    class DirNode:
        # Empty string for INSTALL_ROOT, otherwise the segment name.
        segment_name: str
        children: dict[str, "DirNode"] = field(default_factory=dict)
        files: list[FileEntry] = field(default_factory=list)
        wix_id: str = "INSTALL_ROOT"

    root = DirNode("")

    def ensure_dir(install_component: str, dest_dirs: list[str]) -> DirNode:
        node = root
        path_segments: list[str] = []
        for d in dest_dirs:
            path_segments.append(d)
            if d not in node.children:
                child = DirNode(d)
                # WiX Id for the child mirrors what make_ids() builds:
                # CM_DP_<install_component>.<dest_dirs joined by '.'>
                dotted, _ = _normalize_identifier(".".join([install_component, *path_segments]))
                child.wix_id = f"CM_DP_{dotted}"
                node.children[d] = child
            node = node.children[d]
        return node

    # Insert every file into the tree.
    for entry in entries:
        leaf = ensure_dir(entry.install_component, entry.dest_dirs)
        leaf.files.append(entry)

    # Emit XML.
    wix = ET.Element("Wix", {"xmlns": "http://schemas.microsoft.com/wix/2006/wi"})
    fragment = ET.SubElement(wix, "Fragment")

    # The Directory tree is rooted at INSTALL_ROOT, which is declared by
    # the main product .wxs. We reference it via DirectoryRef.
    dirref = ET.SubElement(fragment, "DirectoryRef", {"Id": "INSTALL_ROOT"})

    component_ids_in_order: list[str] = []

    def emit_node(parent_xml: ET.Element, node: DirNode) -> None:
        # Components for the files at this level.
        for entry in node.files:
            cid, fid, _did = make_ids(entry)
            component = ET.SubElement(parent_xml, "Component", {
                "Id": cid,
                "Guid": "*",
                "Win64": "yes",
            })
            ET.SubElement(component, "File", {
                "Id": fid,
                "KeyPath": "yes",
                "Source": entry.source_abspath,
            })
            component_ids_in_order.append(cid)
        # Children: emit a <Directory> for each, recurse.
        for child_name in sorted(node.children):
            child = node.children[child_name]
            child_xml = ET.SubElement(parent_xml, "Directory", {
                "Id": child.wix_id,
                "Name": child.segment_name,
            })
            emit_node(child_xml, child)

    emit_node(dirref, root)

    # ComponentGroup: lets the Product feature pull every generated
    # component in via one <ComponentGroupRef>.
    fragment2 = ET.SubElement(wix, "Fragment")
    group = ET.SubElement(fragment2, "ComponentGroup", {"Id": fragment_id})
    for cid in component_ids_in_order:
        ET.SubElement(group, "ComponentRef", {"Id": cid})

    # Pretty-print: ElementTree's serializer is dense; indent for
    # readability + diff-ability with the CPack-generated equivalents.
    ET.indent(wix, space="  ", level=0)
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(wix, encoding="unicode")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0] if __doc__ else "")
    parser.add_argument("--selftest", action="store_true", help="Run the offline self-test against the committed golden.")
    parser.add_argument("--staging", type=Path, help="Path to <staging>/<component>/<paths>/<file> tree.")
    parser.add_argument("--out", type=Path, help="Write emitted .wxs here instead of stdout.")
    parser.add_argument("--fragment-id", default="GeneratedFiles", help="Id for the <ComponentGroup> the Product features will reference.")
    args = parser.parse_args(argv[1:])

    if args.selftest:
        return _self_test()

    if not args.staging:
        parser.error("either --selftest or --staging is required")

    entries = walk_staging(args.staging)
    wxs = emit_wxs(entries, args.fragment_id)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(wxs, encoding="utf-8")
        print(f"[gen_wix_files] wrote {args.out} ({len(entries)} files)")
    else:
        sys.stdout.write(wxs)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
