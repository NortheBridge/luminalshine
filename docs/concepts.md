# Concepts

A short glossary of the terms used throughout these docs. Skim it
on your way in; come back when something is unfamiliar.

## Roles

**Host**
: The PC running LuminalShine. The thing you stream *from*.

**Client**
: A device running [Moonlight](https://moonlight-stream.org/) or the
  built-in WebRTC client. The thing you stream *to*.

**Service**
: LuminalShine itself, running as a Windows service. Starts on boot,
  runs without a user logged in, and stays alive across the lock
  screen.

**Tray companion**
: An optional user-session helper that exposes service status in the
  Windows notification area.

## Sessions and pairing

**Pairing**
: The one-time handshake between a host and a client. The client
  presents a 4-digit PIN; you type it into the host's Web UI. After
  that, the client can connect without prompting.

**Session**
: A single live stream from host to client. Each session has its
  own capture, encode, transport, and input threads on the host.

**App**
: A configured launch target on the host. Comes in three flavors:
  the built-in **Desktop** app (streams whatever's on screen), a
  **process app** (launches a binary), or an imported entry from
  Steam / Playnite.

**Resume**
: Reconnecting to an in-flight session. If the client drops, the host
  keeps the app running and lets you reconnect without restarting it.

## Transports

**Moonlight / classic transport**
: The original streaming protocol — NVIDIA-GameStream-compatible
  HTTP control plane plus RTSP + ENet for media. What any installed
  Moonlight client uses by default.

**WebRTC**
: An alternate transport that streams to a browser at
  `https://<host>:47990/webrtc`. No client install needed. Mutually
  exclusive with the classic transport — only one is active at a
  time per host.

## Display and capture

**Display capture**
: Reading the host's framebuffer in real time. LuminalShine prefers
  **Windows Graphics Capture (WGC)** running in service mode and
  falls back to the older Desktop Duplication API if WGC is
  unavailable.

**WGC** (Windows Graphics Capture)
: The modern Windows capture API. In LuminalShine's service-mode
  configuration, it survives VRAM exhaustion, captures
  frame-generated frames at full rate, and keeps the lock screen
  and UAC prompts capturable.

**VDD** (Virtual Display Driver)
: A driver that creates a fake monitor for LuminalShine to render
  to. Useful on headless rigs, hybrid-GPU laptops, or whenever you
  don't want to mirror your real desktop. LuminalShine ships with
  three options:

  - **SudoVDA** — the default; flexible, hybrid-GPU aware.
  - **MTT VDD** — alternative for niche hardware.
  - **LuminalVGD** — NortheBridge's in-house driver, in development.

**Display Setting Automation**
: The subsystem that restores your display layout after crashes,
  reboots, or unreleased virtual displays.

## Encoding

**Encoder**
: The hardware block on your GPU that compresses video frames in
  real time. LuminalShine drives **NVENC** (NVIDIA), **AMF** (AMD),
  or **QuickSync** (Intel).

**Codec**
: The compression format used for the video stream. LuminalShine
  supports **H.264** (universal), **HEVC** (better quality per bit,
  default), and **AV1** (best quality per bit, requires recent
  hardware on both ends).

**Bitrate**
: How much data the encoder produces per second, in Mbps. Higher =
  better picture, more network. Defaults sit around 20 Mbps for
  1080p60; budget ≥50 Mbps for 4K60.

**HDR**
: High dynamic range. Requires HDR-capable display, HEVC or AV1
  codec, and an HDR-capable client.

**Frame pacing**
: The timing of when each encoded frame leaves the host. LuminalShine
  cooperates with **RTSS** and the **NVIDIA Control Panel** to apply
  the correct frame limit and disable V-Sync before a stream starts.

## Performance features

**Frame generation**
: GPU-side techniques (DLSS-FG, FSR Frame Generation, XeSS-FG,
  NVIDIA Smooth Motion, Lossless Scaling) that synthesize
  intermediate frames between rendered ones. LuminalShine has
  specific capture-side fixes so generated frames stream at full
  rate.

**Smooth Motion**
: NVIDIA's driver-level frame generation, supported on RTX 40/50
  series. Treated as a per-app toggle in LuminalShine.

## Security

**Credential store** (`cred_store`)
: The subsystem that holds admin credentials for the Web UI. Three
  layers in order of preference: **TPM sealing** (Windows + TPM 2.0)
  → **platform secret storage** (Credential Manager / Keychain /
  libsecret) → **encrypted file backend**.

**TPM sealing**
: Wrapping a credential with a 2048-bit RSA key bound to the host's
  Trusted Platform Module. Means the credential can only be
  unwrapped on the same physical machine.

**Argon2id**
: The memory-hard password-hashing function LuminalShine uses for
  the admin password. Tunable via `argon2_m_cost_kib`, `argon2_t_cost`,
  `argon2_parallel` in the config.

**Scoped API token**
: An API credential that grants access to a specific set of REST
  methods, not full admin. Use these for automation so scripts
  never need the admin password.

**Session token**
: The HttpOnly cookie issued after Web UI login. Carries the active
  session for browser interactions; expires per the session policy.

## File and config layout

**Config file**
: `%ProgramData%\LuminalShine\sunshine.conf`. Text format,
  one `key = value` per line. The full key reference lives in
  **[Configuration](configuration.md)**.

**Credentials file**
: `%ProgramData%\LuminalShine\sunshine_credentials.json`. Separate
  from the main config so it can be backed up (or rotated)
  independently.

**Logs**
: `%ProgramData%\LuminalShine\sunshine.log`. Rotated per session.

**State**
: `%ProgramData%\LuminalShine\sunshine_state.json`. Pairing data,
  app metadata, runtime state.
