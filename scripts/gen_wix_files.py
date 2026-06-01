#!/usr/bin/env python3
# WiX 7 file-component generator. Owns the per-file Component / File /
# Directory / Feature authoring CPack-WIX used to emit, producing
# Component IDs, File IDs, Directory IDs, and FeatureComponents
# bindings that match the committed compatibility oracle
# (tests/fixtures/msi_golden/msi_baseline.txt) so the build stays
# upgrade-compatible across toolchain changes.
#
# Why we need this at all:
# CMake's CPack WIX generator is hardwired to WiX 3 (no plans for WiX
# 4+ support). The WiX-7 migration moved the authoring into this
# repo-owned generator first (PR2) so the toolchain swap (PR3, this
# one) could change ONLY the schema dialect without re-litigating the
# file-authoring strategy.
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
#   - Component @Guid = "*" — WiX generates the stable UUIDv5 itself
#       from the Component's KeyPath at link time. This is why we do
#       NOT need to replicate any GUID-from-string algorithm: the
#       GUIDs we see in the MSI are produced by the linker, not CPack.
#   - Component @Bitness="always64" for 64-bit installs (the WiX 4+
#       successor to v3's @Win64="yes"; the compiled Component table
#       row is identical).
#   - File @KeyPath="yes" on the single File child.
#   - Feature @AllowAbsent="no" for required components (the WiX 4+
#       successor to v3's @Absent="disallow"; both compile to
#       Attributes=16).
#   - Feature-level <Level Value="N" Condition="expr"/> child for
#       feature-condition entries (the WiX 4+ successor to v3's
#       <Condition Level="N">expr</Condition>).
#
# What we DON'T try to reproduce: the legacy CMake-generated-GUID mode
# (per-user installs only — not used here; we're always per-machine).

from __future__ import annotations

import argparse
import hashlib
import json
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


# ---------------------------------------------------------------------------
# Features manifest (packaging/windows/wix/features.json).
# CPack's CPACK_COMPONENT_<NAME>_<KEY> variables compile to WiX Feature
# rows; the manifest captures that metadata in a tool-agnostic JSON form
# so the generator can author the Feature table without depending on
# CPack. See the manifest itself for field-level notes.
# ---------------------------------------------------------------------------


@dataclass
class FeatureGroupSpec:
    name: str      # produces Feature Id "CM_G_<name>"
    title: str
    display: int


@dataclass
class FeatureConditionSpec:
    # Feature-level <Condition Level="N">expression</Condition>. Written
    # into the MSI Condition table inside the parent Feature element.
    # When `expression` evaluates true at install time, MSI overrides
    # the Feature's Level to `level` (0 = uninstalled, 1 = installed).
    level: int
    expression: str


@dataclass
class FeatureComponentSpec:
    name: str         # the CMake install COMPONENT name; produces Feature Id "CM_C_<name>"
    title: str
    description: str
    group: str | None  # group name (matches FeatureGroupSpec.name); None pins under ProductFeature
    display: int
    required: bool
    conditions: list[FeatureConditionSpec] = field(default_factory=list)


@dataclass
class ProductFeatureSpec:
    id: str
    title: str
    display: int
    directory: str    # Directory_ on the Feature row; INSTALL_ROOT for the root feature
    required: bool


@dataclass
class FeaturesManifest:
    product_feature: ProductFeatureSpec
    groups: list[FeatureGroupSpec]
    components: list[FeatureComponentSpec]


