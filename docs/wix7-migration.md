# WiX 3 → WiX 7 migration plan

This is the live tracking doc for the WiX migration. Status updated as PRs land.

## Why migrate

WiX 3.14 is in deep maintenance (security fixes only, no new features). The
project is moving to WiX 7+ for active support and the modern `wix` CLI / NuGet
extension model. WiX 4+ also drops the dependency on CMake's CPack-WIX generator
— which is itself hardwired to v3 and has no upstream v4+ replacement — so this
migration also resolves a stagnant build-tooling dependency.

## Constraints

These are the **invariants** every PR in the chain must preserve:

- **The bootstrapper design does not change.** `LuminalShineSetup.exe` /
  `uninstall.exe` (the C# / Roslyn binaries built by
  [packaging/windows/bootstrapper/build_bootstrapper.ps1](../packaging/windows/bootstrapper/build_bootstrapper.ps1))
  continue to embed an `MsiPayload` resource and spawn `msiexec.exe`. Only the
  *MSI authoring tooling* migrates.

- **The bootstrapper ↔ MSI property contract stays identical.** The bootstrapper
  passes these properties to `msiexec` and the new MSI must honor every one with
  the same semantics:
  `INSTALL_ROOT`, `INSTALL_MTTVDD`, `INSTALL_SUDOVDA`, `REMOVEVIRTUALDISPLAYDRIVER`,
  `KEEPADMINCREDENTIALS`, `KEEPSESSIONDATA`, `DELETEINSTALLDIR`,
  `SKIP_REMOVE_CONFLICTING_PRODUCTS`, `REBOOT=ReallySuppress`, `SUPPRESSMSGBOXES=1`,
  and the `/qn` quiet flow.

- **Upgrade-critical MSI identifiers stay byte-identical** so existing installs
  upgrade cleanly:
  - `UpgradeCode` = `{C2C36624-2D9C-4AFD-9C79-6B7861AE4A0D}`
  - `ProductName` "LuminalShine", `Manufacturer` "NortheBridge Foundation"
  - `ServiceInstall` short names: `LuminalShineService`, `LuminalShineXboxBtHelper`,
    `LuminalShineSessionMonitor` (any rename detaches SCM state).
  - Every hand-authored Component GUID in `packaging/windows/wix/custom_actions.wxs`
    (services, shortcuts, env, firewall, ARP, SudoVDA registry defaults).
  - `INSTALL_ROOT` resolving to `C:\Program Files\NortheBridge\LuminalShine\`.
  - All `CustomAction` / `InstallExecuteSequence` condition strings, including
    the `WIX_UPGRADE_DETECTED OR ...` stop/kill conditions that the
    upgrade-doesn't-strand-services fix landed in
    [#27](https://github.com/NortheBridge/luminalshine/pull/27).

- **Security model preserved end-to-end.** Bootstrapper manifest stays
  `requireAdministrator`; the `harden_config_directory` DACL on
  `%ProgramData%\LuminalShine\config\` (System integrity, Users read-only)
  stays as-is — both are outside the WiX authoring and unaffected by tooling
  changes. The MSI `Privileged` launch condition ports verbatim to v7 syntax.

The above invariants are enforced by the **MSI table compatibility oracle**
([scripts/dump_msi_tables.ps1](../scripts/dump_msi_tables.ps1) and
[scripts/diff_msi_tables.py](../scripts/diff_msi_tables.py)) which diffs every
candidate MSI against the committed golden
([tests/fixtures/msi_golden/wix3_baseline.txt](../tests/fixtures/msi_golden/wix3_baseline.txt)).
Volatile fields (`ProductCode`, `PackageCode`, `ProductVersion`, absolute
`Sequence` numbers — relative order still enforced) are normalized away; the
rest is byte-checked.

## PR sequence (current)

The original five-PR plan collapsed PR 3+4 into a single cutover after deciding
not to maintain WiX 3 as a long-term fallback. The opt-in / parallel-build phase
existed purely as a hedge against v7 bugs; with no near-term release pressure
and the oracle as the verification harness regardless, the parallel phase added
CI cost without buying meaningful safety.

| Step | Status | Branch / PR | Scope |
|------|--------|-------------|-------|
| **PR 1** — MSI table compatibility oracle | ✅ merged | [#26](https://github.com/NortheBridge/luminalshine/pull/26) (`8f14a61a`) | Dumper (PowerShell), differ (Python, with selftest), golden baseline captured from a clean WiX 3 build, CI compat-check step (warn-only until golden committed, hard-gate in PR 4) |
| **PR 2** — Decouple from CPack WiX generator (stay on WiX 3) | 🟡 iterating | [#28](https://github.com/NortheBridge/luminalshine/pull/28) (draft) | Hand-authored `luminalshine.wxs` + `features.json` manifest + `gen_wix_files.py` (file Components + Features + bindings, byte-equivalent to CPack output via `Guid="*"` + replicated ID conventions). CMake drives `candle.exe` + `light.exe` directly. CPack reduced to ZIP-only. WiX 3 schema unchanged — this PR proves the build is right before the schema migrates. |
| **PR 3** — Cut over to WiX 7 (single step) | ⏸️ pending | — | **Collapsed from old PR 3 + PR 4.** Add WiX 7 toolchain via repo-pinned `.config/dotnet-tools.json` (already present), translate `luminalshine.wxs` + `custom_actions.wxs` + the generator output schema (namespace `http://wixtoolset.org/schemas/v4/wxs`, `<Product>`→`<Package>`, `<Directory ProgramFiles64Folder>`→`<StandardDirectory>`, `Win64="yes"`→`Bitness="always64"`, `<Condition>`→`<Launch Condition>`, `util:` prefix on QuietExec actions). Replace `candle.exe` + `light.exe` invocations in `cmake/packaging/windows_wix.cmake` with a single `wix build` call. Delete WiX 3 toolchain references. Iterate on Windows CI until the oracle diffs clean. Real-Windows install matrix (rcN → next-build upgrade, legacy Sunshine → next-build, fresh install, manual uninstall/reinstall) is the merge gate. |
| **PR 4** — Permanent CI guardrails + lint v7 port | ⏸️ pending | — | Promote the table-diff oracle to a hard CI gate on `UpgradeCode`, `ServiceInstall` names, and pinned Component GUIDs. Port the existing `wix_condition_lint` + ProgramMenuFolder-keypath lint to the v7 schema/namespace. Final docs pass (this file, [docs/building.md](building.md), [CLAUDE.md](../CLAUDE.md) build notes). |

