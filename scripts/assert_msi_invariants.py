#!/usr/bin/env python3
# Hard CI gate for MSI upgrade invariants.
#
# `diff_msi_tables.py` compares the freshly built MSI against the
# committed baseline — a "tell me about any drift" check. This script
# is the narrower companion: a fixed list of upgrade-load-bearing
# values that MUST appear in the candidate MSI regardless of toolchain
# changes elsewhere. It catches the class of regression where the
# overall diff is allowed to update (intentional toolchain swap,
# Wix4UtilCA_X64 rename, etc.) but one of these specific values
# silently slipped — exactly what the WiX 7 cutover (PR 3) had to
# avoid: byte-different fingerprint, byte-identical invariants.
#
# Usage:
#   assert_msi_invariants.py <candidate.txt>     # exit 0 on pass, 1 on fail
#   assert_msi_invariants.py --selftest          # offline fixture tests
#
# The selftest runs on every platform via CTest. The full check runs
# on Windows CI after the dump is produced.

from __future__ import annotations

import sys
from collections import OrderedDict


# ----------------------------------------------------------------------------
# Pinned invariants. Adding, changing, or removing any value in this
# block REQUIRES a deliberate code review against the upgrade-impact
# checklist in docs/wix7-migration.md.
# ----------------------------------------------------------------------------

# UpgradeCode is the MSI-level identity that links a product family
# across versions. Changing it breaks upgrade detection for every
# installed user — they'd see a side-by-side install instead of a
# replacement. NEVER change this value.
PINNED_UPGRADE_CODE = "{C2C36624-2D9C-4AFD-9C79-6B7861AE4A0D}"

# ServiceInstall.Name is the Windows-service short name. Renaming it
# detaches every installed copy's SCM state — the new service registers
# under the new name while the old one lingers (or, if removed via
# ServiceControl, takes its registered persistent state with it).
# Legacy SunshineService / sunshinesvc are NOT pinned here — those are
# cleaned up by CtlStopSunshine's ServiceControl and KillProcsQuiet,
# and the post-26.05.1 builds don't install them at all.
PINNED_SERVICE_INSTALL_NAMES = frozenset({
    "LuminalShineService",
    "LuminalShineXboxBtHelper",
    "LuminalShineSessionMonitor",
})

# Component GUIDs that must stay stable so MSI sees the same Component
# row identity across versions. Changing one of these triggers MSI's
# "different component" path on upgrade: old component removed (taking
# its resources), new component installed (writing them back) — a
# state-churn that's harmless for stateless resources but observable for
# shortcut targets, registry values with downstream readers, etc.
#
# Only includes Components whose Guid is a literal in the .wxs sources
# (Guid="*" Components get fresh auto-derived GUIDs every toolchain
# bump and would falsely fail this check). The five below are pinned
# because Reset Admin / Reconfigure / Start Menu shortcuts and the
# CtlStopSunshine / Env_Path / Fw_Exceptions service-control wrappers
# need stable identity for their on-upgrade contract.
PINNED_COMPONENT_GUIDS = {
    "StartMenuShortcut": "{A3B4C5D6-E7F8-4A5B-9C2D-3E4F5A6B7C8D}",
    "ReconfigureStartMenuShortcut": "{F8E9D7C6-B5A4-4938-8271-6C5D4E3F2A1B}",
    "ResetAdminStartMenuShortcut": "{C9F1E2B0-3A8D-4F1C-95E2-7B6A0D5F4C8E}",
    "CtlStopSunshine": "{B6D8A6A3-63B7-4C3F-8A2C-2C8F2B2F3B61}",
    "Env_Path": "{0D8C0E3E-6A7D-48E2-9A1C-0B1A6B7D8C90}",
    "Fw_Exceptions": "{2A7E0C83-2F3D-4C0C-9D5D-7C0B1A2E3F45}",
}

# Bootstrapper -> MSI property contract. The C# bootstrapper passes
# these as `/PROP=VALUE` arguments to msiexec; the MSI must accept them
# (i.e. the Property table must declare them, even if their default
# values are unset). Removing one from the MSI side silently breaks
# whatever bootstrapper feature it gated.
REQUIRED_BOOTSTRAPPER_PROPERTIES = frozenset({
    "INSTALL_ROOT",
    "INSTALL_MTTVDD",
    "INSTALL_SUDOVDA",
    "REMOVEVIRTUALDISPLAYDRIVER",
    "SKIP_REMOVE_CONFLICTING_PRODUCTS",
})

