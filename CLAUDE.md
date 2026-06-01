# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project context

LuminalShine is a Windows-only game streaming host — a hardened fork of [Sunshine](https://github.com/LizardByte/Sunshine) by way of Vibeshine, run by the NortheBridge Foundation. Upstream Linux/macOS support has been deprecated; the build still configures for those platforms (much of the code remains compilable), but only Windows is shipped and CI-tested. Treat platform-specific code under `src/platform/linux/` and `src/platform/macos/` as legacy.

The CMake target is named `sunshine` (do not rename — many `target_*` calls across `cmake/` reference it), but the executable's `OUTPUT_NAME` is `luminalshine`. Most source files, namespaces, and the `SUNSHINE_*` CMake/define prefix retain the upstream "sunshine" name.

## Build & run

The project is C++23 (CUDA bits at C++17) built with **clang + `--fms-extensions`** so MSVC-only Windows APIs compile without depending on MSVC itself. Get submodules first — many builds break without them (see `.gitmodules` for which are deliberately commented out: Linux-only inputtino/flatpak deps, and doxyconfig because its pinned commit no longer exists upstream).

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -S .
ninja -C build              # builds luminalshine + web-ui + (if BUILD_TESTS=ON) test_sunshine
```

Key CMake options (`cmake/prep/options.cmake`):

- `SUNSHINE_ENABLE_WEBRTC=ON` — Windows-only; links against a separately-built libwebrtc wrapper. See `docs/building.md` and `scripts/build_mingw_webrtc.ps1`. The build cache lives at `%LOCALAPPDATA%\LuminalShine\deps\libwebrtc\{src,out}` and is **shared across worktrees** — wiping `build/` does not invalidate it.
- `BUILD_DOCS=OFF` — required on Windows CI because the `third-party/doxyconfig` submodule pin is dead. Default is ON; flip OFF if doxygen build fails.
- `BUILD_TESTS_WITH_COVERAGE=ON` — opt-in gcov instrumentation. Off by default because MSYS2's clang lacks the profile runtime, which would break the Windows-clang build if unconditional.

Web UI commands run from `src_assets/common/assets/web/` (where its own `package.json` lives — there is no root-level `package.json`):

```bash
npm ci
npm run build           # production Vite build into build/assets/web
npm run build:debug     # debug Vite build with sourcemaps + Vue devtools
npm run dev             # watch-mode debug build
npm run lint            # eslint .js/.ts/.vue (max-warnings 0)
npm run typecheck       # vue-tsc --noEmit
npm run test            # vitest run (frontend tests in tests/frontend/)
npm run test:coverage   # vitest with @vitest/coverage-v8
```

CMake's `web-ui` custom target invokes `npm ci` + `npm run build` automatically; only run npm directly when iterating on the frontend.

The Windows MSI is built by the `luminalshine_msi` target (cmake/packaging/windows_wix.cmake), which drives `dotnet wix build` via the WiX 7 CLI pinned in `.config/dotnet-tools.json`. Run `dotnet tool restore` once after cloning so `dotnet wix` resolves; the CMake configure step does this automatically but standalone packaging work outside CMake needs it. WiX 7 also requires accepting the OSMF EULA — `dotnet wix eula accept wix7` (one-time per user). The MSI lands at `build/cpack_artifacts/LuminalShine-<ver>-win64.msi`; CPack is still used for the portable ZIP (`cpack -G ZIP`) but no longer for the MSI.

## Tests

C++ tests use GoogleTest, built as the `test_sunshine` executable when `BUILD_TESTS=ON` (default). All tests are picked up via `file(GLOB_RECURSE)` from `tests/`. Layout:

- `tests/unit/` — module-level C++ tests.
- `tests/integration/` — cross-cutting checks: `test_config_consistency.cpp` verifies `src/config.cpp`, `docs/configuration.md`, the locale JSON, and the General settings Vue page agree; `test_locale_consistency.cpp` checks all locale files; `test_external_commands.cpp` exercises the shell-out path.
- `tests/frontend/` — Vitest + jsdom tests for Vue/TS code (run via `npm run test`, not via `test_sunshine`).
- `tests/fixtures/` — shared input files.

Run a single C++ test with the GoogleTest filter:

```bash
./build/tests/test_sunshine --gtest_filter='SuiteName.TestName'
ctest --test-dir build -V                # via CTest
```

Integration tests rely on files copied into the build dir at configure time (`configure_file`) and a synced locale tree (`sync_locale_files` custom target) — if you add to `INTEGRATION_TEST_FILES` in `tests/CMakeLists.txt`, re-run CMake.

## Architecture

`architecture.md` at the repo root is the authoritative deep-dive — read it before touching the streaming pipeline. Highlights:

**Single-process daemon, multi-threaded.** `src/main.cpp` initializes subsystems (`config::parse`, `platf::init`, `proc::init`, `input::init`, `video::probe_encoders`, `http::init`) then spawns long-lived service threads:

- `nvhttp::start` — NVIDIA GameStream HTTP control plane (`src/nvhttp.cpp`).
- `confighttp::start` — HTTPS Web UI + `/api/**` REST + WebRTC signaling (`src/confighttp.cpp`).
- `rtsp_stream::start` — classic Moonlight media/control plane (`src/stream.cpp`, `src/rtsp.cpp`).

**Mailbox-based cross-thread coordination.** Global `mail::man = std::make_shared<safe::mail_raw_t>()` in `src/main.cpp`; per-session pipelines own their own mailboxes with typed events/queues (`mail::shutdown`, `mail::idr`, `mail::gamepad_feedback`, …). WebRTC capture uses a dedicated mailbox on `webrtc_capture.mail`.

**Streaming paths are mutually exclusive.** RTSP (classic Moonlight) and WebRTC cannot run concurrently — `webrtc_stream::set_rtsp_sessions_active(bool)` is the flag; WebRTC capture refuses to start if it's set. This is the key architectural constraint when adding features that touch both paths.

**Capture / encode boundaries.**

- `src/video.cpp` is the capture+encode loop; platform-specific capture backends live under `src/platform/windows/display_*.cpp` (WGC, DXGI, D3D11/VRAM). Windows is the only fully-supported capture target.
- Encoders: NVENC (`src/nvenc/`), AMF (`src/amf/`), and FFmpeg paths inside `src/video.cpp`. Each is probed at startup via `video::probe_encoders()`.
- Audio capture: `src/audio.cpp` + `src/platform/**`. WebRTC sets `bypass_opus=true` and pipes raw PCM into libwebrtc, which does its own encoding; the classic path does Opus.
- Input injection: `src/input.cpp` consumes Moonlight-format input packets from `third-party/moonlight-common-c`. WebRTC translates JSON / compact-binary data-channel messages into the same Moonlight packets so the mature injection pipeline is reused.

**Display management is non-trivial on Windows.** The `display_helper_*` files in `src/platform/windows/` manage virtual displays (SudoVDA, MTT VDD, future LuminalVGD), apply display configurations atomically, and verify them with timeouts before capture starts. WebRTC calls `display_helper_integration::apply()` then `wait_for_apply_verification(6000ms)` before launching capture; if verification fails, capture refuses to start.

**Web UI.**

- Vue 3 + TypeScript + Vite + Tailwind (Bootstrap was removed; a small shim in `styles/tailwind.css` maps legacy `.btn` / `.form-control` to utilities — prefer migrating to real Tailwind over extending the shim).
- Vue Router uses `createWebHistory('/')` — no hash routing. The C++ `getSpaEntry` in `src/confighttp.cpp` is the SPA fallback that serves `index.html` for any non-API, non-static route. Adding a new top-level route needs no backend change unless it collides with `/api`, `/assets`, `/covers`, `/images`.
- EJS templating via `vite-plugin-ejs`; entry HTMLs in `src_assets/common/assets/web/` are discovered dynamically (`vite.config.ts`) so removing a page can't break the build.
- WebRTC client (`/webrtc` route) is in `views/WebRtcClientView.vue` and `utils/webrtc/*.ts`; signaling client in `services/webrtcApi.ts` — see `architecture.md` §5 for the full handshake.

**Authentication.** Every `/api/**` endpoint goes through `authenticate()` in `src/confighttp.cpp`, accepting either an `Authorization` header (scoped API token) or an HttpOnly session cookie. WebRTC signaling is not a separate unauthenticated server.

**Credential storage.** TPM-bound by default on Windows (`src/cred_store/`), with platform-native fallbacks (Windows Credential Manager).

**Localization.** `en` only. Strings live in `src_assets/common/assets/web/public/assets/locale/en.json`; do NOT include extracted/compiled `.po`/`.mo` files in PRs — those are generated by `.github/workflows/localize.yml` and CrowdIn.

## Security review guidance

This application runs with SYSTEM-level privileges and can execute code from files. Treat file execution, file parsing, update flows, plugin behavior, command execution, IPC, persistence, and user-controlled input paths as high-risk areas.

When evaluating security issues, prioritize realistic exploitability and practical impact. Issues that require the attacker to already have administrator or SYSTEM-level access are generally lower priority, unless they enable persistence, lateral movement, privilege retention, stealth, supply-chain compromise, or compromise of other users or environments.

Before recommending or implementing security-related changes, **spawn a subagent** to independently assess:

- the real-world exploitability and severity of the issue
- the security benefit of the proposed fix
- the compatibility, reliability, UX, and maintenance tradeoffs
- the risk that the fix introduces regressions or new vulnerabilities
- whether a smaller or safer mitigation would achieve the same goal

Do not suggest security changes purely because they are theoretically safer. Prefer recommendations that materially reduce risk, have clear threat-model alignment, and justify their cost. When a fix is recommended, include the rationale, expected security improvement, tradeoffs, and any safer alternatives considered.

Avoid broad rewrites, speculative hardening, or low-value changes unless there is a concrete threat model, reproducible exploit path, or meaningful reduction in blast radius.

> AI agents are like a glow stick: you snap and shake them and they glow real bright, but every file read and every back-and-forth dims that light. Serious decisions and second opinions should always go to a fresh subagent — not the current context.

## Conventions

- **Don't rename the `sunshine` CMake target or the `SUNSHINE_*` prefix** even though the product is LuminalShine — the build graph and many definitions depend on these names. The product name surfaces only via `OUTPUT_NAME`, packaging metadata, and user-facing strings.
- **Tailwind, not Bootstrap.** The shim exists for incremental migration; new code should use utilities directly. Dynamic class names need a `safelist` entry in `tailwind.config.js`.
- **Locale JSON keys are sorted alphabetically** (use jsonabc or equivalent).
- **MTT VDD's signed driver DLL is tracked in git** despite the `*.dll` ignore — see the explicit `!third-party/mtt-vdd/*.dll` exception in `.gitignore`. The `.cat` catalog must travel with it.