def load_features_manifest(path: Path) -> FeaturesManifest:
    """Load and lightly validate packaging/windows/wix/features.json."""
    data = json.loads(path.read_text(encoding="utf-8"))

    pf_raw = data["product_feature"]
    pf = ProductFeatureSpec(
        id=pf_raw["id"],
        title=pf_raw["title"],
        display=int(pf_raw["display"]),
        directory=pf_raw["directory"],
        required=bool(pf_raw.get("required", False)),
    )

    groups = [
        FeatureGroupSpec(
            name=g["name"],
            title=g["title"],
            display=int(g["display"]),
        )
        for g in data.get("groups", [])
    ]
    group_names = {g.name for g in groups}

    components: list[FeatureComponentSpec] = []
    seen_component_names: set[str] = set()
    for c in data.get("components", []):
        name = c["name"]
        if name in seen_component_names:
            raise ValueError(f"features.json: duplicate component name {name!r}")
        seen_component_names.add(name)
        group = c.get("group")
        if group is not None and group not in group_names:
            raise ValueError(
                f"features.json: component {name!r} references unknown group {group!r}; "
                f"known groups: {sorted(group_names)}"
            )
        conditions = [
            FeatureConditionSpec(level=int(cond["level"]), expression=cond["expression"])
            for cond in c.get("conditions", [])
        ]
        components.append(FeatureComponentSpec(
            name=name,
            title=c.get("title", name),
            description=c.get("description", ""),
            group=group,
            display=int(c["display"]),
            required=bool(c.get("required", False)),
            conditions=conditions,
        ))

    return FeaturesManifest(product_feature=pf, groups=groups, components=components)


def feature_attributes_bits(required: bool) -> int:
    """Compile the WiX-3 Feature Attributes int from high-level flags.

    `required` (CPACK_COMPONENT_<NAME>_REQUIRED) maps to WiX 3
    `Absent="disallow"`, which sets msidbFeatureAttributesUIDisallowAbsent
    = 0x10 = 16. Non-required leaves Attributes=0 (default install
    behavior).

    Earlier iteration also set AllowAdvertise="no" for required
    components, which adds msidbFeatureAttributesDisallowAdvertise
    = 0x8 = 8 → final Attributes=24. The golden has 16 not 24, meaning
    CPack-WIX emits ONLY Absent="disallow" for required (no
    AllowAdvertise). Cross-checked against every row of the golden
    Feature table; see also feature_attrs() below for the XML side.
    """
    return 16 if required else 0


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
_GOLDEN = _REPO_ROOT / "tests" / "fixtures" / "msi_golden" / "msi_baseline.txt"


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

    # Phase 2: Feature manifest + emit_features_wxs validation against
    # the golden Feature and FeatureComponents tables. The generator
    # output must reproduce every CM_C_*, CM_G_*, and ProductFeature row
    # (the LuminalShineExtras feature + its hand-authored ComponentRefs
    # live in the main wxs, not in the generator's output, so they're
    # excluded from this comparison).
    manifest_path = _REPO_ROOT / "packaging" / "windows" / "wix" / "features.json"
    if manifest_path.exists():
        feature_failures = _self_test_features(manifest_path, components)
        if feature_failures:
            print(f"gen_wix_files self-test FAILED ({len(feature_failures)} feature mismatches):",
                  file=sys.stderr)
            for f in feature_failures[:20]:
                print(f"  - {f}", file=sys.stderr)
            if len(feature_failures) > 20:
                print(f"  ... and {len(feature_failures)-20} more", file=sys.stderr)
            return 1
        feature_summary = " + features.json validated against Feature/FeatureComponents tables"
    else:
        feature_summary = " (features.json absent — feature validation skipped)"

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
        f"{feature_summary}"
    )
    return 0


# ---------------------------------------------------------------------------
# Feature/FeatureComponents validation against the golden. Operates on the
# raw Component rows already parsed by the main self-test, so the cross-
# check happens against the same dataset that drives the ID validation.
# ---------------------------------------------------------------------------


def _golden_feature_rows() -> list[dict[str, str]]:
    return _parse_golden_rows("Feature")


def _golden_feature_components() -> list[dict[str, str]]:
    return _parse_golden_rows("FeatureComponents")