# Feature-level Conditions that gate the virtual-display backends on
# the bootstrapper-supplied INSTALL_MTTVDD / INSTALL_SUDOVDA properties.
# The condition's Level value flips the feature between install (Level=1)
# and uninstall (Level=0). Without these rows, the deferred install.ps1
# custom actions could fire against files that were never staged. The
# expression strings come from packaging/windows/wix/features.json.
REQUIRED_FEATURE_CONDITIONS = {
    "CM_C_mttvdd": ("0", 'INSTALL_MTTVDD <> "1"'),
    "CM_C_sudovda": ("1", 'INSTALL_SUDOVDA = "1"'),
}


# ----------------------------------------------------------------------------
# Dump parsing (lightweight; shares the format with diff_msi_tables.py
# but is intentionally not coupled to its module so a bug here can't
# disable the broader differ).
# ----------------------------------------------------------------------------

def parse_dump(text: str) -> "OrderedDict[str, list[dict[str, str]]]":
    """Parse a normalized dump into {table: [row_dict, ...]}."""
    tables: "OrderedDict[str, list[dict[str, str]]]" = OrderedDict()
    current: str | None = None
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
        row: dict[str, str] = {}
        for cell in line.split("|"):
            if "=" in cell:
                key, _, value = cell.partition("=")
                row[key] = value
        tables[current].append(row)
    return tables


# ----------------------------------------------------------------------------
# Individual invariants.
# ----------------------------------------------------------------------------

def check_upgrade_code(tables) -> list[str]:
    """Property table must declare UpgradeCode with the pinned value."""
    failures: list[str] = []
    rows = tables.get("Property", [])
    found = None
    for row in rows:
        if row.get("Property") == "UpgradeCode":
            found = row.get("Value", "")
            break
    if found is None:
        failures.append("UpgradeCode property is missing from the MSI")
    elif found != PINNED_UPGRADE_CODE:
        failures.append(
            f"UpgradeCode drift: got {found!r}, expected {PINNED_UPGRADE_CODE!r}. "
            "Changing UpgradeCode breaks upgrade detection for every "
            "installed user — they will see a side-by-side install."
        )
    return failures


def check_service_install_names(tables) -> list[str]:
    """Every pinned service name must appear in ServiceInstall.Name."""
    failures: list[str] = []
    rows = tables.get("ServiceInstall", [])
    present = {row.get("Name", "") for row in rows}
    for required in PINNED_SERVICE_INSTALL_NAMES:
        if required not in present:
            failures.append(
                f"ServiceInstall row with Name={required!r} is missing. "
                "Renaming or removing a service detaches the SCM state of "
                "every installed copy."
            )
    return failures


def check_component_guids(tables) -> list[str]:
    """Pinned-Guid Components must have the expected ComponentId."""
    failures: list[str] = []
    rows = tables.get("Component", [])
    by_name = {row.get("Component", ""): row.get("ComponentId", "") for row in rows}
    for comp_name, expected_guid in PINNED_COMPONENT_GUIDS.items():
        actual = by_name.get(comp_name)
        if actual is None:
            failures.append(
                f"Component {comp_name!r} is missing from the MSI"
            )
        elif actual != expected_guid:
            failures.append(
                f"Component {comp_name!r} ComponentId drift: "
                f"got {actual!r}, expected {expected_guid!r}. "
                "Stable Guid components must keep their literal Guid in "
                "custom_actions.wxs to preserve upgrade identity."
            )
    return failures


def check_bootstrapper_properties(tables) -> list[str]:
    """Each bootstrapper-contract property must be declared."""
    failures: list[str] = []
    rows = tables.get("Property", [])
    declared = {row.get("Property", "") for row in rows}
    for required in REQUIRED_BOOTSTRAPPER_PROPERTIES:
        if required not in declared:
            failures.append(
                f"Property {required!r} (bootstrapper-contract) is missing. "
                "The C# bootstrapper passes this on the msiexec command line "
                "and the MSI silently drops it when the Property row is absent."
            )
    return failures


