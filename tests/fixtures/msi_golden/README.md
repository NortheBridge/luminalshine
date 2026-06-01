# MSI compatibility golden snapshot

This directory holds the normalized table dump of the **current shipping
MSI** used as the compatibility oracle for the build. Every CI run proves
the freshly built MSI did not drift the upgrade-critical tables by diffing
it against this baseline.

## Files

- `msi_baseline.txt` — normalized dump of the current MSI. **Must be
  generated on Windows** (the dump uses the `WindowsInstaller.Installer`
  COM object).

  History: this file was originally `wix3_baseline.txt` and captured the
  WiX-3 / CPack-WIX MSI at the start of the WiX 7 migration. The WiX 7
  cutover (PR 3 of the migration) deliberately re-baselined it: the
  upgrade-critical content (UpgradeCode, `ServiceInstall` names,
  hand-authored Component GUIDs, sequence ordering, bootstrapper-contract
  properties) is byte-identical between the two, but toolchain
  fingerprints differ (`Wix4UtilCA_X64` vs `WixCA`, `Wix5FWCA_X64` vs
  `WixFirewallCA`, auto-generated 8.3 short names, auto-generated
  registry `KeyPath` IDs, ComponentIds for `Guid="*"` components
  rederived from those KeyPaths). The renamed file makes it clear the
  baseline is a moving snapshot tied to whatever toolchain is current,
  not a frozen artifact of the WiX 3 era.

## Regenerating the baseline (Windows, one-time per deliberate change)

```powershell
# 1. Build the current MSI as normal:
cmake -B build -G Ninja -S .
cmake --build build --target luminalshine_msi

# 2. Dump its tables to the golden file:
pwsh scripts/dump_msi_tables.ps1 `
    -MsiPath build/cpack_artifacts/LuminalShine-*.msi `
    -OutFile tests/fixtures/msi_golden/msi_baseline.txt

# 3. Eyeball the diff vs. the previous baseline, then commit.
git add tests/fixtures/msi_golden/msi_baseline.txt
```

The baseline should be regenerated **only** deliberately — e.g. when an
intentional, reviewed change to the upgrade-critical tables lands, or
when the toolchain itself produces a new (still upgrade-compatible)
fingerprint. A silent regeneration defeats the purpose of the oracle.

## Checking a candidate MSI against the baseline (Windows CI)

```powershell
pwsh scripts/dump_msi_tables.ps1 -MsiPath <candidate>.msi -OutFile candidate.txt
python scripts/diff_msi_tables.py tests/fixtures/msi_golden/msi_baseline.txt candidate.txt
```

Exit 0 means the candidate is upgrade-compatible with the baseline
(modulo the volatile fields the differ normalizes: `ProductCode`,
`PackageCode`, `ProductVersion`, and absolute sequence numbers). Exit 1
lists the offending rows.

## What the differ normalizes vs. enforces

Normalized away (legitimately changes every build): `ProductCode`,
`PackageCode`, `ProductVersion`, and absolute `Sequence` numbers in
`InstallExecuteSequence` / `InstallUISequence` (relative action order is
still enforced).

Enforced (must stay byte-identical): `UpgradeCode`, the `Upgrade` table,
Component GUIDs / KeyPaths / Directories, `ServiceInstall` and
`ServiceControl` names and attributes, `Shortcut` targets, `CustomAction`
definitions, the relative ordering of sequenced actions, and the
`INSTALL_*` / `KEEP*` / `*VIRTUALDISPLAYDRIVER` bootstrapper-contract
properties.
