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
    "INSTALL_SUDOVDA",
    "REMOVEVIRTUALDISPLAYDRIVER",
    "SKIP_REMOVE_CONFLICTING_PRODUCTS",
})

# Feature-level Conditions that gate the virtual-display backend on
# the bootstrapper-supplied INSTALL_SUDOVDA property.
# The condition's Level value flips the feature between install (Level=1)
# and uninstall (Level=0). Without these rows, the deferred install.ps1
# custom actions could fire against files that were never staged. The
# expression strings come from packaging/windows/wix/features.json.
REQUIRED_FEATURE_CONDITIONS = {
    "CM_C_sudovda": ("1", 'INSTALL_SUDOVDA = "1"'),
}

# InstallExecuteSequence-level gating for credential-destroying actions.
# Each entry maps a Custom Action's Id to the list of substrings its
# scheduling Condition must contain. The Condition strings are compared
# with whitespace+quote normalisation (so single-quoted vs double-quoted
# vs spacing variants all pass), but every listed substring must appear.
#
# Why these are pinned:
# `ResetAdminCredentials` runs `luminalshine.exe --reset-admin-credentials`
# during uninstall, which deletes the WCM entry AND the TPM-bound
# wrapping key (tpm_seal::clear) — destroying the admin login.
# Leaving even one clause off the gate exposes users to credential loss
# on a routine upgrade. Specifically:
#   - REMOVE="ALL"            — only fire on a real uninstall, not
#                                 on file-level repair or feature change.
#   - NOT UPGRADINGPRODUCTCODE — DO NOT fire during the uninstall pass
#                                 of a major upgrade (where MSI runs the
#                                 old MSI's uninstall as a nested step
#                                 inside the new install's transaction).
#                                 Skipping this clause was the exact bug
#                                 mechanism investigated in the
#                                 "credentials lost on upgrade" thread.
#   - KEEPADMINCREDENTIALS<>"1" — bootstrapper / CLI escape hatch for
#                                 advanced operators who want a full
#                                 uninstall without losing the WCM entry.
PINNED_RESET_ADMIN_CONDITIONS = ('REMOVE="ALL"', 'NOT UPGRADINGPRODUCTCODE',
                                 'KEEPADMINCREDENTIALS<>"1"')
