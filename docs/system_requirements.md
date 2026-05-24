# System Requirements

LuminalShine is **Windows 11 only by design.** The Linux and macOS
code inherited from upstream Sunshine has been deprecated so the
team can focus entirely on the Windows pipeline. Multi-platform
support may return in the future but is not on the current roadmap.

## Host PC

This is the machine LuminalShine runs *on* — the PC whose games and
desktop you want to stream.

| Component | Minimum | Recommended |
| --- | --- | --- |
| **OS** | Windows 11 23H2 (x64) | Windows 11 24H2 or Insider Preview (Canary / Dev) |
| **CPU** | Quad-core x86-64 with AVX2 | 6+ cores; recent Intel / AMD / Snapdragon X |
| **GPU** | DirectX 12 capable, HEVC encode | NVIDIA RTX 40/50, AMD RDNA 3/4, or Intel Arc with AV1 encode |
| **RAM** | 8 GB | 16 GB+ |
| **Display stack** | WDDM 3.0 | WDDM 3.2+, HDR-capable physical or virtual display at 240 Hz |
| **Network** | 1 Gbps wired or Wi-Fi 6 | 2.5 Gbps wired or Wi-Fi 6E / 7 |
| **PowerShell** | Windows PowerShell 5.1 | [PowerShell 7](https://github.com/PowerShell/PowerShell) |
| **Privileges** | Local administrator (for install + service registration) | TPM 2.0 (used for credential sealing) |

> **PowerShell 7** supersedes Windows PowerShell as Microsoft's
> current shell. LuminalShine's MSI custom actions automatically
> prefer it when installed.

### Why these minimums

- **AVX2 / DirectX 12 / HEVC encode** — the capture and encode
  pipeline assumes all three. Anything older falls outside the
  tested matrix.
- **WDDM 3.0** — required for the Windows Graphics Capture
  pipeline that LuminalShine uses in service mode.
- **240 Hz display (recommended)** — needed if you want
  DLSS / FSR / XeSS frame-generated frames captured at full rate
  without micro-stutter. A virtual 240 Hz display via SudoVDA
  counts.
- **TPM 2.0** — optional but strongly recommended. Without a TPM,
  admin credentials fall back to Windows Credential Manager
  (still encrypted at rest, just not hardware-sealed).

## Hardware encoder support

LuminalShine probes for an encoder on startup and uses the first one
that works. All three major vendors are supported on Windows:

| Vendor | Family | Codecs |
| --- | --- | --- |
| **NVIDIA** | NVENC, GeForce GTX 9xx and newer | H.264, HEVC; AV1 on RTX 40/50 |
| **AMD** | AMF, RDNA / RDNA 2 / 3 / 4 | H.264, HEVC; AV1 on RDNA 3/4 |
| **Intel** | QuickSync / Arc | H.264, HEVC; AV1 on Arc |

NVIDIA **Smooth Motion** is supported on RTX 40 / 50 series.
**RTSS** and the **NVIDIA Control Panel** are integrated for frame
limiting and V-Sync handling before a stream starts.

## Network

- **Same LAN as your client(s).** WAN streaming works but is out of
  scope for the recommended setup — expect to tune buffers and
  pacing yourself.
- **Latency budget.** Wired Ethernet on both ends is the only way to
  get the smoothest experience; Wi-Fi 6E / 7 on the client comes
  close on a quiet RF environment.
- **Bitrate.** Default 20 Mbps suits 1080p60. Plan ≥50 Mbps for 4K60,
  ≥80 Mbps for HDR 4K120.

## Client devices

Anything that runs a [Moonlight](https://moonlight-stream.org/)
client works. Tested + supported:

- Android, iOS / iPadOS / tvOS, Windows, macOS, Linux, ChromeOS
- Xbox, LG webOS TVs (via NortheBridge's sibling project
  [Twilight](https://gitdocs.northebridge.com/)), Raspberry Pi,
  Nintendo Switch, PS Vita
- Any modern browser via the built-in WebRTC client at `/webrtc` —
  no client install required

See the [Moonlight client list](https://github.com/moonlight-stream)
for the full set.

## What you do **not** need

- A separate streaming server. LuminalShine runs on the gaming PC.
- A GeForce Experience / NVIDIA Shield. LuminalShine replaces them.
- A subscription, account, or login of any kind. There is no cloud
  component — pairing is peer-to-peer over your LAN.
- A second GPU. Capture and encode share the GPU you already game on.