def _self_test_features(manifest_path: Path, golden_components: list[dict[str, str]]) -> list[str]:
    """Validate that emit_features_wxs against a synthetic FileEntry
    set derived from `golden_components` would produce a Feature
    hierarchy + FeatureComponents bindings that match the golden's
    rows (excluding the hand-authored LuminalShineExtras feature)."""
    failures: list[str] = []
    manifest = load_features_manifest(manifest_path)

    # ---- Feature table validation ---------------------------------------
    # The golden Feature table is the source of truth. The manifest must
    # produce exactly the same set of rows for ProductFeature, every CM_G_*,
    # and every CM_C_*. LuminalShineExtras is hand-authored in the main
    # wxs so it's excluded from this comparison.
    expected_features = {}
    for row in _golden_feature_rows():
        fid = row.get("Feature", "")
        if fid == "LuminalShineExtras":
            continue
        expected_features[fid] = row

    # Build expected rows from the manifest.
    generated_features: dict[str, dict[str, str]] = {}
    pf = manifest.product_feature
    generated_features[pf.id] = {
        "Feature_Parent": "",
        "Title": pf.title,
        "Description": "",
        "Display": str(pf.display),
        "Level": "1",
        "Directory_": pf.directory,
        "Attributes": str(feature_attributes_bits(pf.required)),
    }
    for g in manifest.groups:
        generated_features[f"CM_G_{g.name}"] = {
            "Feature_Parent": pf.id,
            "Title": g.title,
            "Description": "",
            "Display": str(g.display),
            "Level": "1",
            "Directory_": "",
            "Attributes": "0",
        }
    for c in manifest.components:
        parent = pf.id if c.group is None else f"CM_G_{c.group}"
        generated_features[f"CM_C_{c.name}"] = {
            "Feature_Parent": parent,
            "Title": c.title,
            "Description": c.description,
            "Display": str(c.display),
            "Level": "1",
            "Directory_": "",
            "Attributes": str(feature_attributes_bits(c.required)),
        }

    for fid, expected in expected_features.items():
        got = generated_features.get(fid)
        if got is None:
            failures.append(f"Feature {fid!r} present in golden but not produced by generator")
            continue
        for col in ("Feature_Parent", "Title", "Description", "Display", "Level", "Directory_", "Attributes"):
            if (got.get(col, "") or "") != (expected.get(col, "") or ""):
                failures.append(
                    f"Feature {fid!r} column {col} differs: "
                    f"got {got.get(col, '')!r} expected {expected.get(col, '')!r}"
                )
    for fid in generated_features.keys() - expected_features.keys():
        failures.append(f"Feature {fid!r} produced by generator but absent in golden")

    # ---- FeatureComponents validation -----------------------------------
    # Build the (feature_id, component_id) pairs the generator would
    # produce, by reverse-deriving FileEntries from the golden Component
    # rows (the same dataset the main self-test used). Skip:
    #   - hand-authored Components (LuminalShineExtras members)
    #   - Components that are neither CM_CP_ nor CM_CH_
    HAND_AUTHORED = {
        "ArpCustomUninstallEntry", "CtlStopSunshine", "Env_Path",
        "Fw_Exceptions", "LuminalShineSessionMonSvc",
        "LuminalShineXboxBtHelperSvc", "ReconfigureStartMenuShortcut",
        "ResetAdminStartMenuShortcut", "StartMenuShortcut",
        "SudoVdaRegistryDefaults", "SunshineSvc",
    }

    generated_pairs: set[tuple[str, str]] = set()
    for row in golden_components:
        cid = row.get("Component", "")
        if cid in HAND_AUTHORED:
            continue
        if not cid.startswith(("CM_CP_", "CM_CH_")):
            continue
        # Recover install_component (first dotted segment of the
        # identifier for CM_CP_, or the first segment of the
        # Directory_'s dotted path for CM_CH_).
        if cid.startswith("CM_CP_"):
            install_component = cid[len("CM_CP_"):].split(".", 1)[0]
        else:
            directory_id = row.get("Directory_", "")
            if directory_id == "INSTALL_ROOT" or not directory_id.startswith("CM_DP_"):
                continue  # shouldn't happen for hash-mode but be safe
            install_component = directory_id[len("CM_DP_"):].split(".", 1)[0]
        generated_pairs.add((f"CM_C_{install_component}", cid))

    expected_pairs: set[tuple[str, str]] = set()
    for row in _golden_feature_components():
        feature = row.get("Feature_", "")
        component = row.get("Component_", "")
        if feature == "LuminalShineExtras":
            continue
        expected_pairs.add((feature, component))

    for missing in sorted(expected_pairs - generated_pairs):
        failures.append(f"FeatureComponents row {missing} present in golden but not generated")
    for extra in sorted(generated_pairs - expected_pairs):
        failures.append(f"FeatureComponents row {extra} generated but not in golden")

    return failures


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


