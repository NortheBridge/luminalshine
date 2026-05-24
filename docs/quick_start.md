# Quick Start

The 10-minute happy path. If you hit anything that doesn't match,
fall back to the longer **[Getting Started](getting_started.md)**
walkthrough.

## Before you begin

- A Windows 11 PC that meets the **[System
  Requirements](system_requirements.md)**.
- Local administrator access on that PC.
- A second device on the same LAN to stream to (phone, tablet, TV,
  laptop, or just another browser).

## 1. Install LuminalShine on the host

1. Grab the latest installer from
   [github.com/NortheBridge/luminalshine/releases](https://github.com/NortheBridge/luminalshine/releases/latest).
   The MSI bootstrapper (`LuminalShineInstaller.exe`) is the
   recommended package.
2. Run it as administrator. Accept the defaults — they install the
   service, register it to start on boot, and install **SudoVDA** as
   the default virtual display driver.
3. When the installer finishes, it opens the LuminalShine Web UI at:

   ```
   https://localhost:47990
   ```

   Your browser will warn about the self-signed certificate. That is
   expected on first run; accept it and continue.

## 2. Set your admin credentials

The first time the Web UI loads, it prompts you to create an admin
username and password. These are:

- Stored using **TPM-bound credential sealing** if your PC has TPM
  2.0 (the default on modern Windows 11 hardware).
- Falling back to Windows Credential Manager + an Argon2id-hashed
  password if no TPM is available.

Pick a password you can recover — there is no "forgot password"
email flow. If you do lose it, the installer ships a **Reset
LuminalShine Admin Password** shortcut.

## 3. Install a Moonlight client

On the device you want to stream *to*, install [Moonlight from
moonlight-stream.org](https://moonlight-stream.org/) — it has builds
for Android, iOS, Windows, macOS, Linux, ChromeOS, TVs, handhelds,
and more.

If you'd rather not install anything, skip this step and use the
**WebRTC client** built into the LuminalShine Web UI — see step 5.

## 4. Pair the client

1. Open Moonlight on the client device. It should auto-discover the
   host on your LAN. If not, type the host's IP manually.
2. Tap the LuminalShine host. Moonlight shows a 4-digit PIN.
3. Back in the LuminalShine Web UI on the host PC, go to
   **PIN** in the sidebar and enter the PIN.
4. Pairing succeeds — Moonlight now shows the list of apps configured
   on your host (defaults include **Desktop** and **Steam**).

Pairing happens once per client. Subsequent connections are
automatic.

## 5. Start streaming

**With Moonlight:** tap **Desktop** (or any app) on the client and
the stream begins.

**With the built-in WebRTC client:** in the Web UI on the host, click
**WebRTC** in the sidebar, pick an app, and click **Connect**. The
stream renders in the same browser tab — no client install required.

That's it. You're streaming.

## What to do next

- **Tune the stream.** Bitrate, codec (H.264 / HEVC / AV1), HDR, and
  frame pacing all live under **Configuration → Video** in the Web
  UI, and are documented in **[Configuration](configuration.md)**.
- **Add games.** **Configuration → Applications** lets you add
  custom launches; Steam and Playnite integration auto-import your
  library.
- **Set up a virtual display.** If you'd rather not stream your real
  desktop, the SudoVDA driver lets you spin up a dedicated streaming
  display at any resolution / refresh rate. See **Configuration →
  Virtual Display**.
- **Hit problems?** Check **[Troubleshooting](troubleshooting.md)**
  or the logs at `%ProgramData%\LuminalShine\sunshine.log`.
