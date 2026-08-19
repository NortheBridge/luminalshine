<h1 align="center">LuminalShine</h1>

<p align="center">
  <a href="https://apps.northebridge.com/en/luminalshine"><img src="https://img.shields.io/badge/LuminalShine-Visit_Site-FFB020?style=for-the-badge" alt="LuminalShine — Visit Site"/></a>
  <a href="https://apps.northebridge.com/en/LuminalVGD"><img src="https://img.shields.io/badge/LuminalVGD-Official_Site-FFB020?style=for-the-badge" alt="LuminalVGD — Official Site"/></a>
  <a href="https://github.com/NortheBridge/luminalshine/releases/latest"><img src="https://img.shields.io/github/v/release/NortheBridge/luminalshine?include_prereleases&label=Release&color=blue&style=for-the-badge" alt="Latest Release"/></a>
  <a href="https://github.com/microsoft/winget-pkgs/tree/master/manifests/n/NortheBridge/LuminalShine"><img src="https://img.shields.io/winget/v/NortheBridge.LuminalShine?label=WinGet&color=blue&style=for-the-badge" alt="WinGet Version"/></a>
  <a href="https://github.com/NortheBridge/luminalshine/commits/main"><img src="https://img.shields.io/github/commits-since/NortheBridge/luminalshine/latest?include_prereleases&label=Commits%20since%20release&color=blue&style=for-the-badge" alt="Commits since latest release"/></a>
  <a href="https://github.com/NortheBridge/luminalshine/releases"><img src="https://img.shields.io/github/downloads/NortheBridge/luminalshine/total?label=Downloads&color=blue&style=for-the-badge&logo=github&logoColor=white" alt="GitHub Downloads"/></a>
  <a href="https://github.com/NortheBridge/luminalshine/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/NortheBridge/luminalshine/ci.yml?label=CI&style=for-the-badge" alt="CI"/></a>
  <a href="https://codecov.io/gh/NortheBridge/luminalshine"><img src="https://img.shields.io/codecov/c/github/NortheBridge/luminalshine?token=BCG83VQ1LZ&label=Coverage&style=for-the-badge" alt="codecov"/></a>
  <a href="https://github.com/NortheBridge/luminalshine/actions/workflows/update-pages.yml"><img src="https://img.shields.io/github/actions/workflow/status/NortheBridge/luminalshine/update-pages.yml?label=Build%20GH-Pages&style=for-the-badge" alt="Build GH-Pages"/></a>
  <a href="https://github.com/NortheBridge/luminalshine"><img src="https://img.shields.io/github/languages/code-size/NortheBridge/luminalshine?label=Code%20size&color=blue&style=for-the-badge" alt="Code size in bytes"/></a>
  <a href="https://github.com/NortheBridge/luminalshine/stargazers"><img src="https://img.shields.io/github/stars/NortheBridge/luminalshine?label=Stars&color=blue&style=for-the-badge&logo=github&logoColor=white" alt="GitHub stars"/></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/NortheBridge/luminalshine?label=License&color=blue&style=for-the-badge" alt="License"/></a>
</p>

<p align="center">
  <strong>A modern, self-hosted, Sunshine-based game streaming platform purpose-built for Modern Windows 11.</strong>
</p>

---

## What is LuminalShine?

