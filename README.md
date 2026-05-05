# LuminalShine

## What is LuminalShine?

<<<<<<< HEAD
AI-enhanced, LuminalShine by the NortheBridge Foundation (historically published as "NortheBridge North America") is a modern version of Vibeshine, a fork of Sunshine, a multi-platform popular remote streaming application. It's objective is to offer a Modern Sunshine Experience on Modern Windows including Windows Insider Preview through the Canary Builds.
=======
AI-enhanced, LuminalShine by the NortheBridge Foundation (historically published as "NortheBridge North America") is a modern version of Vibeshine, a fork of Sunshine, a multi-platform popular remote streaming application. Its objective is to offer a Modern Sunshine Experience on Modern Windows including Windows Insider Preview through the Canary Builds.
>>>>>>> c57ff3520ec4e620d9942a43492f0535b0ff1107

### Foundation of LuminalShine
LuminalShine was created as a direct response to the Vibeshine developers not supporting Windows Insider Preview Builds in the slightest and simply dismissing errors that could and would eventually have to be rectified in order for them to release on Windows Next.

**Among the Many Changes**: Support for multi-platform is deprecated to allow developers and contributors to solely focus on the Windows platform ___but it is possible for other platforms to return in the future___, advanced support for Windows Insider Preview releases, and the removal of large amounts of "dead code" as well as a unique new development build framework.

### Our Promise
<<<<<<< HEAD
The NortheBridge Foundation promises that this software will remain fully supported for as long as Sunshine is supported and remain free as well as open source under the GNU GPL-v3 License and will not be available for commercial purposes. This includes subrelated projects such as **the future LuminalShine Virtual Graphics Drives (LuminalShine VGD) that will eventually replace the SudoVDA and MTT VDD drivers included with the current release of LuminalShine.
=======
The NortheBridge Software Foundation promises that this software will remain fully supported for as long as Sunshine is supported and remain free as well as open source under the GNU GPL-v3 License, and will not be available for commercial purposes. This includes subrelated projects such as **the future LuminalShine Virtual Graphics Drives (LuminalShine VGD) that will eventually replace the SudoVDA and MTT VDD drivers included with the current release of LuminalShine.
>>>>>>> c57ff3520ec4e620d9942a43492f0535b0ff1107



## Key Features

* **Native Windows Current Release & Insider Preview Support**
  LuminalShine is a single platform project developed from Vibeshine and Sunshine with the express purpose of providing a "Modern Sunshine Game Host Experience for Moonligh on Modern Windows." We, therefore, can focus on Windows development and issues. This also includes removal of 'dead code' and Windows-only enhancements.

* **HEVC and AV1 First Support**
  A result of supporting the latest version of Windows Canary Builds, and due to an error in Microsoft's _dxgi.dll_, LuminalShine supports HEVC and AV1 with HDR streaming first. Windows releases based on 24H2, or releases outside Windows Insider Preview, should still work with H.264 **but we recommend still using _HEVC_ or _AV1_ modern codecs.

* **Native SudoVDA and MTT VDD Display Support**
  LuminalShine includes SudoVDA by default, with multiple stability improvements. It can capture output from any GPU, automatically or by specification, including those in hybrid laptops, ensuring the virtual screen connects to the correct GPU when needed. It also provides simple virtual display options, allowing users to choose between a physical or virtual display. On headless setups, it automatically enables the prevention of 503 errors and false encoder detections, such as incorrect HEVC support reports.

  **MTT VDD is included for compatibility and you should read the notice in the LuminalShine Installer before deciding to select MTT VDD**

* **Windows Graphics Capture (WGC) in Service Mode**
  Running Windows Graphics Capture (WGC) as a service improves performance and stability. It captures the full frame rate of frame‑generated titles, avoids crashes when VRAM is exceeded, and follows Microsoft’s recommended capture method going forward. LuminalShine auto‑switches capture methods on demand, so the login screen and UAC prompts are still captured even when using WGC.

* **Native Virtualized Display**
  LuminalShine includes SudoVDA by default, with multiple stability improvements. It can capture output from any GPU, including those in hybrid laptops, ensuring the virtual screen connects to the correct GPU when needed. It also provides simple virtual display options, allowing users to choose between a physical or virtual display. On headless setups, it automatically enables the prevention of 503 errors and false encoder detections, such as incorrect HEVC support reports.

* **Display Setting Automation**
  LuminalShine adds multiple safeguards to prevent dummy plugs or virtual displays from not being properly released when you return to your PC. It resolves common Windows 11 **24H2** and **Insider Preview** display issues with restores of your layout after hard crashes, shutdowns, or reboots. (**Currently**: the only scenario it can’t restore is during a user logout.) The flow is simplified to a dropdown—just pick the display you want to stream.

* **Modern Frontend WebUI with Full Mobile Support**
  The modern Web UI makes it easy to add games and change settings without restarting the program. It’s fully responsive, so you can manage your library and configuration from a phone or tablet.

* **WebRTC Browser Streaming**
  LuminalShine can stream straight to your web browser from the `/webrtc` page, so you can play without installing a separate client. It is designed for fast response and smooth audio/video, while still letting you use the regular Moonlight-compatible streaming path if you prefer.

