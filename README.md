<h1 align="center">LuminalShine</h1>

<p align="center">
  <a href="https://apps.northebridge.com/en/luminalshine"><img src="https://img.shields.io/badge/LuminalShine-Visit_Site-FFB020?style=for-the-badge" alt="LuminalShine — Visit Site"/></a>
  <a href="https://apps.northebridge.com/en/LuminalVGD"><img src="https://img.shields.io/badge/LuminalVGD-Official_Site-FFB020?style=for-the-badge" alt="LuminalVGD — Official Site"/></a>
  <a href="https://github.com/NortheBridge/luminalshine/releases/latest"><img src="https://img.shields.io/github/v/release/NortheBridge/luminalshine?include_prereleases&label=Release&color=blue&style=for-the-badge" alt="Latest Release"/></a>
  <a href="https://github.com/microsoft/winget-pkgs/tree/master/manifests/n/NortheBridge/LuminalShine"><img src="https://img.shields.io/winget/v/NortheBridge.LuminalShine?label=WinGet&color=blue&style=for-the-badge" alt="WinGet Version"/></a>
  <a href="https://github.com/NortheBridge/luminalshine/commits/main"><img src="https://img.shields.io/github/commits-since/NortheBridge/luminalshine/latest?include_prereleases&label=Commits%20since%20release&color=blue&style=for-the-badge" alt="Commits since latest release"/></a>
  <a href="https://github.com/NortheBridge/luminalshine/releases"><img src="https://img.shields.io/github/downloads/NortheBridge/luminalshine/total?label=Downloads&color=blue&style=for-the-badge" alt="GitHub Downloads"/></a>
  <a href="https://github.com/NortheBridge/luminalshine/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/NortheBridge/luminalshine/ci.yml?label=CI&style=for-the-badge" alt="CI"/></a>
  <a href="https://codecov.io/gh/NortheBridge/luminalshine"><img src="https://img.shields.io/codecov/c/github/NortheBridge/luminalshine?token=BCG83VQ1LZ&label=Coverage&style=for-the-badge" alt="codecov"/></a>
  <a href="https://github.com/NortheBridge/luminalshine/actions/workflows/update-pages.yml"><img src="https://img.shields.io/github/actions/workflow/status/NortheBridge/luminalshine/update-pages.yml?label=Build%20GH-Pages&style=for-the-badge" alt="Build GH-Pages"/></a>
  <a href="https://github.com/NortheBridge/luminalshine"><img src="https://img.shields.io/github/languages/code-size/NortheBridge/luminalshine?label=Code%20size&color=blue&style=for-the-badge" alt="Code size in bytes"/></a>
  <a href="https://github.com/NortheBridge/luminalshine/stargazers"><img src="https://img.shields.io/github/stars/NortheBridge/luminalshine?label=Stars&color=blue&style=for-the-badge" alt="GitHub stars"/></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/NortheBridge/luminalshine?label=License&color=blue&style=for-the-badge" alt="License"/></a>
</p>

<p align="center">
  <strong>A modern, self-hosted, Sunshine-based game streaming platform purpose-built for Modern Windows 11.</strong>
</p>

---

## What is LuminalShine?