LuminalShine is a Windows-only game streaming host developed by the **NortheBridge Foundation**. It is a hardened, modernized descendant of [Sunshine](https://github.com/LizardByte/Sunshine) by way of Vibeshine, focused exclusively on delivering a stable, low-latency streaming experience on current Windows 11 releases — including the **Windows Insider Preview** channels — to any Moonlight client, or straight to a browser through the built-in WebRTC client with no client install at all.

For an overview, downloads, and project news, visit the official product page:

### [apps.northebridge.com/en/luminalshine](https://apps.northebridge.com/en/luminalshine/)

LuminalShine began as a fork of Vibeshine specifically to address deficiencies on the Windows 11 Insider Preview platform that upstream maintainers were unwilling to investigate. Since then it has diverged into its own platform: a first-party virtual display driver ([LuminalVGD](https://apps.northebridge.com/en/LuminalVGD/)), a service-mode Windows Graphics Capture pipeline, a full WebRTC streaming stack alongside the classic Moonlight protocol, and an end-to-end tuned video path that sustains full frame rates with single-digit-millisecond frame delivery.

---

## The LuminalVGD Virtual Display Driver

LuminalShine ships with and is built around **[LuminalVGD](https://apps.northebridge.com/en/LuminalVGD/)** — the *Luminal Video Graphics Display Driver* — NortheBridge's first-party IddCx virtual display driver, and uses it as the **default virtual display backend**.

- **On-demand virtual displays.** LuminalVGD creates HDR-capable, high-refresh virtual displays matched to the connecting client's resolution, refresh rate, and HDR capabilities — ideal for headless hosts and for streaming at modes your physical monitor can't do.
- **Developed in-house, vendored in-tree.** The driver core is written in Rust and vendored into this repository as a submodule at `src/drivers/luminal-display`, linked into the host through a C-ABI FFI layer. Host and driver evolve in lockstep, which is how tight integrations like GPU-reset self-healing and shared-ring frame delivery straight into the encoder are possible.
- **Verified display configuration.** The host applies display configurations atomically and verifies the requested topology actually took effect before capture starts — no more streaming a black screen because Windows silently rejected a mode change.
- **Resilient by design.** GPU resets and driver restarts are detected and recovered from without wedging the host; display layouts are restored after crashes, shutdowns, and reboots.
- **Installed for you.** The signed driver is bundled with and installed by the LuminalShine MSI — no separate download.

SudoVDA remains available as a legacy fallback backend, but new setups should use LuminalVGD. Full driver documentation lives on the [LuminalVGD site](https://apps.northebridge.com/en/LuminalVGD/).

---

## Key Features

- **Native Windows 11 + Insider Preview Support** — Engineered against the latest Insider Preview flights, with workarounds for platform regressions upstream projects won't touch.
- **HEVC and AV1 First, with HDR** — Modern codec paths are the default, including 10-bit and 4:4:4 chroma on capable encoders. H.264 remains supported for older clients.
- **Low-Latency Pipeline** — The capture → encode → egress path is tuned end-to-end (high-resolution timers, backlog control, stream-ordered GPU interop) to deliver full frame rates with single-digit-millisecond frame ages.
- **[LuminalVGD](https://apps.northebridge.com/en/LuminalVGD/) as the Primary Virtual Display Driver** — First-party, HDR-capable, self-healing; see above. SudoVDA is retained as a legacy fallback.
- **Windows Graphics Capture (WGC) in Service Mode** — Higher throughput, captures full frame-generated frame rates, survives VRAM exhaustion, and falls back automatically so the login screen and UAC prompts remain capturable.
- **WebRTC Browser Streaming** — Stream directly to any modern browser via the `/webrtc` route — no client install needed. The classic Moonlight path is fully supported alongside it.
- **Hardware Encoding on Every Vendor** — Dedicated NVENC and AMF integrations plus FFmpeg-based paths; encoders are probed at startup and the best available is selected.
- **Display Setting Automation** — Applies and verifies display configurations atomically before capture, and restores your layout after hard crashes, shutdowns, or reboots.
- **Modern Web UI** — Vue 3 + TypeScript + Tailwind, fully responsive; manage your library and configuration from a phone or tablet without restarting the service.
- **TPM-Bound Credential Sealing** — Credentials are sealed to the TPM by default, with Windows Credential Manager as the platform fallback.
- **Scoped API Tokens & Session Auth** — Method-level token scoping so automation never needs full admin rights; password-manager-friendly session login with an opt-in "remember me" flow.
- **RTSS & NVIDIA Control Panel Integration** — Applies the correct frame limit and disables V-Sync before streaming for noticeably smoother frame pacing.
- **Frame-Generation Capture** — DLSS / FSR / XeSS frame-generation titles capture at full rate without micro-stutter; Lossless Scaling and NVIDIA Smooth Motion (RTX 40/50 series) are supported per-app.
- **Playnite Integration** — Auto-sync recently played games with per-category rules and exclusions; artwork, launching, and clean termination are handled for you.
- **Modern Packaging** — WiX 7-built MSI installer (which also installs the LuminalVGD driver), [WinGet distribution](https://github.com/microsoft/winget-pkgs/tree/master/manifests/n/NortheBridge/LuminalShine) (`winget install NortheBridge.LuminalShine`), and a portable ZIP.
- **Pre-Release + GA Update Notifications** — Side-by-side channels so you can pull in-development fixes the moment they land.

---

## System Requirements

> LuminalShine is **Windows-only by design**. Linux and macOS support from upstream Sunshine has been deprecated so the team can focus entirely on the Windows platform.

### Recommended

| Component | Recommended Requirements | Hardware & Software Recommendation |
|-----------|--------------------------|------------------------------------|
| **OS** | Windows 11 x64 Build 22000 (21H2) or later — 24H2 required for some features | Windows 11 24H2 or later |
| **Processor** | Dual-core (4-thread) Intel or AMD CPU | Modern Intel or AMD processor |
| **GPU** | DirectX 11 capable with HEVC encode; DirectX 12 HEVC encode recommended | NVIDIA RTX 40/50-series, AMD RDNA 3/4, or Intel Arc with HEVC and AV1 encode |
| **RAM** | — | 16 GB+ |
| **Display Stack** | WDDM 3.0; WDDM 3.2+ recommended | WDDM 3.2+ with at least one HDR-capable display for [LuminalVGD](https://apps.northebridge.com/en/LuminalVGD/) configuration |
| **Network** | 2.5 Gbps (host) and Wi-Fi 5 (clients) | 2.5 Gbps (host) and Wi-Fi 6E/7 (clients) |
| **PowerShell** | Windows PowerShell 5.1 | [PowerShell 7](https://github.com/PowerShell/PowerShell/releases) *(see note below)* |
| **Privileges** | Local administrator for installation and service registration | Local administrator for installation and service registration, plus TPM 2.0 (used for credential sealing) |

### Dev & Test System

The hardware LuminalShine is actively developed and tested on:

| Component | Current Dev & Test Machine |
|-----------|----------------------------|
| **OS** | Windows 11 Enterprise Insider Preview (Experimental Preview branch) |
| **Processor** | Intel Core i9-12900KS @ 5.2 GHz (P-cores) / 4.0 GHz (E-cores) |
| **GPU** | NVIDIA RTX 5080-series Blackwell GPU |
| **RAM** | 64 GB |
| **Display Stack** | WDDM 3.2+ with at least one HDR-capable display for [LuminalVGD](https://apps.northebridge.com/en/LuminalVGD/) configuration |
| **Network** | 5 Gbps (host) and Wi-Fi 6E/7 (clients) |
| **PowerShell** | [PowerShell 7](https://github.com/PowerShell/PowerShell/releases) *(see note below)* |
| **Privileges** | Local administrator for installation and service registration, plus TPM 2.0 (used for credential sealing) |

> **PowerShell 7:** PowerShell 7 supersedes Windows PowerShell as Microsoft's current shell, and LuminalShine's MSI custom actions automatically prefer it when installed. Download the latest release from the [PowerShell 7 releases page](https://github.com/PowerShell/PowerShell/releases).

---

## Relationship to Sunshine and Vibeshine

LuminalShine is a **complementary fork**, not a replacement. Sunshine remains the right choice for cross-platform deployments, and Vibeshine remains its own project.

**LuminalShine features will not be backported to Sunshine or Vibeshine.** The LuminalVGD driver integration, the WGC service architecture, the WebRTC stack, the Insider Preview workarounds, and the pace of development make maintaining backports impractical. The codebase has diverged far enough that upstreaming would no longer be a clean merge — it would be a rewrite.

LuminalShine remains free and open source and will not be sold or offered commercially.

---

## Documentation

Full documentation — installation, configuration, driver selection, WebRTC setup, troubleshooting, and developer guides — lives at:

### [appdocs.northebridge.com/projects/luminalshine](https://appdocs.northebridge.com/projects/luminalshine/en/latest/)

`appdocs.northebridge.com/projects/luminalshine` is the **official documentation site for LuminalShine**, hosted under the NortheBridge Foundation documentation portal. Issues, contributing guidelines, and release notes are mirrored there alongside this repository's GitHub Issues tracker. Contributors should also read [`architecture.md`](architecture.md) — the authoritative deep-dive into the streaming pipeline.

---

## Origin of the Name

"LuminalShine" is the adjective form of *lumen* — the SI unit for luminous flux — riffing on the first half of *Sunshine*.

---

## License

LuminalShine is distributed under the [GNU General Public License v3.0](LICENSE). Contributions are welcome under the same terms.

The vendored [LuminalVGD](https://apps.northebridge.com/en/LuminalVGD/) driver is licensed separately — AGPL-3.0 with a commercial option; see [`src/drivers/luminal-display/LICENSING.md`](src/drivers/luminal-display/LICENSING.md).
