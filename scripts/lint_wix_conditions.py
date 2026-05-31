#!/usr/bin/env python3
# Walks the WiX patch fragment that schedules the StopServices /
# KillProcs custom actions and asserts the conditions cover the four
# situations where helper processes and services must be stopped:
#
#   1. Major upgrade install pass (WIX_UPGRADE_DETECTED set)
#   2. Repair / same-version reinstall (Installed AND NOT REMOVE)
#   3. Manual uninstall (REMOVE="ALL" AND NOT UPGRADINGPRODUCTCODE)
#
# Plus checks that the PowerShell command bodies for the stop / start
# service actions list every known LuminalShine service. This catches
# the class of regression that 26.05.0-rc4 shipped: a new service was
# authored into the MSI (LuminalShineSessionMonitor) but not added to
# the proactive PowerShell stop list, so upgrades hit file-in-use on
# the binary that service held mapped.
#
# Run standalone for debugging:
#   python3 scripts/lint_wix_conditions.py
#
# Returns exit 0 on pass, 1 on failure with a diagnostic written to
# stderr. Designed to be wired in as a CTest test via CMake.

from __future__ import annotations

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
# PATCH_FILE used to point at patch_custom_actions.wxs (the CPack-WIX
# patch fragment) which contained the InstallExecuteSequence
# customizations. PR2.c.1 folded that file into luminalshine.wxs and
# PR2.d's final iteration deleted it; the conditions now live in the
# top-level product wxs.
PATCH_FILE = REPO_ROOT / "packaging" / "windows" / "wix" / "luminalshine.wxs"
CUSTOM_ACTIONS_FILE = REPO_ROOT / "packaging" / "windows" / "wix" / "custom_actions.wxs"

# Custom action IDs whose conditions are load-bearing for the "stop
# everything that holds binaries mapped before InstallFiles runs" flow.
# Both immediate (pre-InstallValidate) and deferred (post-InstallInitialize)
# variants must cover the same three trigger cases.
STOP_KILL_ACTION_IDS = (
    "SetKillProcsQuietImmediate",
    "KillProcsQuietImmediate",
    "SetStopSvcQuietImmediate",
    "StopSvcQuietImmediate",
    "SetStopSvcQuiet",
    "StopSvcQuiet",
)

# Service-name tokens that must appear inside the PowerShell foreach()
# loops in the SetStopSvcQuiet* / SetStartSvcQuiet bodies. These are the
# Windows service short names — keep in sync with the ServiceInstall
# Name attributes in custom_actions.wxs. SunshineService and sunshinesvc
# are legacy short names left in place to handle upgrades from pre-rename
# Vibeshine / Sunshine installs.
REQUIRED_SERVICE_NAMES = (
    "SunshineService",
    "LuminalShineService",
    "LuminalShineXboxBtHelper",
    "LuminalShineSessionMonitor",
)

# Setter action IDs whose Value attribute holds the PowerShell command
# line that gets handed to QuietExec. Each of these has to enumerate
# every service name in REQUIRED_SERVICE_NAMES.
SERVICE_LIST_SETTER_IDS = (
    "SetStopSvcQuietImmediate",
    "SetStopSvcQuiet",
    "SetStartSvcQuiet",
)


def find_namespace(tree: ET.ElementTree) -> str:
    # ElementTree exposes namespaced tags as "{uri}local". Sniff the root
    # element to find what URI WiX is actually using so we don't hardcode
    # the schema version (3.x vs 4.x differ).
    root = tree.getroot()
    match = re.match(r"\{(?P<uri>[^}]+)\}", root.tag)
    return match.group("uri") if match else ""


def custom_action_conditions(tree: ET.ElementTree, ns: str) -> dict[str, str]:
    # patch_custom_actions.wxs uses a CPackWiXPatch fragment, not a real
    # WiX schema. The <Custom> elements live under <InstallExecuteSequence>
    # without a namespace, so the namespace sniff above will be empty and
    # the iteration below picks them up directly. The function is generic
    # so the lint stays correct if the file is ever folded into a proper
    # WiX-namespaced schema.
    prefix = f"{{{ns}}}" if ns else ""
    sequence = tree.find(f".//{prefix}InstallExecuteSequence")
    if sequence is None:
        # Fall back to a no-namespace search for the CPackWiXPatch case.
        sequence = tree.find(".//InstallExecuteSequence")
    if sequence is None:
        raise SystemExit("no InstallExecuteSequence element found")

    conditions: dict[str, str] = {}
    for elem in sequence:
        tag = elem.tag.split("}")[-1]
        if tag != "Custom":
            continue
        action = elem.get("Action")
        if not action:
            continue
        # WiX 4+ encodes the <Custom> condition as a Condition=
        # attribute. WiX 3 put it in element text; we still fall
        # through to that form so a regression that accidentally
        # reverts to the v3 style doesn't go silently unlinted.
        condition_text = (elem.get("Condition") or "").strip()
        if not condition_text:
            condition_text = (elem.text or "").strip()
        conditions[action] = condition_text
    return conditions


