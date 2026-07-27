# Postmortem — 2026-07-27 GPU/WDDM collapse, blank monitors, refused sessions

> Cross-machine handoff copy. An identical copy lives in the LuminalVGD
> repo (`docs/POSTMORTEM-2026-07-27.md`). Written on the macOS
> analysis box; the evidence-collection appendix at the end is the to-do
> list for the Windows dev machine.

**Systems:** LuminalShine 26.08.0-beta.5 (c5ca13646) + LuminalVGD 0.1.0.13 (proto 0.3, caps 0x1e5) on RTX 5080, Windows Insider build.
**Evidence:** `luminalshine_logs-20260727-123054.zip` (129 logs, 2026-05-16 → 2026-07-27 12:30), web UI screenshots, LuminalVGD repo @ 71418b7, luminalshine clone @ 26.07.1-beta.2.
**Method:** 16-agent analysis (7 parallel evidence readers → synthesis → adversarial verification of 8 hypotheses; every load-bearing citation re-checked against primary logs).

---

## TL;DR

One real GPU hang occurred at **11:00:22** during a 4K240 HDR stream (GTA5-profile load). Windows' TDR recovery **failed** and left the entire WDDM display stack dead machine-wide — `QueryDisplayConfig` returns `ERROR_NOT_SUPPORTED`, zero displays enumerate, `SetDisplayConfig` returns `ERROR_GEN_FAILURE`. That is why the physical monitors are blank: the desktop was on a virtual-display-exclusive topology when the stack died, and no user-mode code can re-enable anything. **Only a reboot exits this state** (the identical May 17 wedge, under SudoVDA, cleared only on reboot).

Everything else on the screen — "39 TDR events", "Recovery in progress (new sessions refused)", "Unable to find display or encoder", "Virtual Display Driver: Not present" — is LuminalShine beta.5 **misdiagnosing** that single dead-stack condition in an unbounded loop. There was exactly **one** TDR today, not 39.

LuminalVGD build 13's control plane worked flawlessly throughout (handshake, CREATE_MONITOR, ring GPU-reset event all correct). It is exonerated as the cause, with two minor defects noted below and one open question about the IddCx class as a whole.

A separate, independent bug: `luminalshine.exe` dies silently (~10 abrupt terminations since 7/25; dump `luminalshine.exe.57428.dmp` at 10:48:36 is unanalyzed). These crashes did **not** cause the blank monitors — every post-crash restart enumerated displays fine.

---

## Timeline (2026-07-27, all times local)

| Time | Event |
|---|---|
| 04:00:46 | Silent `luminalshine.exe` death #1 (idle, overnight). Restart healthy. |
| 10:35–10:41 | 4K240 HDR stream to client "SnowFrost-CCS" via LuminalVGD ring (DISPLAY22). Healthy. |
| 10:41:08 | Silent host death #2 **mid-stream** (last line: `Audio capture signaled buffer discontinuity`). Helper's golden restore **succeeds** at 10:41:13 — stack healthy. |
| 10:43–10:47 | Four short stream attempts (LG C2, Steamdeck, LG C2 ×2), ~10 s each, client-side disconnects. All reverts/restores succeed. |
| 10:48:32 | Silent host death #3 (idle, 73 s after a REVERT) → **crash dump 57428** at 10:48:36. |
| 10:48:41 | Restart: handshake OK, encoder probe passes on physical DISPLAY2 — **stack still healthy after the crash**. |
| 10:51:37 | Fatal stream begins: SnowFrost-CCS, VGD session `0x3c9f037e7b3ae845`, 3840×2160@240 HDR. Helper applies virtual-exclusive topology (last successful topology write on this machine). |
| 10:57:07 | Frame latency triples (5→16 ms) — heavy GPU load (GTA5 window; Playnite `[Desktop]` placebo hides the name). |
| **11:00:22.813** | **THE FAILURE:** 3× audio discontinuity → `NvEnc: frame 40357 encode wait timeout` (hresult=0x0, no DXGI device-removed code) → `GPU TDR escalated to WDDM stack failure (encoder stall)`. |
| 11:00:23.025 | `LuminalVGD capture: GPU-reset event recorded; releasing shared ring textures and reinitializing` — the ring sees the reset, correctly. |
| 11:00:25–41 | NVENC destroy drain times out 2500 ms → deliberate registration leak #1. `D3D11CreateDevice` fails `0x887A0004` ×5. **WDDM never comes back.** |
| 11:00:41.806 | First `QueryDisplayConfig` → `ERROR_NOT_SUPPORTED`. Cleanup runs (`had_active_virtual_display=false` — nothing left to remove). Helper rejects golden **and** session snapshots: `no valid devices available`. **Physical monitors blank from here.** |
| 11:00:56 | Deliberate clean self-restart ("reclaim leaked NVENC registrations") — not a crash. |
| 11:01:02 | New session boots into dead stack: QDC error 50, 0 devices, **yet** `LuminalVGD driver ready: proto 0.3 build 13`. Temp monitor created; `no new display surfaced within 5 s`. |
| 11:01:12 → 12:30+ | Misclassification loop: `0x887A0004 → "GPU TDR recovery suspected" → 5 retries → "escalated to WDDM stack failure … new sessions refused"` in ~33 s cycles (bursts at startup and on each client attempt). All 5 encoders fail identically. Health card counts each cycle as a "TDR event" → 39. |
| 12:25:04–12:30 | LG C2 retry: 2 more monitors created, never surface → **2 zombie driver sessions**. `SetDisplayConfig` → `ERROR_GEN_FAILURE`, `CDS_RESET` → −1. Helper: `Enumerated device list does not contain primary devices!` |

