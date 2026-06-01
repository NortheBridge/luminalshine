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
([tests/fixtures/msi_golden/msi_baseline.txt](../tests/fixtures/msi_golden/msi_baseline.txt)).
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
| **PR 2** — Decouple from CPack WiX generator (stay on WiX 3) | ✅ merged | [#28](https://github.com/NortheBridge/luminalshine/pull/28) (`b0e5bccf`) | Hand-authored `luminalshine.wxs` + `features.json` manifest + `gen_wix_files.py` (file Components + Features + bindings, byte-equivalent to CPack output via `Guid="*"` + replicated ID conventions). CMake drives `candle.exe` + `light.exe` directly. CPack reduced to ZIP-only. WiX 3 schema unchanged — this PR proved the build was right before the schema migrated. |
| **PR 3** — Cut over to WiX 7 (single step) | 🟡 iterating | — (`wix7-migration-pr3`) | **Collapsed from old PR 3 + PR 4.** WiX 7 toolchain pinned in `.config/dotnet-tools.json`. `luminalshine.wxs` + `custom_actions.wxs` + the generator-emitted `files.wxs` / `features.wxs` translated to v4 namespace (`http://wixtoolset.org/schemas/v4/wxs`): `<Product>`→`<Package>` merged, `<Directory ProgramFiles64Folder>`→`<StandardDirectory>`, `Win64="yes"`→`Bitness="always64"`, top-level `<Condition>`→`<Launch>`, `<Custom>` condition-text→`Condition=` attribute, `Feature Absent="disallow"`→`AllowAbsent="no"`, feature-level `<Condition Level=>`→`<Level Value= Condition=>`, `BinaryKey="WixCA"`→`BinaryRef="Wix4UtilCA_X64"` (util extension's renamed CA binary; entry-point names unchanged), `<UIRef>`→`<ui:WixUI>`. `windows_wix.cmake` rewritten to drive `dotnet wix build` directly; the WiX 3 candle/light path is gone. CI workflow installs the v7 toolchain via `dotnet tool restore` + `dotnet wix extension add` (UI / Util / Firewall). Iterating on Windows CI until the oracle diffs clean. Real-Windows install matrix (rcN → next-build upgrade, legacy Sunshine → next-build, fresh install, manual uninstall/reinstall) is the merge gate. |
| **PR 4** — Permanent CI guardrails | ⏸️ pending | — | Promote the table-diff oracle to a hard CI gate on `UpgradeCode`, `ServiceInstall` names, and pinned Component GUIDs (currently warn-only). Add the `Condition` MSI table to `scripts/dump_msi_tables.ps1`'s `$TablesToDump` so feature-level Conditions are diffed. Final docs pass ([docs/building.md](building.md), [CLAUDE.md](../CLAUDE.md) build notes). |

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

## PR 2 cadence (kept for reference)

PR 2 landed as sequential commits within a single draft PR so each sub-step
was locally verifiable before the next was authored. Same cadence is being
followed for PR 3.

- **PR 2.a** — `scripts/gen_wix_files.py` file-component generator + offline
  selftest against the golden's Component IDs.
- **PR 2.b** — `packaging/windows/wix/features.json` manifest + generator
  extension to emit Features + FeatureComponents.
- **PR 2.c.1** — Hand-authored `packaging/windows/wix/luminalshine.wxs`,
  folding the WiX-3-style `WIX.template.in` + `patch_custom_actions.wxs` into
  one product wxs (additive only, CPack-WIX still in the loop).
- **PR 2.c.2** — `windows_wix.cmake` rewritten to drive candle/light directly
  via `cmake --install` staging → generator → candle → light. CPack-WIX
  removed; CI workflow points at the new target.
- **PR 2.d** — Windows CI iteration until `diff_msi_tables.py` reported clean
  (10 iterations: CNDL0005 / LGHT0091 / Directory subtree per install
  component / Feature Attributes 16 vs 24 / WixUI_FeatureTree vs InstallDir /
  ARP* properties / LGHT0094 sequence-anchor / sequence position /
  condition operator whitespace).
- **PR 2.final** — Dead WiX-3-CPack files (`WIX.template.in`,
  `patch_custom_actions.wxs`) deleted; `lint_wix_conditions.py` `PATCH_FILE`
  redirected to `luminalshine.wxs`.

## PR 3 internal cadence

- **PR 3.a** — `.config/dotnet-tools.json` populated with WiX 7 pin (file
  pre-existed at repo root from `60bb2c79` but was empty and in the wrong
  location for `dotnet tool restore` auto-discovery; moved to `.config/`).
- **PR 3.b** — `packaging/windows/wix/luminalshine.wxs` translated to v4
  namespace in place.
- **PR 3.c** — `packaging/windows/wix/custom_actions.wxs` translated.
- **PR 3.d** — `scripts/gen_wix_files.py` updated to emit v4-namespace
  fragments (`Bitness="always64"`, `AllowAbsent="no"`, `<Level Value= Condition=>`).
  Offline selftest still passes (it validates compiled-table-row values, not
  XML schema).
- **PR 3.e** — `cmake/packaging/windows_wix.cmake` rewritten to drive
  `dotnet wix build` instead of candle/light. `dotnet tool restore` runs at
  configure time; WiX extensions install on first build.
- **PR 3.f** — CI workflow: `choco install wixtoolset` replaced with
  `dotnet tool restore` + `dotnet wix extension add` for UI / Util / Firewall.
  Diagnostic-dump-on-failure step pivoted from CPack-WIX wix.log to the
  generator's wix_gen output.
- **PR 3.g** — Windows CI iteration until `diff_msi_tables.py` reports clean.
  Seven iterations on the branch:
  - **iter1** — `choco install wixtoolset` removed (WiX 7 is a dotnet tool, not
    chocolatey); the first build surfaced WIX7015 — the Open Source
    Maintenance Fee EULA gate WiX 7 ships with.
  - **iter2** — first attempt with `WIX_ACCEPT_OSMF_EULA=1` env var (wrong;
    the WiX CLI ignores it).
  - **iter3** — switched to `dotnet wix --accept-eula` (also wrong; that flag
    doesn't exist).
  - **iter4** — final EULA fix: the WiX CLI's actual form is a subcommand,
    `dotnet wix eula accept wix7`. Sourced from the WiX 7 upstream's
    `EulaCommand.cs`. Build went all the way through to the table diff.
  - **iter5** — first real diff: 86 differences, of which 3 were
    upgrade-critical (sequence-anchor tie-break at `InstallExecuteSequence`
    pos 2 and `InstallUISequence` pos 9, plus `Upgrade` row `Language=`
    column defaulting to en-US instead of locale-agnostic). Fixed by
    anchoring `SetPowerShell5AsPath` on `RelocateLegacyInstallRoot`,
    chaining `BlockUserCancelledLegacy` after `BlockLegacySunshineStillPresent`,
    and `IgnoreLanguage="yes"` on `<MajorUpgrade/>`.
  - **iter5b** — `Upload MSI compatibility dump` step needed `if: always()`
    so the dump uploads even when the diff fails; without it the artifact
    we'd need to re-baseline against doesn't exist.
  - **iter6** — re-baselined the golden against the WiX 7 candidate. The
    remaining 82 diffs were all toolchain fingerprints (`WixCA` →
    `Wix4UtilCA_X64`, `WixFirewallCA` → `Wix5FWCA_X64`, `Wix5*_X64`
    action-name prefix on firewall CAs, `WixUIPrintEula` gone in v4+ UI,
    BURNMSIMODIFY/REPAIR/UNINSTALL added to SecureCustomProperties,
    `WixUI_Mode` gone, registry KeyPath auto-IDs hashed differently,
    `*`-Guid ComponentIds re-derived from those KeyPaths, 8.3 short
    names regenerated). Manually audited the new candidate against the
    pre-rebaseline golden for upgrade invariants (UpgradeCode,
    ServiceInstall names, hand-authored Component GUIDs, sequence
    ordering, bootstrapper-contract properties) — all preserved.
    Renamed `wix3_baseline.txt` → `msi_baseline.txt` since the file is
    no longer the WiX-3 era snapshot.
  - **iter7** — CI all green. Oracle diffs clean against the new
    baseline.
- **PR 3.final** — Real-Windows install matrix verification (see PR 3 row's
  merge gate above).

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