LuminalShine is a Windows-first game streaming host developed by the **NortheBridge Foundation**. It is a hardened, modernized descendant of [Sunshine](https://github.com/LizardByte/Sunshine) by way of Vibeshine, focused exclusively on delivering a stable, low-latency streaming experience on current Windows 11 releases — including the **Windows Insider Preview Canary and Dev channels** — paired with any Moonlight client or the built-in WebRTC browser client.

LuminalShine began as a fork of **Vibeshine** specifically to address deficiencies on the Windows 11 Insider Preview platform that upstream maintainers were unwilling to investigate. Since then it has diverged substantially in architecture, driver support, and capture pipeline.

---

## System Requirements (Windows)

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| **OS** | Windows 11 23H2 (x64) | Windows 11 24H2 or Insider Preview (Canary / Dev) |
| **CPU** | Quad-core x86-64 with AVX2 | 6+ cores, recent Intel / AMD / Snapdragon X |
| **GPU** | DirectX 12 capable, HEVC encode | NVIDIA RTX 40/50-series, AMD RDNA 3/4, or Intel Arc with AV1 encode |
| **RAM** | 8 GB | 16 GB+ |
| **Display Stack** | WDDM 3.0 | WDDM 3.2+ with HDR-capable physical or virtual display at 240 Hz for frame-gen capture |
| **Network** | 1 Gbps wired or Wi-Fi 6 | 2.5 Gbps wired or Wi-Fi 6E/7 |
| **PowerShell** | Windows PowerShell 5.1 | [PowerShell 7](https://github.com/PowerShell/PowerShell) <sub>*— PowerShell 7 supersedes Windows PowerShell as Microsoft's current shell; LuminalShine's MSI custom actions automatically prefer it when installed.*</sub> |
| **Privileges** | Local administrator for install and service registration | TPM 2.0 (used for credential sealing) |

> LuminalShine is **Windows-only by design**. Linux and macOS support from upstream Sunshine has been deprecated to allow the team to focus entirely on the Windows platform. Multi-platform support may return in the future but is not on the current roadmap.

---

## Key Features

- **Native Windows 11 + Insider Preview Support** — Engineered against the latest Canary and Dev channel builds; works around `dxgi.dll` quirks introduced in recent flights.
- **HEVC and AV1 First, with HDR** — Modern codec paths are the default. H.264 remains supported on older builds.
- **Windows Graphics Capture (WGC) in Service Mode** — Higher throughput, captures full frame-generated frame rates, survives VRAM exhaustion, and falls back automatically so the login screen and UAC prompts remain capturable.
- **SudoVDA as the Primary Virtual Display Driver** — Ships and is enabled by default with stability improvements. Supports automatic or explicit GPU binding, including hybrid-GPU laptops; ideal for headless setups.
- **Luminal Video Graphics Display Driver (LuminalVGD)** — *Coming later this year.* A first-party virtual display driver developed in-house to eventually supersede SudoVDA as the default.
- **Display Setting Automation** — Restores your display layout after hard crashes, shutdowns, or reboots; safeguards against unreleased dummy plugs and virtual displays on Windows 11 24H2 and Insider Preview.
- **Modern Frontend WebUI** — Fully responsive; manage your library and configuration from a phone or tablet without restarting the service.
- **WebRTC Browser Streaming** — Stream directly to the browser via the `/webrtc` endpoint — no separate client install needed. The standard Moonlight path is still available.
- **TPM-bound Credential Sealing** — Administrator credentials are sealed to the TPM by default on Windows, with platform-native fallbacks (Windows Credential Manager, libsecret, Keychain) on other systems.
- **RTSS & NVIDIA Control Panel Integration** — Applies the correct frame limit and disables V-Sync before streaming for noticeably smoother frame pacing.
- **Frame-Generation Capture Fixes** — DLSS / FSR / XeSS frame-generation titles capture at full rate without micro-stutter (requires a 240 Hz display, physical or virtual).
- **Lossless Scaling & NVIDIA Smooth Motion** — Optional per-app frame generation; Smooth Motion supported on RTX 40 / 50 series.
- **Playnite Integration** — Auto-sync recently played games with per-category rules and exclusions; artwork, launching, and clean termination are handled for you.
- **Scoped API Tokens** — Method-level scoping so automation scripts never need full admin rights.
- **Session-Based Authentication** — Password-manager friendly, with an opt-in "remember me" flow.
- **Pre-Release + GA Update Notifications** — Side-by-side channels so you can pull in-development fixes the moment they land.

---

## Relationship to Sunshine and Vibeshine

LuminalShine is a **complementary fork**, not a replacement. Sunshine remains the right choice for cross-platform deployments, and Vibeshine remains its own project.

**LuminalShine features will not be backported to Sunshine or Vibeshine.** Driver changes, the WGC service architecture, the Insider Preview workarounds, and the pace of development make maintaining backports impractical. The codebase has diverged far enough that upstreaming would no longer be a clean merge — it would be a rewrite.

LuminalShine remains free and open source under the **GNU GPL-v3** license and will not be sold or offered commercially.

---

## Documentation

Full documentation — installation, configuration, driver selection, WebRTC setup, troubleshooting, and developer guides — lives at:

### [gitdocs.northebridge.com/luminalshine](https://gitdocs.northebridge.com/luminalshine)

`gitdocs.northebridge.com/luminalshine` is the **official documentation site for LuminalShine**, hosted under the NortheBridge Foundation documentation portal. Issues, contributing guidelines, and release notes are mirrored there alongside this repository's GitHub Issues tracker.

---

## Origin of the Name

"LuminalShine" is the adjective form of *lumen* — the SI unit for luminous flux — riffing on the first half of *Sunshine*.

---

## License

LuminalShine is distributed under the [GNU General Public License v3.0](LICENSE). Contributions are welcome under the same terms.