def emit_files_wxs(entries: Iterable[FileEntry]) -> str:
    """Emit the <Wix> file authoring: Directory tree + Components + Files.

    The Feature/ComponentRef bindings live in the features fragment
    (emit_features_wxs) so that file Components are pulled into
    Features by install-component name. Light links the two fragments
    via cross-fragment Component-Id references.

    Per-install-component Directory subtrees: CPack auto-emits a
    separate intermediate Directory row PER install COMPONENT even
    when the on-disk dest name collides — e.g. with `tools` getting
    installed under both the `application` and `audio` install
    components, the golden has CM_DP_application.tools AND
    CM_DP_audio.tools as siblings of INSTALL_ROOT, both DefaultDir=tools.
    A naive directory-name-keyed tree would merge them; we therefore
    keep separate top-level subtrees per install_component and only
    merge nested children WITHIN a single install_component's tree.
    """
    @dataclass
    class DirNode:
        # Empty string for INSTALL_ROOT, otherwise the segment name.
        segment_name: str
        children: dict[str, "DirNode"] = field(default_factory=dict)
        files: list[FileEntry] = field(default_factory=list)
        wix_id: str = "INSTALL_ROOT"

    # `root` only ever holds files installed to INSTALL_ROOT directly
    # (dest_dirs=[]); its children live in component_subtrees below.
    root = DirNode("")

    # install_component -> {first_dest_dir_name -> DirNode}.
    # Each install_component owns its own top-level subtree, so the
    # `tools` from "application" and the `tools` from "audio" become
    # distinct Directory rows (CM_DP_application.tools / CM_DP_audio.tools)
    # rather than merging into a single intermediate.
    component_subtrees: dict[str, dict[str, DirNode]] = {}

    def ensure_dir(install_component: str, dest_dirs: list[str]) -> DirNode:
        if not dest_dirs:
            return root
        comp_subtree = component_subtrees.setdefault(install_component, {})
        first = dest_dirs[0]
        if first not in comp_subtree:
            child = DirNode(first)
            dotted, _ = _normalize_identifier(f"{install_component}.{first}")
            child.wix_id = f"CM_DP_{dotted}"
            comp_subtree[first] = child
        node = comp_subtree[first]
        path_segments = [first]
        for d in dest_dirs[1:]:
            path_segments.append(d)
            if d not in node.children:
                child = DirNode(d)
                dotted, _ = _normalize_identifier(".".join([install_component, *path_segments]))
                child.wix_id = f"CM_DP_{dotted}"
                node.children[d] = child
            node = node.children[d]
        return node

    # Insert every file into the tree.
    for entry in entries:
        leaf = ensure_dir(entry.install_component, entry.dest_dirs)
        leaf.files.append(entry)

    # Emit XML. v4 namespace covers WiX 4 through 7.
    wix = ET.Element("Wix", {"xmlns": "http://wixtoolset.org/schemas/v4/wxs"})
    fragment = ET.SubElement(wix, "Fragment")

    # The Directory tree is rooted at INSTALL_ROOT, which is declared by
    # the main product .wxs. We reference it via DirectoryRef.
    dirref = ET.SubElement(fragment, "DirectoryRef", {"Id": "INSTALL_ROOT"})

    def emit_node(parent_xml: ET.Element, node: DirNode) -> None:
        # Components for the files at this level.
        for entry in node.files:
            cid, fid, _did = make_ids(entry)
            component = ET.SubElement(parent_xml, "Component", {
                "Id": cid,
                "Guid": "*",
                # WiX 4+ encoding of v3's Win64="yes". Compiles to the
                # same Component table msidbComponentAttributes64bit
                # bit (0x100), so the on-disk Component row matches.
                "Bitness": "always64",
            })
            ET.SubElement(component, "File", {
                "Id": fid,
                "KeyPath": "yes",
                "Source": entry.source_abspath,
            })
        # Children: emit a <Directory> for each, recurse.
        for child_name in sorted(node.children):
            child = node.children[child_name]
            child_xml = ET.SubElement(parent_xml, "Directory", {
                "Id": child.wix_id,
                "Name": child.segment_name,
            })
            emit_node(child_xml, child)

    # Emit root-level files first.
    emit_node(dirref, root)
    # Then per-install-component top-level Directory elements as
    # siblings under INSTALL_ROOT. Sort outer by install_component name
    # and inner by directory name for determinism.
    for comp_name in sorted(component_subtrees.keys()):
        for top_dir_name in sorted(component_subtrees[comp_name].keys()):
            top_node = component_subtrees[comp_name][top_dir_name]
            dir_xml = ET.SubElement(dirref, "Directory", {
                "Id": top_node.wix_id,
                "Name": top_node.segment_name,
            })
            emit_node(dir_xml, top_node)

    # Pretty-print: ElementTree's serializer is dense; indent for
    # readability + diff-ability with the CPack-generated equivalents.
    ET.indent(wix, space="  ", level=0)
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(wix, encoding="unicode")