def check_upgrade_coverage(action_id: str, condition: str) -> list[str]:
    failures: list[str] = []
    if "WIX_UPGRADE_DETECTED" not in condition:
        failures.append(
            f"{action_id}: condition does not include WIX_UPGRADE_DETECTED, "
            "so the action won't fire during the install pass of a major "
            "upgrade and helper processes will keep the old binaries "
            f"mapped. Got: {condition!r}"
        )
    if "Installed AND NOT REMOVE" not in condition:
        failures.append(
            f"{action_id}: condition does not cover repair / same-version "
            f"reinstall (Installed AND NOT REMOVE). Got: {condition!r}"
        )
    if 'REMOVE = "ALL"' not in condition and 'REMOVE="ALL"' not in condition:
        failures.append(
            f"{action_id}: condition does not cover manual uninstall "
            f'(REMOVE = "ALL"). Got: {condition!r}'
        )
    return failures


def setter_command_lines(tree: ET.ElementTree, ns: str) -> dict[str, str]:
    prefix = f"{{{ns}}}" if ns else ""
    fragments = tree.findall(f".//{prefix}Fragment")
    if not fragments:
        fragments = tree.findall(".//Fragment")

    values: dict[str, str] = {}
    for fragment in fragments:
        for elem in fragment.iter():
            tag = elem.tag.split("}")[-1]
            if tag != "CustomAction":
                continue
            action_id = elem.get("Id")
            value = elem.get("Value")
            if action_id and value:
                values[action_id] = value
    return values


def check_service_list_coverage(action_id: str, value: str) -> list[str]:
    failures: list[str] = []
    for svc in REQUIRED_SERVICE_NAMES:
        if svc not in value:
            failures.append(
                f"{action_id}: PowerShell body does not reference the "
                f"{svc!r} service. The service will keep running through "
                "an upgrade and hold its binary mapped, causing "
                "InstallFiles to fail with file-in-use."
            )
    return failures


def main() -> int:
    if not PATCH_FILE.exists():
        print(f"missing: {PATCH_FILE}", file=sys.stderr)
        return 1
    if not CUSTOM_ACTIONS_FILE.exists():
        print(f"missing: {CUSTOM_ACTIONS_FILE}", file=sys.stderr)
        return 1

    patch_tree = ET.parse(PATCH_FILE)
    patch_ns = find_namespace(patch_tree)
    conditions = custom_action_conditions(patch_tree, patch_ns)

    failures: list[str] = []

    for action_id in STOP_KILL_ACTION_IDS:
        if action_id not in conditions:
            failures.append(
                f"{action_id}: not scheduled in InstallExecuteSequence of "
                f"{PATCH_FILE.name}"
            )
            continue
        failures.extend(check_upgrade_coverage(action_id, conditions[action_id]))

    ca_tree = ET.parse(CUSTOM_ACTIONS_FILE)
    ca_ns = find_namespace(ca_tree)
    setter_values = setter_command_lines(ca_tree, ca_ns)

    for action_id in SERVICE_LIST_SETTER_IDS:
        if action_id not in setter_values:
            failures.append(
                f"{action_id}: no CustomAction with this Id found in "
                f"{CUSTOM_ACTIONS_FILE.name}"
            )
            continue
        failures.extend(check_service_list_coverage(action_id, setter_values[action_id]))

    if failures:
        print(
            f"WiX condition lint failed ({len(failures)} issue"
            f"{'s' if len(failures) != 1 else ''}):",
            file=sys.stderr,
        )
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(
        f"WiX condition lint: OK "
        f"({len(STOP_KILL_ACTION_IDS)} actions, "
        f"{len(REQUIRED_SERVICE_NAMES)} services, "
        f"{len(SERVICE_LIST_SETTER_IDS)} setters checked)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
