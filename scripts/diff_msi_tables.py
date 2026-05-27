#!/usr/bin/env python3
# Compatibility oracle for the WiX 3 -> WiX 7 MSI migration.
#
# Diffs two normalized MSI table dumps (produced by
# scripts/dump_msi_tables.ps1) and fails if any UPGRADE-CRITICAL content
# differs. "Upgrade-critical" means anything Windows Installer uses to
# decide that a new MSI is an upgrade of an existing install, plus the
# bootstrapper<->MSI property contract:
#
#   - UpgradeCode and the Upgrade table
#   - Component GUIDs, KeyPaths, target Directories
#   - ServiceInstall / ServiceControl names and attributes
#   - Shortcut targets
#   - CustomAction definitions and their *relative* sequencing
#   - the INSTALL_* / KEEP* / *VIRTUALDISPLAYDRIVER properties the
#     bootstrapper passes to msiexec
#
# Volatile fields that legitimately change every build (ProductCode,
# PackageCode, ProductVersion) and absolute sequence NUMBERS (which WiX 7
# may renumber even when relative order is identical) are normalized away
# so they don't produce false positives. Relative action order IS
# compared.
#
# Usage:
#   diff_msi_tables.py GOLDEN CANDIDATE     # exit 0 if compatible, 1 if not
#   diff_msi_tables.py --selftest           # run built-in tests (no MSI needed)
#
# The --selftest path is wired into CTest and runs on every platform; the
# two-file diff path runs on Windows CI after the MSI is packaged.

from __future__ import annotations

import sys
from collections import OrderedDict

# Property names whose Value changes on every build / release and must be
# ignored when comparing the Property table. UpgradeCode is deliberately
# NOT here — it must stay stable, so a change to it should fail the diff.
VOLATILE_PROPERTY_KEYS = frozenset({
    "ProductCode",
    "ProductVersion",
    "PackageCode",
    "ProductName",  # tracked separately if desired; name is checked by CI assertions
})

# Tables whose rows carry an absolute "Sequence" number that may be
# renumbered by a different toolchain even when relative ordering is
# preserved. For these we compare the order of (Action, Condition) pairs,
# not the raw numbers.
SEQUENCE_TABLES = frozenset({
    "InstallExecuteSequence",
    "InstallUISequence",
})


def parse_dump(text: str) -> "OrderedDict[str, list[str]]":
    """Parse a normalized dump into {table_name: [row_strings]}.

    Comment lines (starting with '#') and blank lines are ignored except
    that '## TABLE: X' headers delimit sections.
    """
    tables: "OrderedDict[str, list[str]]" = OrderedDict()
    current = None
    for raw in text.splitlines():
        line = raw.rstrip("\r")
        if line.startswith("## TABLE:"):
            current = line[len("## TABLE:"):].strip()
            tables.setdefault(current, [])
            continue
        if not line or line.startswith("#"):
            continue
        if current is None:
            continue
        tables[current].append(line)
    return tables


def _row_to_dict(row: str) -> "OrderedDict[str, str]":
    cells: "OrderedDict[str, str]" = OrderedDict()
    for cell in row.split("|"):
        if "=" in cell:
            key, _, value = cell.partition("=")
            cells[key] = value
        else:
            cells[cell] = ""
    return cells


def normalize_table(name: str, rows: "list[str]") -> "list[str]":
    """Apply volatility normalization for a single table's rows.

    Returns a list of normalized row strings suitable for set/order
    comparison.
    """
    if name == "Property":
        kept = []
        for row in rows:
            cells = _row_to_dict(row)
            prop = cells.get("Property", "")
            if prop in VOLATILE_PROPERTY_KEYS:
                continue
            kept.append(row)
        return sorted(kept)

    if name in SEQUENCE_TABLES:
        # Compare relative order of (Action, Condition), dropping the
        # absolute Sequence number. Rows with no/empty Sequence (e.g.
        # standard actions pulled in implicitly) sort last but keep their
        # relative order via a stable sort on the numeric sequence.
        def seq_key(row: str) -> "tuple[int, str]":
            cells = _row_to_dict(row)
            raw = cells.get("Sequence", "")
            try:
                return (int(raw), "")
            except (TypeError, ValueError):
                # Conditional actions encode the condition where the
                # number would be; treat as "unordered, compare by action".
                return (1 << 30, cells.get("Action", ""))

        ordered = sorted(rows, key=seq_key)
        normalized = []
        for idx, row in enumerate(ordered):
            cells = _row_to_dict(row)
            action = cells.get("Action", "")
            condition = cells.get("Condition", "")
            normalized.append(f"rank={idx}|Action={action}|Condition={condition}")
        return normalized

    # Default: faithful, order-insensitive comparison.
    return sorted(rows)