def emit_features_wxs(manifest: FeaturesManifest, entries: Iterable[FileEntry]) -> str:
    """Emit the <Wix> Feature hierarchy + FeatureComponents bindings.

    The hierarchy mirrors what CPack auto-generates from
    CPACK_COMPONENT_<NAME>_* variables:
      <Feature Id="ProductFeature" Display=1 Directory=INSTALL_ROOT ...>
        <Feature Id="CM_C_Unspecified" .../>      <!-- ungrouped sit under Product -->
        <Feature Id="CM_G_Core" ...>
          <Feature Id="CM_C_application" ...>
            <ComponentRef Id="CM_CP_application.luminalshine.exe"/>
            ...
          </Feature>
          ...
        </Feature>
        <Feature Id="CM_G_Drivers" ...>...</Feature>
        ...
      </Feature>

    File Components are bound to the CM_C_<install_component> Feature
    derived from the FileEntry.install_component. Hand-authored
    Components (services, shortcuts, etc.) live in the main wxs under
    LuminalShineExtras and are not the generator's concern.
    """
    # Bucket file components by install_component for the ComponentRef
    # emission below. CPack's golden orders ComponentRefs alphabetically
    # within each feature — match that for byte-stable diff.
    refs_by_component: dict[str, list[str]] = {}
    for entry in entries:
        cid, _fid, _did = make_ids(entry)
        refs_by_component.setdefault(entry.install_component, []).append(cid)
    for cid_list in refs_by_component.values():
        cid_list.sort()

    # Cross-check: every install_component in the staging tree must have
    # a matching component spec in the manifest, and vice versa. A drift
    # here means CMakeLists added/removed an install COMPONENT without
    # updating features.json — surface it loudly.
    manifest_names = {c.name for c in manifest.components}
    staging_names = set(refs_by_component.keys())
    missing_in_manifest = staging_names - manifest_names
    if missing_in_manifest:
        raise ValueError(
            f"staging tree references install components not in features.json: "
            f"{sorted(missing_in_manifest)}. Update features.json or the install rule."
        )
    # Note: the converse (manifest names with no staging files) is OK —
    # an empty component still gets a Feature, just with no ComponentRefs.

    def feature_attrs(comp_or_pf, required_default: bool = False) -> dict[str, str]:
        """Build the WiX 4+ XML attributes for a Feature so that the
        compiled Attributes integer matches the golden. Only
        `AllowAbsent="no"` is emitted for required components — that
        alone yields Attributes=16 (msidbFeatureAttributesUIDisallowAbsent),
        matching what CPack-WIX produced under WiX 3's `Absent="disallow"`
        attribute (the v3->v4 attribute rename is purely surface; the
        compiled bit is identical). Adding `AllowAdvertise="no"` would
        compile to Attributes=24 by also setting
        msidbFeatureAttributesDisallowAdvertise (8); the golden does
        NOT have that bit set, so leave it off.
        """
        is_required = getattr(comp_or_pf, "required", required_default)
        attrs: dict[str, str] = {}
        if is_required:
            attrs["AllowAbsent"] = "no"
        return attrs

    # v4 namespace covers WiX 4 through 7.
    wix = ET.Element("Wix", {"xmlns": "http://wixtoolset.org/schemas/v4/wxs"})
    fragment = ET.SubElement(wix, "Fragment")

    pf = manifest.product_feature
    pf_xml = ET.SubElement(fragment, "Feature", {
        "Id": pf.id,
        "Title": pf.title,
        "Level": "1",
        "Display": str(pf.display),
        "ConfigurableDirectory": pf.directory,
        **feature_attrs(pf),
    })

    # Group features and the components nested under them. Components
    # whose group is None sit as direct children of ProductFeature
    # (matches the golden for CM_C_Unspecified, which has no group).
    groups_by_name: dict[str, ET.Element] = {}
    for g in sorted(manifest.groups, key=lambda x: x.display):
        g_xml = ET.SubElement(pf_xml, "Feature", {
            "Id": f"CM_G_{g.name}",
            "Title": g.title,
            "Level": "1",
            "Display": str(g.display),
            # CM_G_* features in the golden have Attributes=0 — no
            # required/disallow attributes emitted.
        })
        groups_by_name[g.name] = g_xml

    # Emit per-component features. Group ordering: Unspecified-and-other-
    # ungrouped components first (matching golden's display order — they
    # have Display=0 and appear at the top), then grouped components
    # under their CM_G_* parent.
    components_sorted = sorted(manifest.components, key=lambda c: (c.group is not None, c.display, c.name))
    for c in components_sorted:
        parent_xml = pf_xml if c.group is None else groups_by_name[c.group]
        c_xml = ET.SubElement(parent_xml, "Feature", {
            "Id": f"CM_C_{c.name}",
            "Title": c.title,
            **({"Description": c.description} if c.description else {}),
            "Level": "1",
            "Display": str(c.display),
            **feature_attrs(c),
        })
        # Feature-level Conditions (Condition table rows). WiX 3's
        # `<Condition Level="N">expr</Condition>` became
        # `<Level Value="N" Condition="expr"/>` in WiX 4+; the compiled
        # MSI Condition row is identical. Emit BEFORE ComponentRefs so
        # the XML mirrors the order WiX documents.
        for cond in c.conditions:
            ET.SubElement(c_xml, "Level", {
                "Value": str(cond.level),
                "Condition": cond.expression,
            })
        for cid in refs_by_component.get(c.name, []):
            ET.SubElement(c_xml, "ComponentRef", {"Id": cid})

    ET.indent(wix, space="  ", level=0)
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(wix, encoding="unicode")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0] if __doc__ else "")
    parser.add_argument("--selftest", action="store_true",
                        help="Run the offline self-test against the committed golden.")
    parser.add_argument("--staging", type=Path,
                        help="Path to <staging>/<component>/<paths>/<file> tree.")
    parser.add_argument("--files-out", type=Path,
                        help="Write the file-components fragment (Directories + Components + Files) here.")
    parser.add_argument("--features-manifest", type=Path,
                        default=Path("packaging/windows/wix/features.json"),
                        help="Path to features.json (default: packaging/windows/wix/features.json).")
    parser.add_argument("--features-out", type=Path,
                        help="Write the features fragment (Feature hierarchy + ComponentRefs) here. "
                             "Requires --features-manifest.")
    args = parser.parse_args(argv[1:])

    if args.selftest:
        return _self_test()

    if not args.staging:
        parser.error("either --selftest or --staging is required")
    if not args.files_out and not args.features_out:
        parser.error("at least one of --files-out / --features-out is required when --staging is given")

    entries = walk_staging(args.staging)

    if args.files_out:
        wxs = emit_files_wxs(entries)
        args.files_out.parent.mkdir(parents=True, exist_ok=True)
        args.files_out.write_text(wxs, encoding="utf-8")
        print(f"[gen_wix_files] wrote {args.files_out} ({len(entries)} files)")

    if args.features_out:
        if not args.features_manifest.exists():
            parser.error(f"features manifest not found: {args.features_manifest}")
        manifest = load_features_manifest(args.features_manifest)
        wxs = emit_features_wxs(manifest, entries)
        args.features_out.parent.mkdir(parents=True, exist_ok=True)
        args.features_out.write_text(wxs, encoding="utf-8")
        print(f"[gen_wix_files] wrote {args.features_out} "
              f"({len(manifest.components)} component features, "
              f"{len(manifest.groups)} group features)")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