def check_feature_conditions(tables) -> list[str]:
    """Pinned feature-level Conditions must exist with the expected Level + expression."""
    failures: list[str] = []
    rows = tables.get("Condition", [])
    by_feature: dict[str, list[tuple[str, str]]] = {}
    for row in rows:
        feature = row.get("Feature_", "")
        by_feature.setdefault(feature, []).append(
            (row.get("Level", ""), row.get("Condition", ""))
        )
    for feature_id, (expected_level, expected_expr) in REQUIRED_FEATURE_CONDITIONS.items():
        rows_for = by_feature.get(feature_id, [])
        if not rows_for:
            failures.append(
                f"Condition table is missing the feature-level row for "
                f"Feature_={feature_id!r}. Without this row, the deferred "
                "install.ps1 custom action for that backend could fire "
                "against files that were never staged."
            )
            continue
        if (expected_level, expected_expr) not in rows_for:
            failures.append(
                f"Condition row for Feature_={feature_id!r} drift: "
                f"got {rows_for!r}, expected (Level={expected_level!r}, "
                f"Condition={expected_expr!r})."
            )
    return failures


# ----------------------------------------------------------------------------
# Self-test: synthetic dumps that demonstrate pass + every failure mode.
# Runs on every platform via CTest.
# ----------------------------------------------------------------------------

_OK_DUMP = """\
## TABLE: Property
Property=UpgradeCode|Value={C2C36624-2D9C-4AFD-9C79-6B7861AE4A0D}
Property=INSTALL_ROOT|Value=
Property=INSTALL_MTTVDD|Value=0
Property=INSTALL_SUDOVDA|Value=1
Property=REMOVEVIRTUALDISPLAYDRIVER|Value=0
Property=SKIP_REMOVE_CONFLICTING_PRODUCTS|Value=0

## TABLE: ServiceInstall
ServiceInstall=A|Name=LuminalShineService|Start=auto
ServiceInstall=B|Name=LuminalShineXboxBtHelper|Start=auto
ServiceInstall=C|Name=LuminalShineSessionMonitor|Start=auto

## TABLE: Component
Component=StartMenuShortcut|ComponentId={A3B4C5D6-E7F8-4A5B-9C2D-3E4F5A6B7C8D}|Directory_=PMF
Component=ReconfigureStartMenuShortcut|ComponentId={F8E9D7C6-B5A4-4938-8271-6C5D4E3F2A1B}|Directory_=PMF
Component=ResetAdminStartMenuShortcut|ComponentId={C9F1E2B0-3A8D-4F1C-95E2-7B6A0D5F4C8E}|Directory_=PMF
Component=CtlStopSunshine|ComponentId={B6D8A6A3-63B7-4C3F-8A2C-2C8F2B2F3B61}|Directory_=IR
Component=Env_Path|ComponentId={0D8C0E3E-6A7D-48E2-9A1C-0B1A6B7D8C90}|Directory_=IR
Component=Fw_Exceptions|ComponentId={2A7E0C83-2F3D-4C0C-9D5D-7C0B1A2E3F45}|Directory_=IR

## TABLE: Condition
Feature_=CM_C_mttvdd|Level=0|Condition=INSTALL_MTTVDD <> "1"
Feature_=CM_C_sudovda|Level=1|Condition=INSTALL_SUDOVDA = "1"
"""


