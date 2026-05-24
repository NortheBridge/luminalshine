# Introduction

**LuminalShine is a self-hosted, low-latency game streaming host for
modern Windows 11.** It runs on the PC you want to stream *from*, and
any [Moonlight](https://moonlight-stream.org/) client — phone, tablet,
TV, handheld, browser — can connect to it and play your library
remotely with near-local latency.

It is maintained by the [NortheBridge
Foundation](https://gitdocs.northebridge.com/) and is free and open
source under the [GNU GPL-v3](https://github.com/NortheBridge/luminalshine/blob/main/LICENSE).

## Where LuminalShine fits

| Component | What it does | Where it runs |
| --- | --- | --- |
| **LuminalShine** | Captures, encodes, and streams the host's display + audio; replays remote input | Windows 11 gaming PC |
| **Moonlight client** | Decodes the stream, sends input back | Phone, tablet, TV, browser, handheld, etc. |
| **WebRTC client** *(built-in)* | Same as above, but in any modern browser — no install | Any browser at `/webrtc` |

If you have ever used NVIDIA GameStream, LuminalShine is the same
shape: a host on your gaming PC, a client somewhere else. The
difference is that LuminalShine is open-source, vendor-neutral
(NVIDIA / AMD / Intel encoders all supported), and works on any
modern GPU.

## Why a separate project

LuminalShine is a hardened, Windows-first descendant of
[Sunshine](https://github.com/LizardByte/Sunshine) by way of
Vibeshine. It exists because:

- **Windows 11 Insider Preview support.** The Canary and Dev channels
  ship `dxgi.dll` changes that broke upstream capture; LuminalShine
  works around them and tracks new flights as they release.
- **WGC capture in service mode.** Higher throughput, captures
  frame-generated frames at full rate, survives VRAM exhaustion, and
  keeps the login screen and UAC prompts capturable.
- **Hardened credential storage.** Admin credentials are sealed to
  the TPM by default on Windows, with platform-native fallbacks and
  a three-layer recovery flow. Argon2id replaces SHA-256 for the
  password KDF.
- **First-party virtual display work.** SudoVDA ships and is enabled
  by default; MTT VDD is available; the in-house LuminalVGD driver
  is in development.
- **Single-platform focus.** Linux and macOS code from upstream
  Sunshine has been deprecated so the team can invest fully in the
  Windows pipeline.

LuminalShine is a **complementary fork**, not a replacement. Sunshine
remains the right choice for cross-platform deployments. **Features
do not flow back upstream** — the architecture has diverged too far
for clean merges.

## A note on the name

"LuminalShine" is the adjective form of *lumen* — the SI unit for
luminous flux — riffing on the first half of *Sunshine*.

## Next steps

- New here? Start with the **[Quick Start](quick_start.md)** for a
  10-minute happy-path setup.
- Want to know if your hardware works? See **[System
  Requirements](system_requirements.md)**.
- Want to understand what's running on your PC? Read the
  **[Architecture](architecture.md)** overview.
- Just want a glossary of the terms used throughout these docs?
  See **[Concepts](concepts.md)**.