REQUIRED_CUSTOM_ACTION_CONDITIONS: "dict[str, tuple[str, ...]]" = {
    "SetResetAdminCredentials": PINNED_RESET_ADMIN_CONDITIONS,
    "ResetAdminCredentials": PINNED_RESET_ADMIN_CONDITIONS,
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
    """Each bootstrapper-contract property must be marked Secure (i.e.
    appear in SecureCustomProperties). Without Secure="yes" on the
    <Property/> declaration, msiexec silently drops the property when
    the bootstrapper passes it on the command line — exactly the
    failure mode we're guarding against."""
    failures: list[str] = []
    rows = tables.get("Property", [])
    secure_value = ""
    for row in rows:
        if row.get("Property") == "SecureCustomProperties":
            secure_value = row.get("Value", "")
            break
    secure_set = set(secure_value.split(";")) if secure_value else set()
    for required in REQUIRED_BOOTSTRAPPER_PROPERTIES:
        if required not in secure_set:
            failures.append(
                f"Property {required!r} (bootstrapper-contract) is not "
                "in SecureCustomProperties. Without Secure=\"yes\" on the "
                "<Property/> declaration, msiexec silently drops it when "
                "the bootstrapper passes it on the command line."
            )
    return failures


def _normalize_condition(expr: str) -> str:
    """Collapse whitespace and strip spaces around relational operators
    so substring matching against the pinned clauses survives benign
    formatting variation between WiX 3 and WiX 7 emitters. Quote style
    (single vs double) is preserved — the pinned substrings use double
    quotes because that's what every MSI Condition in this repo emits
    today, and MSI's own parser treats them identically anyway."""
    out = " ".join(expr.split())
    for op in ('<>', '=', '<=', '>=', '<', '>'):
        # avoid double-rewriting when an op is a prefix of a longer one;
        # handled implicitly by ordering longer ops first above.
        out = out.replace(f" {op} ", op)
    return out


def check_custom_action_conditions(tables) -> list[str]:
    """Every pinned credential-destroying custom action must be scheduled
    with every required gating clause in its InstallExecuteSequence
    Condition. Catches the regression where a future MSI edit silently
    drops one of the gates (e.g. NOT UPGRADINGPRODUCTCODE), turning a
    routine major-upgrade pass into a credential-erasing event."""
    failures: list[str] = []
    rows = tables.get("InstallExecuteSequence", [])
    by_action: dict[str, str] = {}
    for row in rows:
        action = row.get("Action", "")
        if action:
            by_action[action] = row.get("Condition", "")
    for action_id, required_clauses in REQUIRED_CUSTOM_ACTION_CONDITIONS.items():
        if action_id not in by_action:
            failures.append(
                f"InstallExecuteSequence is missing required row for "
                f"Action={action_id!r}. The credential-destroying custom "
                "action must be scheduled with its full gating Condition; "
                "removing the row defeats the gate entirely."
            )
            continue
        normalized = _normalize_condition(by_action[action_id])
        normalized_required = [_normalize_condition(c) for c in required_clauses]
        missing = [orig for orig, norm in zip(required_clauses, normalized_required)
                   if norm not in normalized]
        if missing:
            failures.append(
                f"Condition on Action={action_id!r} is missing required "
                f"gate clause(s) {missing!r}. Without all three of "
                f"{list(required_clauses)!r}, the action can fire on a "
                "routine upgrade and destroy the user's saved admin "
                f"credential. Observed: {by_action[action_id]!r}"
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
Property=SecureCustomProperties|Value=INSTALL_ROOT;INSTALL_SUDOVDA;REMOVEVIRTUALDISPLAYDRIVER;SKIP_REMOVE_CONFLICTING_PRODUCTS

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
Feature_=CM_C_sudovda|Level=1|Condition=INSTALL_SUDOVDA = "1"

## TABLE: InstallExecuteSequence
Action=SetResetAdminCredentials|Condition=REMOVE="ALL" AND NOT UPGRADINGPRODUCTCODE AND KEEPADMINCREDENTIALS<>"1"|Sequence=3700
Action=ResetAdminCredentials|Condition=REMOVE="ALL" AND NOT UPGRADINGPRODUCTCODE AND KEEPADMINCREDENTIALS<>"1"|Sequence=3705
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

    # Case 7: bootstrapper property dropped from SecureCustomProperties → flagged.
    no_install_sudovda = parse_dump(_OK_DUMP.replace(
        "INSTALL_SUDOVDA;", ""))
    msgs = check_bootstrapper_properties(no_install_sudovda)
    expect("Missing bootstrapper-contract property must be flagged",
           any("INSTALL_SUDOVDA" in m for m in msgs))

    # Case 8: feature condition present and correct → no failure.
    msgs = check_feature_conditions(tables)
    expect(f"OK feature conditions should pass, got: {msgs}", msgs == [])

    # Case 9: feature condition expression drift → flagged.
    drifted = parse_dump(_OK_DUMP.replace(
        'Feature_=CM_C_sudovda|Level=1|Condition=INSTALL_SUDOVDA = "1"',
        'Feature_=CM_C_sudovda|Level=0|Condition=INSTALL_SUDOVDA <> "1"'))
    msgs = check_feature_conditions(drifted)
    expect("Feature-condition drift must be flagged",
           any("CM_C_sudovda" in m and "drift" in m for m in msgs))

    # Case 10: feature condition entirely missing → flagged.
    no_sudovda = parse_dump(_OK_DUMP.replace(
        'Feature_=CM_C_sudovda|Level=1|Condition=INSTALL_SUDOVDA = "1"\n',
        ""))
    msgs = check_feature_conditions(no_sudovda)
    expect("Missing pinned feature condition must be flagged",
           any("CM_C_sudovda" in m and "missing" in m for m in msgs))

    # Case 11: pinned ResetAdminCredentials gating present → no failure.
    msgs = check_custom_action_conditions(tables)
    expect(f"OK custom-action gating should pass, got: {msgs}", msgs == [])

    # Case 12: ResetAdminCredentials missing the UPGRADINGPRODUCTCODE
    # exclusion → flagged (this is the exact "credentials lost on
    # upgrade" failure mode investigated upstream).
    no_upgrade_guard = parse_dump(_OK_DUMP.replace(
        ' AND NOT UPGRADINGPRODUCTCODE', '', 2))  # both rows
    msgs = check_custom_action_conditions(no_upgrade_guard)
    expect("Missing NOT UPGRADINGPRODUCTCODE on ResetAdminCredentials must be flagged",
           any("ResetAdminCredentials" in m and "UPGRADINGPRODUCTCODE" in m
               for m in msgs))

    # Case 13: KEEPADMINCREDENTIALS escape hatch silently removed → flagged.
    no_keep = parse_dump(_OK_DUMP.replace(
        ' AND KEEPADMINCREDENTIALS<>"1"', '', 2))
    msgs = check_custom_action_conditions(no_keep)
    expect("Missing KEEPADMINCREDENTIALS clause on ResetAdminCredentials must be flagged",
           any("KEEPADMINCREDENTIALS" in m for m in msgs))

    # Case 14: REMOVE="ALL" gate dropped (action would fire on patches
    # / repairs that aren't full uninstalls) → flagged.
    no_remove = parse_dump(_OK_DUMP.replace(
        'REMOVE="ALL" AND ', '', 2))
    msgs = check_custom_action_conditions(no_remove)
    expect("Missing REMOVE=\"ALL\" clause must be flagged",
           any('REMOVE="ALL"' in m for m in msgs))

    # Case 15: action row entirely missing from IES → flagged (without
    # this we'd silently let a future MSI edit drop the action and pass
    # the gate check vacuously).
    dropped_row = parse_dump(_OK_DUMP.replace(
        'Action=ResetAdminCredentials|Condition=REMOVE="ALL" AND NOT UPGRADINGPRODUCTCODE AND KEEPADMINCREDENTIALS<>"1"|Sequence=3705\n',
        ""))
    msgs = check_custom_action_conditions(dropped_row)
    expect("Missing ResetAdminCredentials IES row must be flagged",
           any("ResetAdminCredentials" in m and "missing" in m for m in msgs))

    # Case 16: whitespace + quote variation in a valid condition should
    # still pass (normalisation is the point of _normalize_condition).
    spaced = parse_dump(_OK_DUMP.replace(
        'REMOVE="ALL" AND NOT UPGRADINGPRODUCTCODE AND KEEPADMINCREDENTIALS<>"1"',
        'REMOVE = "ALL"   AND   NOT UPGRADINGPRODUCTCODE  AND  KEEPADMINCREDENTIALS  <>  "1"',
        2))
    msgs = check_custom_action_conditions(spaced)
    expect(f"Whitespace-varied valid condition must pass, got: {msgs}",
           msgs == [])

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
        f"{len(REQUIRED_FEATURE_CONDITIONS)} feature conditions, "
        f"{len(REQUIRED_CUSTOM_ACTION_CONDITIONS)} credential-destroying CAs)"
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
        + check_custom_action_conditions(tables)
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
        f"{len(REQUIRED_FEATURE_CONDITIONS)} feature conditions, "
        f"{len(REQUIRED_CUSTOM_ACTION_CONDITIONS)} credential-destroying CAs)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
