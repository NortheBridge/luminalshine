# PyroWave (vendored)

A self-contained copy of the **PyroWave** GPU wavelet video codec by
Hans-Kristian Arntzen, extracted from WiVRn's `proto/pyrowave` branch and
decoupled from WiVRn's build/include tree so it can be linked into both
**Sunshine** (CMake) and **moonlight-qt** (qmake).

PyroWave is **not** a CPU codec. The entire encode/decode pipeline runs on the
GPU as Vulkan compute and fragment shaders. There is no ffmpeg path: you feed it
a Vulkan YCbCr image and it produces a packetized bitstream, and vice-versa.

This directory is the *foundation* milestone: the codec library compiles and
links as a standalone unit. The Sunshine encoder and moonlight-qt decoder
integrations build on top of it.

## Layout

```
pyrowave/
  src/pyrowave/      pyrowave_common/encoder/decoder.{h,cpp}   (the codec)
  src/vk/            VMA allocation layer vendored from WiVRn
                     allocation, vk_allocator, check, error_category, vk_mem_alloc.cpp (VMA impl TU)
  src/utils/         singleton.h
  external/          vk_mem_alloc.h (VMA single header)
  shaders/           *.comp / *.glsl / *.inc  (GLSL source)
  tools/             generate_shaders.py  (GLSL -> SPIR-V -> C++ table)
  CMakeLists.txt     static-lib target `pyrowave`        (Sunshine / standalone)
  pyrowave.pro/.pri  static-lib subproject `pyrowave`    (moonlight-qt)
```

Only four files were taken from WiVRn besides the codec itself: `vk/allocation`,
`vk/vk_allocator`, `vk/check` (+`error_category`), and `utils/singleton`. The
shader build step (WiVRn's `CompileGLSL.cmake`) is replaced by
`tools/generate_shaders.py` so CMake and qmake share one implementation.

## Build dependencies

* A C++20 compiler (the public headers use `std::span`, `std::format`,
  designated initializers).
* Vulkan headers including `vulkan/vulkan_raii.hpp` (Vulkan SDK, or the
  `vulkan-headers` package).
* **Python 3** and **glslangValidator** on `PATH` at build time (shader
  compilation). Override with `-DPYROWAVE_GLSLANG=...` / `PYROWAVE_GLSLANG=...`.

## Enabling the build

Sunshine (CMake):

```
cmake -DSUNSHINE_ENABLE_PYROWAVE=ON ...
```

Adds the `pyrowave` static lib, links it into `sunshine`, and defines
`SUNSHINE_ENABLE_PYROWAVE=1` for guarding the (future) encoder code.

moonlight-qt (qmake):

```
qmake CONFIG+=pyrowave
```

Builds the `pyrowave` static-lib subproject, links it into `app`, bumps the app
to C++20, and defines `HAVE_PYROWAVE=1`.

Both flags default **off** so existing builds are unaffected until the
encoder/decoder integrations land.

## Public API surface

```cpp
#include "pyrowave/pyrowave_encoder.h"   // PyroWave::Encoder   (server side)
#include "pyrowave/pyrowave_decoder.h"   // PyroWave::Decoder   (client side)
#include "pyrowave/pyrowave_common.h"    // shared types, ChromaSubsampling
```

Encoder (server) — one `encode` per frame, then packetize for the network:

```cpp
PyroWave::Encoder enc(physicalDevice, device, width, height,
                      PyroWave::ChromaSubsampling::Chroma420);
enc.encode(cmd, {viewY, viewCb, viewCr}, bitstreamBuffers);   // records GPU work
// after the GPU is done and meta/bitstream buffers are host-visible:
size_t n = enc.compute_num_packets(meta, packetBoundary);
enc.packetize(packets, packetBoundary, out, outSize, meta, bitstream);
```

Decoder (client) — accumulate packets into a `DecoderInput`, then `decode`:

```cpp
PyroWave::Decoder dec(physicalDevice, device, width, height,
                      PyroWave::ChromaSubsampling::Chroma420, /*fragment_path=*/true);
PyroWave::DecoderInput input(dec);
for (auto& pkt : packets) input.push_data(pkt);
dec.decode(cmd, input, {viewY, viewCb, viewCr});   // records GPU work
```

The WiVRn wrappers `server/encoder/video_encoder_pyrowave.cpp` and
`client/decoder/pyrowave/decoder.cpp` (left in the WiVRn checkout) are the
reference for how to drive this API; the Sunshine/moonlight integrations adapt
that logic to each app's capture/present and networking.

## Integration contract — READ THIS

1. **VMA singleton (required).** The vendored allocation layer reaches a global
   `vk_allocator` via `vk_allocator::instance()`. The host **must construct
   exactly one** `vk_allocator` after creating the Vulkan device and **before**
   constructing any `PyroWave::Encoder`/`Decoder`, and keep it alive for their
   lifetime:

   ```cpp
   VmaAllocatorCreateInfo aci{ .physicalDevice = phys, .device = dev,
                               .instance = inst, .vulkanApiVersion = VK_API_VERSION_1_3 };
   vk_allocator allocator(aci, /*has_debug_utils=*/false);   // singleton
   ```

2. **One VMA implementation.** `src/vk/vk_mem_alloc.cpp` defines
   `VMA_IMPLEMENTATION`. If the host already links a VMA implementation (e.g.
   moonlight-qt via libplacebo) and you hit duplicate symbols, define
   `PYROWAVE_NO_VMA_IMPL` (qmake) / drop that TU and let the host's VMA satisfy
   the symbols.

3. **Vulkan dispatch.** The codec uses `vulkan_raii.hpp`. Make sure the module
   and host agree on the dispatcher configuration
   (`VULKAN_HPP_DISPATCH_LOADER_DYNAMIC`). Mismatches surface as link/runtime
   dispatch errors.

4. **Device features the codec assumes.** A Vulkan 1.3 device with subgroup
   support; `shaderFloat16` enables the faster `*_fp16` shader paths (optional —
   there are non-fp16 fallbacks). `PYROWAVE_PRECISION` (compile-time, default 1;
   env override `PYROWAVE_PRECISION`) selects fp16/fp32 wavelet precision.

5. **Image formats.** Encoder input is a 3-plane YCbCr image (e.g.
   `G8_B8_R8_3PLANE_420`) viewed as three single-plane image views
   (Y = R8, Cb/Cr = R8 from plane1). Decoder output is the same, written via
   per-plane storage views. Match the WiVRn wrappers for the exact view setup.

## What is intentionally *not* here yet

* No codec negotiation in the GameStream/Moonlight protocol (Sunshine
  `videoFormat` / moonlight-common-c `VIDEO_FORMAT_*`).
* No Sunshine `encode_session_t` / capture→Vulkan-image plumbing.
* No moonlight-qt decoder + Vulkan present path.

Those are the next milestones and depend on this library being linkable.

## Provenance / licensing

Codec sources and shaders: © 2025 Hans-Kristian Arntzen, **MIT** (see SPDX
headers). The vendored `vk/` and `utils/singleton.h` files are from **WiVRn**
(GPL-3.0-or-later) — check WiVRn's headers. `external/vk_mem_alloc.h` is AMD's
Vulkan Memory Allocator (MIT). Verify license compatibility with each host
project before shipping.