def diff_dumps(golden_text: str, candidate_text: str) -> "list[str]":
    """Return a list of human-readable difference messages. Empty == match."""
    golden = parse_dump(golden_text)
    candidate = parse_dump(candidate_text)

    messages: "list[str]" = []

    all_tables = list(OrderedDict.fromkeys(list(golden.keys()) + list(candidate.keys())))
    for table in all_tables:
        if table not in golden:
            messages.append(f"[{table}] table present in candidate but absent in golden")
            continue
        if table not in candidate:
            messages.append(f"[{table}] table present in golden but absent in candidate")
            continue

        g_norm = normalize_table(table, golden[table])
        c_norm = normalize_table(table, candidate[table])

        if table in SEQUENCE_TABLES:
            # Ordered comparison.
            if g_norm != c_norm:
                # Report the first divergence for a focused message.
                for i in range(max(len(g_norm), len(c_norm))):
                    g_item = g_norm[i] if i < len(g_norm) else "<missing>"
                    c_item = c_norm[i] if i < len(c_norm) else "<missing>"
                    if g_item != c_item:
                        messages.append(
                            f"[{table}] action order diverges at position {i}: "
                            f"golden={g_item!r} candidate={c_item!r}"
                        )
                        break
            continue

        g_set = set(g_norm)
        c_set = set(c_norm)
        for removed in sorted(g_set - c_set):
            messages.append(f"[{table}] row in golden but not candidate: {removed}")
        for added in sorted(c_set - g_set):
            messages.append(f"[{table}] row in candidate but not golden: {added}")

    return messages


# --------------------------------------------------------------------------
# Self-test: synthetic fixtures, runs on any platform (no MSI required).
# --------------------------------------------------------------------------

_SELFTEST_GOLDEN = """\
# header comment
## TABLE: Property
Property=UpgradeCode|Value={C2C36624-2D9C-4AFD-9C79-6B7861AE4A0D}
Property=ProductCode|Value={AAAAAAAA-1111-1111-1111-111111111111}
Property=ProductVersion|Value=26.5.0.0
Property=INSTALL_SUDOVDA|Value=1

## TABLE: ServiceInstall
Component_=SunshineSvc|Name=LuminalShineService|Start=auto

## TABLE: InstallExecuteSequence
Action=KillProcsQuietImmediate|Condition=WIX_UPGRADE_DETECTED|Sequence=1400
Action=StopSvcQuietImmediate|Condition=WIX_UPGRADE_DETECTED|Sequence=1401
Action=InstallFiles|Condition=|Sequence=4000
"""

# Same product, a later build: ProductCode/PackageCode/ProductVersion
# differ, and the sequence NUMBERS are renumbered, but everything
# upgrade-critical is identical. Must compare EQUAL.
_SELFTEST_CANDIDATE_OK = """\
## TABLE: Property
Property=UpgradeCode|Value={C2C36624-2D9C-4AFD-9C79-6B7861AE4A0D}
Property=ProductCode|Value={BBBBBBBB-2222-2222-2222-222222222222}
Property=ProductVersion|Value=26.6.0.0
Property=INSTALL_SUDOVDA|Value=1

## TABLE: ServiceInstall
Component_=SunshineSvc|Name=LuminalShineService|Start=auto

## TABLE: InstallExecuteSequence
Action=KillProcsQuietImmediate|Condition=WIX_UPGRADE_DETECTED|Sequence=2100
Action=StopSvcQuietImmediate|Condition=WIX_UPGRADE_DETECTED|Sequence=2200
Action=InstallFiles|Condition=|Sequence=4000
"""