* **RTSS & NVIDIA Control Panel Integration**
  LuminalShine can manage RTSS to apply the correct frame limit and disable V‑Sync before streaming, significantly improving frame pacing and smoothness. The applied frame cap matches the client device’s requested FPS. _Please Note: RTSS isn't installed by LuminalShine and must be obtained through the RTSS Official Download or MSI Afterburner._

* **Frame‑Gen Capture Fixes**
  LuminalShine includes workarounds so DLSS/FSR frame-generation games are captured at the game’s full frame rate without micro‑stutter. This requires a very high‑refresh‑rate display (physical or virtual) at **240 Hz**.

* **Lossless Scaling & NVIDIA Smooth Motion**
  LuminalShine can automatically apply optimal Lossless Scaling settings to generate frames for any application. On RTX 40‑series and RTX 50-series or newer GPUs, you can optionally enable **NVIDIA Smooth Motion** for better performance and image quality.

* **Playnite Integration**
  Deep integration with Playnite (a “launcher of launchers”) automatically syncs your recently played games with configurable expiration rules, per‑category sync, and exclusions. You can also add games manually from a Web UI dropdown; LuminalShine handles artwork, launching, and clean termination. The goal is a seamless, GeForce Experience–style library experience—only better. **We, however, recommend using Steam Big Picture Mode for the best experience.**

* **API Token Management**
  Access tokens can be tightly scoped—down to specific methods—so external scripts don’t need full administrative rights. This is a  security improvement while keeping automations flexible.

* **Session‑Based Authentication**
  The sign‑in flow supports password managers and includes a “remember me” option to minimize prompts. The experience is security‑hardened without sacrificing convenience.

* **Update Notifications**
  Built‑in notifications let you know when new features or bug fixes are available, making it easy to stay current. We offer **Pre-Release Notifications side-by-side with Release Notifications**. _It is recommended that you turn Pre-Release Notifications on in case a feature in development or a bug fix you need has already been implemented. LuminalShine will always prompt to download the latest version, whether it's Pre-Release or General Availability._

---

## Does LuminalShine aim to replace Sunshine or Vibeshine?

LuminalShine is a **complementary fork** of Sunshine and Vibeshine intended to provide the best Sunshine experience on Modern Windows 11 Systems.


## Will LuminalShine’s features merge back into Sunshine or Vibeshine?

**Answer: No, LuminalShine will not be backported to Sunshine or Vibeshine. Significant changes, including driver updates, and the pace of development would make it nearly impossible to maintain multiple backports.**

LuminalShine is based on the **Vibeshine Codebase**, which was largely AI‑generated. While it works well, it carries a kind of surface‑level technical debt that many upstream projects want resolved before taking big changes (styling consistency, thin/missing docs, and some over‑engineering). This debt is relatively unimportant today because modern AI tools can answer “why does this function exist?”, “What does this parameter do?”, or “how do these classes interact?” and will soon auto‑fix these issues—re‑style trees, write docstrings, and prune unused layers—without human effort.

LuminalShine, however, looks to rectify this debt as development continues on the software to bring it up to par with something not simply "Vibe-coded" like Vibeshine by creating an interactive, single pane of glass, unified experience.

<<<<<<< HEAD
Because the Vibeshine codebase was studied before the NortheBridge Foundation defined the new architecture, we know how everything works. We aim to polish the code and documentation but sometimes, due to legacy components, this is not always possible.
=======
Because the Vibeshine codebase was studied before the NortheBridge Software Foundation defined the new architecture, we know how everything works. We aim to polish the code and documentation, but sometimes, due to legacy components, this is not always possible.
>>>>>>> c57ff3520ec4e620d9942a43492f0535b0ff1107

---

## Origin of the Name "LuminalShine"

The name "LuminalShine" emerged from the adjective form of the noun "Lumen," which in physics is the SI Unit for luminous flux - or light emitted. It's a play on the first part of the name of "Sunshine."

---

## Why Use AI‑assisted generated Code?

Broadly speaking, AI-assisted code development is becoming more and more common, whether it is in the implementation of Operating Systems or simple applications. By using AI-assisted code development, we are able to speed up the process of developing and releasing software updates that include new features, bug fixes, and more.

As we understand the architecture of the software, we guide our AI to generate AI-assisted code to ensure that the architecture not only remains intact but that we don't introduce new bugs while maintaining a cadence equal to the Windows Insider Preview program when required.

---

## AI Models Used by LuminalShine

LuminalShine is built using the latest **Claude** Models as the primary workflow, and that has meant significant changes to the code since it was forked from Vibeshine, which depended on **GPT-5.3-Codex**, including the rectification of serious errors, 'dead code,' and ensuring compatibility of threads with the latest versions of Windows.

Previously, Vibeshine, the project which LuminalShine was forked from, has always been built with **Codex** as the primary workflow, and in practice that has meant mostly the **GPT‑5 family** (at the time: **GPT‑5.3‑Codex**). 

An older version of Claude was used by Vibeshine more heavily earlier on. Older Claude models had a tendency to go off on their own path, even when the architectural plan was clear. That behavior has mostly been fixed in newer Claude releases, but GPT, which was at the time Vibeshine was being developed, ended up being the more useful engineering tool.

In general, with LuminalShine, the latest **Claude** Models are exclusively used for architectural, codebase development, and design development choices.