### Why the collapse is safe

The opt-in / parallel-build phase in the original plan existed to provide a
fallback if WiX 7's output had bugs — letting CI build both and the maintainer
ship the WiX 3 MSI while iterating on v7. Two facts make that hedge unnecessary
here:

1. **LuminalShine is in early-stage RC and will remain so for some time** — no
   release pressure forces a fallback path.
2. **The oracle (PR 1) is the verification harness regardless of how many
   transition steps exist.** Whether v7 lands in one cutover or two, the diff
   against the golden is what proves correctness. The parallel phase didn't
   change *what* gets verified, only *when*.

The cost the parallel phase did carry was real — doubled CI time per push, two
CMake paths to maintain, more workflow infrastructure to author and tear down.
Collapsing trades that cost for a single (still iteratively verifiable on CI)
cutover.

## PR 2 internal cadence (notes for future similar PRs)

PR 2 is being landed as sequential commits within the same draft PR so each
sub-step is locally verifiable before the next is authored. Same cadence is
recommended for PR 3.

- **PR 2.a** — `scripts/gen_wix_files.py` file-component generator + offline
  selftest against the golden's Component IDs. Locally verifiable; landed first
  as the byte-exact-reproducibility foundation.
- **PR 2.b** — `packaging/windows/wix/features.json` manifest + generator
  extension to emit Features + FeatureComponents. Selftest validates against
  the golden's Feature table.
- **PR 2.c.1** — Hand-authored `packaging/windows/wix/luminalshine.wxs`,
  additive only (CMake still using CPack-WIX). Folds the WiX-3-style
  `WIX.template.in` + `patch_custom_actions.wxs` into one product wxs.
- **PR 2.c.2** — CMake plumbing cutover. `windows_wix.cmake` rewritten to drive
  candle/light directly via `cmake --install` staging → generator → candle →
  light. `package_msi` target preserved by name. CI workflow points at the
  new target.
- **PR 2.d** — Windows CI iteration until `diff_msi_tables.py` against the
  golden reports clean. Each iteration produces a small targeted fix:
  - iter1: `<Condition>` inside `<FeatureRef>` → moved into generator (CNDL0005).
  - iter2: `InstallScope="perMachine"` duplicated explicit `<Property ALLUSERS=1>`
    → dropped `InstallScope`, matching CPack's `CPACK_WIX_INSTALL_SCOPE=NONE`
    pattern (LGHT0091).
  - iter3+: TBD.

Once PR 2.d's oracle diff is clean, the dead WiX-3-CPack files
(`WIX.template.in`, `patch_custom_actions.wxs`) get deleted in the final commit
on the branch.

## What lives outside this migration

These were caught during the migration but are tracked separately:

- **Verify whether the "Reset LuminalShine Admin Password" Start Menu shortcut
  is actually missing on Windows installs**, and if so why. The original report
  prompted a misguided keypath fix
  ([#27](https://github.com/NortheBridge/luminalshine/pull/27) reverted it) that
  exposed it was based on an unverified hypothesis — ICE38/43 require the HKCU
  keypath we have. The real cause, if any, needs on-Windows verification of
  `$env:ProgramData\Microsoft\Windows\Start Menu\Programs\LuminalShine\Reset LuminalShine Admin Password.lnk`
  after a clean install vs. an upgrade.
- **`common lint` workflow `startup_failure`** on PRs is a pre-existing
  repo-wide issue (also fails on unrelated branches), not migration fallout.
- **Add the `Condition` MSI table to `scripts/dump_msi_tables.ps1`'s
  `$TablesToDump`** so the oracle validates feature-level Conditions
  (currently emitted by the generator but not diffed against golden).