# UpgradeCode changed -> must FAIL (would break upgrade detection).
_SELFTEST_CANDIDATE_BAD_UPGRADECODE = _SELFTEST_GOLDEN.replace(
    "{C2C36624-2D9C-4AFD-9C79-6B7861AE4A0D}", "{DEADBEEF-0000-0000-0000-000000000000}"
)

# Service renamed -> must FAIL (SCM continuity break).
_SELFTEST_CANDIDATE_BAD_SERVICE = _SELFTEST_GOLDEN.replace(
    "Name=LuminalShineService", "Name=LuminalShineSvc2"
)

# Stop/kill actions reordered relative to InstallFiles -> must FAIL.
_SELFTEST_CANDIDATE_BAD_ORDER = """\
## TABLE: Property
Property=UpgradeCode|Value={C2C36624-2D9C-4AFD-9C79-6B7861AE4A0D}
Property=INSTALL_SUDOVDA|Value=1

## TABLE: ServiceInstall
Component_=SunshineSvc|Name=LuminalShineService|Start=auto

## TABLE: InstallExecuteSequence
Action=InstallFiles|Condition=|Sequence=1000
Action=KillProcsQuietImmediate|Condition=WIX_UPGRADE_DETECTED|Sequence=2000
Action=StopSvcQuietImmediate|Condition=WIX_UPGRADE_DETECTED|Sequence=2001
"""


def _run_selftest() -> int:
    failures = []

    def expect(label, condition):
        if not condition:
            failures.append(label)

    # 1. Renumbered + version-bumped but compatible -> no diffs.
    msgs = diff_dumps(_SELFTEST_GOLDEN, _SELFTEST_CANDIDATE_OK)
    expect(
        f"compatible candidate should produce no diffs, got: {msgs}",
        msgs == [],
    )

    # 2. UpgradeCode change -> at least one diff mentioning UpgradeCode.
    msgs = diff_dumps(_SELFTEST_GOLDEN, _SELFTEST_CANDIDATE_BAD_UPGRADECODE)
    expect(
        "UpgradeCode change must be flagged",
        any("UpgradeCode" in m for m in msgs),
    )

    # 3. Service rename -> flagged.
    msgs = diff_dumps(_SELFTEST_GOLDEN, _SELFTEST_CANDIDATE_BAD_SERVICE)
    expect(
        "service rename must be flagged",
        any("ServiceInstall" in m for m in msgs),
    )

    # 4. Action reorder -> flagged on the sequence table.
    msgs = diff_dumps(_SELFTEST_GOLDEN, _SELFTEST_CANDIDATE_BAD_ORDER)
    expect(
        "action reorder must be flagged",
        any("InstallExecuteSequence" in m and "order" in m for m in msgs),
    )

    # 5. Identity diff -> no diffs.
    msgs = diff_dumps(_SELFTEST_GOLDEN, _SELFTEST_GOLDEN)
    expect(f"identical dumps must match, got: {msgs}", msgs == [])

    if failures:
        print("diff_msi_tables self-test FAILED:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("diff_msi_tables self-test: OK (5 cases)")
    return 0


def main(argv: "list[str]") -> int:
    if len(argv) == 2 and argv[1] == "--selftest":
        return _run_selftest()

    if len(argv) != 3:
        print(
            "usage: diff_msi_tables.py GOLDEN CANDIDATE\n"
            "       diff_msi_tables.py --selftest",
            file=sys.stderr,
        )
        return 2

    golden_path, candidate_path = argv[1], argv[2]
    try:
        with open(golden_path, "r", encoding="utf-8") as fh:
            golden_text = fh.read()
    except OSError as exc:
        print(f"cannot read golden {golden_path!r}: {exc}", file=sys.stderr)
        return 2
    try:
        with open(candidate_path, "r", encoding="utf-8") as fh:
            candidate_text = fh.read()
    except OSError as exc:
        print(f"cannot read candidate {candidate_path!r}: {exc}", file=sys.stderr)
        return 2

    messages = diff_dumps(golden_text, candidate_text)
    if messages:
        print(
            f"MSI compatibility diff FAILED ({len(messages)} difference"
            f"{'s' if len(messages) != 1 else ''}):",
            file=sys.stderr,
        )
        for m in messages:
            print(f"  - {m}", file=sys.stderr)
        return 1

    print("MSI compatibility diff: OK (upgrade-critical tables match golden)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