## Why it's occurring — three layers

### (a) A real GPU-level failure (the trigger)

At 11:00:22, ~8.6 min into a 4K240 HDR HEVC stream under heavy game load, the GPU stalled: NVENC encode-wait timeout, confirmed 200 ms later by the LuminalVGD ring's GPU-reset event. The same signature struck 7/26 17:04 (105 min into a 4K HDR DLSS-FG session, beta.4 + driver build 11) — that one **recovered in ~2 minutes**. Prior history: Halo → `watchdog.sys` bugchecks; the identical terminal wedge on **2026-05-17 under SudoVDA**, before LuminalVGD existed.

Attribution: an NVIDIA (Blackwell) / Insider-WDDM-level fault is the strongest hypothesis, but is **not yet proven** — no kernel-side evidence (System event log `nvlddmkm` 4101, LiveKernelReports, minidumps) has been collected. The OS was Insider build **29617** as of 2026-07-24 (recorded in CLAUDE.md's control-ACL notes); the NVIDIA driver version is still unrecorded anywhere. One caveat the verifiers insisted on: **in all three observed collapses (5/17, 7/26, 7/27) an IddCx virtual display was the exclusive active display**, so "active indirect display during TDR recovery" being part of the trigger cannot be excluded — that would still be an OS/NVIDIA bug, but it changes the mitigation calculus. Driver build 13 *specifically* is weakly implicated at most (n=1 each: build 11's TDR recovered, build 13's wedged; build 13 also ran ~17 h of healthy sessions first).

### (b) Failed OS recovery → machine-wide WDDM wedge (why monitors are blank)

Post-11:00:41, in **two independent processes** (SYSTEM service + interactive-desktop helper): `QueryDisplayConfig` → `ERROR_NOT_SUPPORTED`, enumeration → 0 devices, `D3D11CreateDevice` → `DXGI_ERROR_UNSUPPORTED` (236×), `SetDisplayConfig` → `ERROR_GEN_FAILURE`, and a successfully-created IddCx monitor that never surfaces (monitor arrival requires dxgkrnl). This is below every user-mode component. The physical monitors are blank because the desktop had been virtual-display-exclusive since ~10:35 and nothing can re-enable the physical outputs while the display-topology API is dead. The helper's restore logic is *proven good* — it succeeded at 10:41:13 — it simply cannot operate.

**Refuted hypothesis (for the record):** cleanup destroying the only active display did *not* cause or aggravate the wedge — the 11:00:41 cleanup logged `had_active_virtual_display=false` and ran in 1 ms; the stack was already dead 19 s earlier; five identical cleanups at 10:41–10:47 caused nothing. Residual truth: running virtual-*exclusive* meant no physical output was lit when the wedge hit, maximizing user impact.

### (c) LuminalShine beta.5's defective reaction (why the UI looks like this)

1. **HRESULT misclassification.** `0x887A0004` is `DXGI_ERROR_UNSUPPORTED` — a capability/environment error. Beta.5's dd-test classifier calls it "GPU TDR recovery suspected". The spec (DESIGN.md §3.3 rule 2; WGC-RELIABILITY.md R3) keys TDR **exclusively** on `DXGI_ERROR_DEVICE_REMOVED`/`GetDeviceRemovedReason` + ring state `REBUILDING`. `0x887A` appears nowhere in the LuminalVGD repo — this classifier is fork-side code that bypasses the tested `CaptureController`. (Nuance: on 7/26 the same HRESULT *was* transiently returned during a real TDR and cleared — so retrying briefly is defensible; the defect is there is no terminal state and no cross-check of the co-occurring QDC failure that distinguishes "stack dead" from "adapter resetting".)
2. **Unbounded loop + fabricated counter.** Each burst: 5 retries (1/2/4/8 s backoff) → escalate → re-arm, ~33 s/cycle, ~66 s per encoder, "Fatal: Unable to find display or encoder" every ~5.5 min, "new sessions refused" with **no exit condition**. The spec's ladder is once-per-rung ending in R6 "fail the session loudly ≤3 s". The "39 TDR events" are 1 event re-detected ~22+ times (21 escalations in the current log + counter still running).
3. **WGC fallback cannot engage.** The WGC IPC backend needs a D3D11 device on the same dead adapter before it ever launches `luminalshine_wgc_capture.exe`; there is no WARP path. So the DESIGN.md §2.1 seamless fallback — which has *never* visibly engaged mid-session in any log — is structurally unavailable exactly when it's needed.
4. **Dead legacy recovery UI.** `GET /api/state/vdd-diagnostic` still probes SudoVDA identifiers (LuminalVGD deliberately uses new IDs: `root\luminal_vgd`, new interface GUID) → "Not present" while the real driver handshakes fine in the same process. Worse, at beta.2 the *automatic* QDC-wedge handler still calls `platf::sudovda::run_recovery_ladder(pnp_restart)` (`virtual_display.cpp` ~4180) — inert on this host. The "Restart Virtual Display Driver" button has never executed (zero POSTs in all logs). FEATURE-MATRIX.md explicitly dropped the PnP disable/enable model in favor of session IOCTLs.
5. **Zombie sessions.** `CREATE_MONITOR` succeeds against a dead dxgkrnl, monitor never surfaces, host keeps the session: "no tracked session matches display (2 sessions)". No surfaced/failed feedback loop exists in the protocol.

### Independent finding: the silent host-crash bug

~10 abrupt `luminalshine.exe` terminations since 7/25 (some are installer/update kills; solid crash candidates: 7/25 12:59:30 mid-stream, 7/26 17:04:47 *during* TDR recovery, 7/27 04:00:46 idle, 10:41:08 mid-stream, 10:48:32 with dump). Two mid-stream deaths end with `Audio capture signaled buffer discontinuity`; two others are teardown/REVERT-adjacent. **Confirmed:** none of them killed the display stack. The 68 MB dump has not been analyzed — that is the single highest-value unopened piece of evidence, along with the Windows System event log. Note: luminalshine already tracks a known heap crash (task #33, `0xc0000374` when capture starts against a never-activated display with an empty device name, found 2026-07-25) — check whether the dump matches it before opening a new crash investigation.

### LuminalVGD driver: verdict

Exonerated as cause of (a)/(b) — control plane flawless through the entire incident; identical wedge predates it (SudoVDA, 5/17). Real defects found:
- **Watchdog contract lie:** handshake advertises `watchdog 3 s`, but `effective_lease_timeout()` floors USE_DEFAULT at `DEFAULT_LEASE_TIMEOUT_MS = 10_000` (`crates/luminal-vgd-core/src/session.rs:84-99`); measured orphan-unplug after the 10:41 crash was ~11 s.
- **No surfaced/failed feedback** for CREATE_MONITOR; no recovery/reset IOCTL.
- **Driver-side tracing existed but wasn't running.** ETW TraceLogging (provider `NortheBridge.LuminalVGD`, GUID in CLAUDE.md) has shipped since phase 2, but ETW is capture-on-demand — no `logman` session was active during the wedge window, so there is zero driver-side evidence for it. WPP/IFR (which would have given always-on in-flight recorder data, §3.3.6) remains an unwired tracked deviation.
- *(Correction to the original analysis: the "deployed build 13 diverges from `main`" claim was an artifact of a two-week-stale analysis clone. `origin/main` at `9a9dd2d` contains the entire Windows-side line — phases 2/4/5/7, driver builds 2–13, released as v0.1.0-alpha.3.)*

---

## Plan of action

### Now — recover the machine (in order)

1. Try `Win+Ctrl+Shift+B` (WDDM driver restart). Free, likely won't clear it given `SetDisplayConfig` → `GEN_FAILURE`. Ignore the UI's SudoVDA advice entirely.
2. **Preserve evidence before rebooting** — see the appendix below for the full collection list.
3. **Reboot** (`shutdown /r /t 0`). Only known exit from this wedge.
4. After reboot: confirm physical topology restored (the boot-time `LuminalShineDisplayRestore` task should fire; if not, run the helper `--restore`). Check the driver's permanent pool is empty; zombie sessions die with the reboot.
5. Restart LuminalShine only after QDC is confirmed healthy, so the "recovery in progress" state clears.
6. Analyze the dump in WinDbg (`!analyze -v`) and pull the System event log entries — these settle the real-TDR attribution and the crash bug.
7. Until fixes land, reduce recurrence risk: cap the virtual display at 4K120 (both real TDRs were 4K HDR high-refresh under load), raise `TdrDelay`/`TdrDdiDelay`, update/roll back the NVIDIA driver, seriously consider taking this box off the Insider ring, and (optionally) stop using virtual-*exclusive* topology so a future wedge can't blank every output.

### Beta.5 release blockers (LuminalShine host — all in fork code)

1. **Classifier fix:** `0x887A0004` = environment error, never TDR. TDR detection = `DXGI_ERROR_DEVICE_REMOVED` + `GetDeviceRemovedReason` + ring `REBUILDING` (per spec). Pair every D3D11 failure with a QDC probe: `QDC ERROR_NOT_SUPPORTED` + 0 devices ⇒ terminal diagnosis **"display stack down — reboot required"**, surfaced in the web UI.
2. **Bound the loop:** after N (≈2) full failed cycles with no state change, enter the terminal state, fail loudly per R6 (structured error + log bundle), stop the per-encoder 66 s probes. Health card shows **1 event with a duration**, not a counter of re-detections.
3. **Replace the SudoVDA diagnostic/recovery path:** probe `LUMINAL_VGD_INTERFACE_GUID` + `HANDSHAKE`/`GET_STATUS`; delete the PnP-SudoVDA remediation text and the `platf::sudovda::run_recovery_ladder` call in the wedge handler; wire the restart button to a real, server-logged action (DESTROY-all + re-handshake; PnP-cycle `root\luminal_vgd` as last resort).
4. **Wire the tested `CaptureController`** (§2.1 seamless fallback, 1→30 s restore probes, R1–R6) into the streaming path, and add a WARP device path for probes/WGC-IPC so nothing funnels through one pinned, possibly-dead adapter. Re-resolve `adapter_name`→LUID on every recovery attempt, not just session start.
5. **Crash bug:** dump analysis, top-level crash handler (log + dump on all deaths), investigate the audio-capture `buffer discontinuity` path and REVERT-adjacent teardown.
6. **Helper:** on persistent QDC error-50, escalate to "reboot required" instead of 84 min of silent polling; arm retry-on-device-arrival instead of rejecting snapshots each cycle; bound orphan lifetime after pipe loss.

### Driver build 14 (LuminalVGD repo)

7. Reconcile the watchdog contract (advertise 10 s or honor 3 s; one line either way, plus doc) — confirmed still present on `main` @ `9a9dd2d`: `effective_lease_timeout()` floors USE_DEFAULT at `DEFAULT_LEASE_TIMEOUT_MS = 10_000`.
8. CREATE_MONITOR surfaced/failed feedback (GET_STATUS per-session arrival flag) + host auto-DESTROY when nothing surfaces in its 5 s window; consider a DESTROY_ALL/RESET recovery IOCTL (PROTO_VERSION_MINOR bump).
9. **Close the observability gap**: wire WPP/IFR (§3.3.6, the tracked deviation) so the next wedge leaves always-on driver-side evidence; until then, keep a standing `logman` ETW session on `NortheBridge.LuminalVGD` running on the dev box during any stress/repro testing.

### Verification / follow-up

10. Controlled repro to settle the IddCx-class question: same load on release-ring OS / different NVIDIA driver; a run with a physical display kept active alongside the virtual one.
11. Port `alttab_stress` and add a TDR-injection test (per WGC-RELIABILITY §7) so the ladder's terminal behavior is exercised in CI.

---

## Verified-hypothesis summary

| # | Claim | Verdict |
|---|---|---|
| H1 | 11:00:22 was a genuine GPU hang (not app-caused) | Plausible — kernel-level failure effectively confirmed; nvlddmkm/GTA5 attribution needs Event Log |
| H2 | Post-11:00:41 state is an OS-level WDDM wedge only a reboot clears | **Confirmed** |
| H3 | Cleanup destroying the only display caused/aggravated the blank monitors | **Refuted** |
| H4 | Beta.5 misclassifies 0x887A0004 → unbounded loop, refused sessions, bogus counter | **Confirmed** |
| H5 | LuminalVGD build 13 not causal; only minor secondary defects | Plausible (IddCx-class involvement in failed recovery not excludable) |
| H6 | Silent host crashes are independent; didn't blank monitors | Plausible ("didn't blank monitors" confirmed; "independent of TDR" fails for the 7/26 17:04:47 death) |
| H7 | vdd-diagnostic card is dead SudoVDA legacy; restart never executed | **Confirmed** |
| H8 | Insider + RTX 5080 regression underlies both TDR rate and failed recovery | Plausible — strongest machine-level hypothesis, needs kernel evidence |

---

## Appendix — evidence collection on the Windows dev machine

Do this **before** reboot where possible, and before any disk cleanup or
Windows update runs (WER and Disk Cleanup prune `CrashDumps` and
`LiveKernelReports` on their own schedule).

**User-mode crash dump** (for the silent-crash bug):
- Target file: `luminalshine.exe.57428.dmp` — 68 MB, created 7/27 10:48:36 AM
  (WER LocalDumps naming: `<exe>.<pid>.dmp`).
- Easiest: the web UI **Export Crash Bundle** button (the server already
  knows the dump's path — it displays name/size/timestamp).
- Direct locations, in order of likelihood:
  - `C:\Windows\System32\config\systemprofile\AppData\Local\CrashDumps\`
    (LuminalShine runs as a SYSTEM service)
  - `%LOCALAPPDATA%\CrashDumps\` for the logged-in user
- Fallback search:
  `Get-ChildItem -Path C:\Users, C:\Windows\System32\config\systemprofile, C:\ProgramData -Recurse -Filter 'luminalshine.exe.57428.dmp' -ErrorAction SilentlyContinue | Select-Object FullName, Length, LastWriteTime`
- Analyze with WinDbg `!analyze -v`.

**Kernel-side evidence** (for the TDR wedge — currently zero kernel evidence exists):
- `C:\Windows\LiveKernelReports\` incl. `WATCHDOG`/`NVIDIA` subfolders —
  failed-TDR LiveKernelEvent 117/141 reports; most likely direct evidence
  of the 11:00:22 hang.
- `C:\Windows\Minidump\*.dmp` — bugcheck minidumps, incl. any from the
  Halo `watchdog.sys` era.
- `C:\Windows\MEMORY.DMP` if present.
- `wevtutil epl System C:\evidence\system.evtx` — System event log with
  `nvlddmkm` 4101 / `dxgkrnl` entries around 7/27 11:00:22, plus
  Event 41/1001 bugcheck records.
- Also record the exact **OS Insider build** and **NVIDIA driver version**
  — neither is logged anywhere, and both are needed to settle the
  Insider-vs-NVIDIA-vs-IddCx-class attribution.
- Snapshot state files: `%APPDATA%\Sunshine\display_golden_restore.json`
  and `display_session_*.json`.
