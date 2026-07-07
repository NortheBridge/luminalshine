# Architecture

This page sketches what's actually running on your PC when
LuminalShine is active. It is the conceptual map; the C++ API
reference at [/api/](./api/) is the source of truth.

## At a glance

LuminalShine is **one Windows service** (`LuminalShine.exe`,
optionally with a tray companion) that does five things in parallel:

```
                ┌────────────────────────────────────────────┐
   GPU display  │  Capture  →  Encode  →  Transport          │   Network
   ───────────► │   (WGC)     (NVENC /     (Classic Moonlight │   ─────────►
   GPU + WASAPI │             AMF / QSV)    *or* WebRTC)      │   Moonlight
                │                                              │   or browser
                │             Input replay  ◄────────────────  │   ◄────────
                └────────────────────────────────────────────┘
                              │
                              │  served by the
                              ▼
                      Web UI + REST API
                      (HTTPS on configured port)
```

| Stage | What happens | Where it lives |
| --- | --- | --- |
| **Capture** | Grab the display + system audio | [`src/video.cpp`](https://github.com/NortheBridge/luminalshine/tree/main/src/video.cpp), [`src/audio.cpp`](https://github.com/NortheBridge/luminalshine/tree/main/src/audio.cpp), [`src/platform/windows/`](https://github.com/NortheBridge/luminalshine/tree/main/src/platform/windows/) |
| **Encode** | Compress with the GPU's hardware encoder | [`src/nvenc/`](https://github.com/NortheBridge/luminalshine/tree/main/src/nvenc/), [`src/amf/`](https://github.com/NortheBridge/luminalshine/tree/main/src/amf/), [`src/video.cpp`](https://github.com/NortheBridge/luminalshine/tree/main/src/video.cpp) |
| **Transport** | Move encoded packets to the client | [`src/rtsp.cpp`](https://github.com/NortheBridge/luminalshine/tree/main/src/rtsp.cpp), [`src/stream.cpp`](https://github.com/NortheBridge/luminalshine/tree/main/src/stream.cpp), [`src/webrtc_stream.cpp`](https://github.com/NortheBridge/luminalshine/tree/main/src/webrtc_stream.cpp) |
| **Input replay** | Inject mouse / keyboard / gamepad from the client | [`src/input.cpp`](https://github.com/NortheBridge/luminalshine/tree/main/src/input.cpp), [`src/platform/windows/input.cpp`](https://github.com/NortheBridge/luminalshine/tree/main/src/platform/windows/input.cpp) |
| **Web UI + REST API** | Pair clients, configure apps, drive WebRTC signaling | [`src/confighttp.cpp`](https://github.com/NortheBridge/luminalshine/tree/main/src/confighttp.cpp), [`src_assets/common/assets/web/`](https://github.com/NortheBridge/luminalshine/tree/main/src_assets/common/assets/web/) |

## Process and thread model

LuminalShine runs as a **single Windows service process**. Long-lived
threads handle the three "front doors" the service listens on:

- **`nvhttp` thread** — the NVIDIA GameStream-compatible HTTP control
  plane that Moonlight clients pair against.
- **`confighttp` thread** — the HTTPS Web UI + REST API. Also hosts
  the WebRTC signaling endpoints under `/api/webrtc/`.
- **`rtsp_stream` thread** — the classic streaming media + control
  plane for paired Moonlight sessions.

When a client connects, **per-session threads** spin up: capture,
encode, audio, input, and (for WebRTC) feedback. They tear down when
the session ends.

Cross-thread coordination uses a typed "mailbox" abstraction
(`safe::mail_raw_t`) rather than ad-hoc condition variables. Each
session gets its own mailbox; WebRTC capture uses a dedicated one.

## Two transports, one at a time

LuminalShine speaks two protocols:

- **Classic Moonlight (RTSP + ENet).** What any Moonlight client uses
  by default. Pairing happens over the legacy NVIDIA HTTP control
  plane.
- **WebRTC.** Served at `/webrtc` in the built-in Web UI. Stream
  directly to any modern browser — no client install needed.

**The two are mutually exclusive.** If a classic session is active,
WebRTC capture refuses to start, and vice-versa. This is enforced
inside [`src/webrtc_stream.cpp`](https://github.com/NortheBridge/luminalshine/tree/main/src/webrtc_stream.cpp)
via a single `rtsp_sessions_active` flag toggled by
[`src/stream.cpp`](https://github.com/NortheBridge/luminalshine/tree/main/src/stream.cpp).

## Capture: WGC in service mode

LuminalShine uses **Windows Graphics Capture (WGC)** running inside
the Windows service. That gives it three properties the typical
user-session capture can't deliver:

- **Higher throughput.** Captures full frame-generated frame rates
  (DLSS / FSR / XeSS) at the display's native cadence.
- **Survives VRAM exhaustion.** Falls back gracefully instead of
  dropping the session.
- **Keeps the lock screen, login screen, and UAC prompts
  capturable.** The session continues across the secure desktop.

If WGC is unavailable for any reason, the capture layer falls back
to the older Desktop Duplication API.

## Virtual display drivers

For headless setups, hybrid-GPU laptops, or any case where you don't
want to use the physical display, LuminalShine ships virtual display
driver support:

- **SudoVDA** *(default, enabled)* — automatic or explicit GPU
  binding, hybrid-GPU support, headless-friendly.
  configurations. Read the installer notice before selecting it.
- **LuminalVGD** *(in development)* — a first-party driver from
  NortheBridge that will eventually supersede both.

Display layout is restored automatically after hard crashes, reboots,
or unreleased dummy plugs via the **Display Setting Automation**
subsystem.

## Encoding

LuminalShine probes the host's encoders at startup and picks the
first usable one. The pipeline can drive **NVENC** (NVIDIA), **AMF**
(AMD), or **QuickSync** (Intel). Modern codecs are the default:
**HEVC and AV1**, with **HDR** support; **H.264** remains available
for older builds and clients.

Frame pacing is co-operative with **RivaTuner Statistics Server
(RTSS)** and the **NVIDIA Control Panel**: LuminalShine sets the
right frame limit and disables V-Sync before a stream starts.

## Credentials

Admin credentials for the Web UI are managed by the
[`src/cred_store/`](https://github.com/NortheBridge/luminalshine/tree/main/src/cred_store/)
subsystem, which provides a three-layer recovery path:

1. **TPM sealing** *(Windows default if TPM 2.0 is available)* — a
   2048-bit RSA key bound to the host's TPM wraps the credential.
2. **Platform-native secret storage** — Windows Credential Manager,
   macOS Keychain, libsecret on Linux.
3. **Encrypted file backend** — fallback if neither of the above is
   reachable.

Password hashing uses **Argon2id** (memory-hard) rather than the
legacy SHA-256 path inherited from upstream. Tuning parameters
(`argon2_m_cost_kib`, `argon2_t_cost`, `argon2_parallel`) live in
the config file.

Health is exposed at `GET /api/health/cred-store` for monitoring
and migration tooling.

## What's *not* in this picture

- **No cloud service.** Pairing is peer-to-peer over your LAN. There
  is no NortheBridge backend.
- **No telemetry by default.** Analytics, crash reports, and update
  checks are opt-in.
- **No second process per session.** Capture / encode / transport
  all happen inside the main service; the only per-launch external
  process is your game.

## Where to go next

- Want to actually run it? **[Quick Start](quick_start.md)**.
- Want the per-field config reference? **[Configuration](configuration.md)**.
- Want symbol-level detail? The Doxygen browser at **[/api/](./api/)**.