def _run_selftest() -> int:
    failures: list[str] = []

    def expect(label: str, cond: bool) -> None:
        if not cond:
            failures.append(label)

    # Case 1: OK dump → no failures from any check.
    tables = parse_dump(_OK_DUMP)
    msgs = (check_upgrade_code(tables) + check_service_install_names(tables)
            + check_component_guids(tables) + check_bootstrapper_properties(tables))
    expect(f"OK dump should pass cleanly, got: {msgs}", msgs == [])

    # Case 2: UpgradeCode drift → flagged with the value in the message.
    bad = parse_dump(_OK_DUMP.replace(PINNED_UPGRADE_CODE,
                                     "{DEADBEEF-0000-0000-0000-000000000000}"))
    msgs = check_upgrade_code(bad)
    expect("UpgradeCode drift must be flagged",
           any("UpgradeCode drift" in m for m in msgs))

    # Case 3: missing UpgradeCode → flagged with "missing".
    no_uc = parse_dump(_OK_DUMP.replace(
        f"Property=UpgradeCode|Value={PINNED_UPGRADE_CODE}", ""))
    msgs = check_upgrade_code(no_uc)
    expect("Missing UpgradeCode must be flagged",
           any("missing" in m for m in msgs))

    # Case 4: a pinned service renamed → flagged.
    renamed = parse_dump(_OK_DUMP.replace("LuminalShineService",
                                          "LuminalShineService2"))
    msgs = check_service_install_names(renamed)
    expect("Renamed pinned service must be flagged",
           any("LuminalShineService" in m for m in msgs))

    # Case 5: pinned Component Guid mutated → flagged.
    mutated = parse_dump(_OK_DUMP.replace(
        "{A3B4C5D6-E7F8-4A5B-9C2D-3E4F5A6B7C8D}",
        "{99999999-9999-9999-9999-999999999999}"))
    msgs = check_component_guids(mutated)
    expect("Mutated pinned Component Guid must be flagged",
           any("StartMenuShortcut" in m and "drift" in m for m in msgs))

    # Case 6: pinned Component missing entirely → flagged.
    dropped = parse_dump(_OK_DUMP.replace(
        "Component=Env_Path|ComponentId={0D8C0E3E-6A7D-48E2-9A1C-0B1A6B7D8C90}|Directory_=IR\n",
        ""))
    msgs = check_component_guids(dropped)
    expect("Missing pinned Component must be flagged",
           any("Env_Path" in m and "missing" in m for m in msgs))

    # Case 7: bootstrapper property missing → flagged.
    no_install_mttvdd = parse_dump(_OK_DUMP.replace(
        "Property=INSTALL_MTTVDD|Value=0\n", ""))
    msgs = check_bootstrapper_properties(no_install_mttvdd)
    expect("Missing bootstrapper-contract property must be flagged",
           any("INSTALL_MTTVDD" in m for m in msgs))

    # Case 8: feature condition present and correct → no failure.
    msgs = check_feature_conditions(tables)
    expect(f"OK feature conditions should pass, got: {msgs}", msgs == [])

    # Case 9: feature condition expression drift → flagged.
    drifted = parse_dump(_OK_DUMP.replace(
        'Feature_=CM_C_mttvdd|Level=0|Condition=INSTALL_MTTVDD <> "1"',
        'Feature_=CM_C_mttvdd|Level=1|Condition=INSTALL_MTTVDD = "1"'))
    msgs = check_feature_conditions(drifted)
    expect("Feature-condition drift must be flagged",
           any("CM_C_mttvdd" in m and "drift" in m for m in msgs))

    # Case 10: feature condition entirely missing → flagged.
    no_sudovda = parse_dump(_OK_DUMP.replace(
        'Feature_=CM_C_sudovda|Level=1|Condition=INSTALL_SUDOVDA = "1"\n',
        ""))
    msgs = check_feature_conditions(no_sudovda)
    expect("Missing pinned feature condition must be flagged",
           any("CM_C_sudovda" in m and "missing" in m for m in msgs))

    if failures:
        print("assert_msi_invariants self-test FAILED:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1

    print(
        f"assert_msi_invariants self-test: OK "
        f"({len(PINNED_SERVICE_INSTALL_NAMES)} services, "
        f"{len(PINNED_COMPONENT_GUIDS)} pinned-Guid components, "
        f"{len(REQUIRED_BOOTSTRAPPER_PROPERTIES)} bootstrapper properties, "
        f"{len(REQUIRED_FEATURE_CONDITIONS)} feature conditions)"
    )
    return 0


# ----------------------------------------------------------------------------
# CLI entry point.
# ----------------------------------------------------------------------------

def main(argv: list[str]) -> int:
    if len(argv) == 2 and argv[1] == "--selftest":
        return _run_selftest()

    if len(argv) != 2:
        print(
            "usage: assert_msi_invariants.py <candidate.txt>\n"
            "       assert_msi_invariants.py --selftest",
            file=sys.stderr,
        )
        return 2

    candidate_path = argv[1]
    try:
        with open(candidate_path, "r", encoding="utf-8") as fh:
            text = fh.read()
    except OSError as exc:
        print(f"cannot read {candidate_path!r}: {exc}", file=sys.stderr)
        return 2

    tables = parse_dump(text)
    failures = (
        check_upgrade_code(tables)
        + check_service_install_names(tables)
        + check_component_guids(tables)
        + check_bootstrapper_properties(tables)
        + check_feature_conditions(tables)
    )

    if failures:
        print(
            f"MSI invariants FAILED ({len(failures)} violation"
            f"{'s' if len(failures) != 1 else ''}):",
            file=sys.stderr,
        )
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1

    print(
        f"MSI invariants: OK ("
        f"UpgradeCode pinned, "
        f"{len(PINNED_SERVICE_INSTALL_NAMES)} services, "
        f"{len(PINNED_COMPONENT_GUIDS)} pinned-Guid components, "
        f"{len(REQUIRED_BOOTSTRAPPER_PROPERTIES)} bootstrapper properties, "
        f"{len(REQUIRED_FEATURE_CONDITIONS)} feature conditions)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
