# MSI compatibility golden snapshots

This directory holds the normalized table dump of the **current shipping
MSI** (WiX 3, CPack-generated) used as the compatibility oracle for the
WiX 3 → WiX 7 migration. Every later migration PR proves it did not drift
the upgrade-critical tables by diffing its freshly built MSI against this
baseline.

## Files

- `wix3_baseline.txt` — normalized dump of the WiX-3 / CPack MSI, captured
  once at the start of the migration. **Must be generated on Windows** (the
  dump uses the `WindowsInstaller.Installer` COM object). Not yet committed
  if this file is absent — see generation step below.

## Regenerating the baseline (Windows, one-time)

```powershell
# 1. Build the current (WiX 3 / CPack) MSI as normal:
cmake -B build -G Ninja -S .
cmake --build build --target package   # or however the MSI is produced in CI

# 2. Dump its tables to the golden file:
pwsh scripts/dump_msi_tables.ps1 `
    -MsiPath build/cpack_artifacts/LuminalShine-*.msi `
    -OutFile tests/fixtures/msi_golden/wix3_baseline.txt

# 3. Eyeball the diff vs. any previous baseline, then commit.
git add tests/fixtures/msi_golden/wix3_baseline.txt
```

The baseline should be regenerated **only** deliberately — e.g. when an
intentional, reviewed change to the upgrade-critical tables lands. A
silent regeneration defeats the purpose of the oracle.

## Checking a candidate MSI against the baseline (Windows CI)

```powershell
pwsh scripts/dump_msi_tables.ps1 -MsiPath <candidate>.msi -OutFile candidate.txt
python scripts/diff_msi_tables.py tests/fixtures/msi_golden/wix3_baseline.txt candidate.txt
```

Exit 0 means the candidate is upgrade-compatible with the baseline
(modulo the volatile fields the differ normalizes: ProductCode,
PackageCode, ProductVersion, and absolute sequence numbers). Exit 1 lists
the offending rows.

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
