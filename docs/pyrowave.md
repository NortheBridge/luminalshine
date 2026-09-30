# Experimental PyroWave on Windows

The first integration targets 700 Mbps SDR BT.709 and HDR10 BT.2020/PQ,
4:2:0, through LuminalShine's existing GPU capture path. It does not require a
LuminalVGD driver update. H.264, HEVC and AV1 keep their encoder selection,
capability snapshots and transport defaults.

## Pinned client and codec

- Aurora (Moonlight Qt fork): `Koloses/aurora-qt` at
  `2c574a9e79da5b3fa95bab90ffd3d322f5d63ac0`.
- Its protocol submodule: `Koloses/moonlight-common-c` at
  `a5228faeeda949a3ab0140f55a75bd14fe74e899`.
- Vendored codec and initial host adapter reference: `Koloses/Solarflare` at
  `48ae555e73428926ae4be234f15513967a839397`.

The host uses stream format 3, SDP `PYROWAVE/90000`, SDR server capability
`0x00800000`, and HDR10 capability `0x02000000`. It does not advertise 4:4:4,
adaptive replenishment, adaptive bitrate, or the client's timing-feedback
extensions. Every transmitted frame is a complete intra frame. A stock
Moonlight client continues to negotiate a regular codec.

## Building and selecting

Use the normal Windows clang build with Vulkan headers (including Vulkan-Hpp),
Python 3 and glslangValidator available:

```powershell
cmake -S . -B build -DSUNSHINE_ENABLE_PYROWAVE=ON -DBUILD_DOCS=OFF
cmake --build build --target sunshine test_sunshine
```

Optional CMake overrides: `Vulkan_INCLUDE_DIR`, `Vulkan_LIBRARY`,
`PYROWAVE_GLSLANG`. Shaders are compiled and embedded at build time. The system
GPU driver supplies the Vulkan loader at runtime; no Vulkan SDK is needed on
the streaming host. PyroWave requires Vulkan 1.3 and Windows external-memory /
keyed-mutex interoperability on the capture adapter.

In Advanced settings enable experimental PyroWave and retain 700 Mbps, or set:

```ini
pyrowave_enabled = enabled
pyrowave_bitrate_mbps = 700
```

Select PyroWave in the pinned client and leave 4:4:4 disabled. Start with
1920x1080 at 60 FPS, then test the intended resolution and frame rate. For HDR,
enable HDR in the client and ensure display preparation enables HDR on the
LuminalVGD monitor. A requested HDR stream with an SDR capture source fails
setup rather than falsely labelling SDR pixels as HDR.

The target describes compressed video, not total link traffic. FEC/audio/header
overhead requires additional capacity. Use a network with sufficient headroom.
Zero selects the 700 Mbps automatic default. The UI accepts up to 10,000 Mbps,
but the current four-block GameStream transport cannot carry all combinations
of bitrate and FPS: excessive frame budgets are rejected before capture.
Large frames may use the existing no-FEC fallback. Full 10 Gbps transport and
client-coordinated fragmentation are a separate milestone.

## Capture and recovery contract

LuminalVGD completes its copy and releases the driver ring slot before the
encoder receives the host-owned pooled D3D11 texture. PyroWave imports that NT
shared texture on the same adapter (matched by LUID), acquires/releases key 0
through Vulkan's keyed-mutex submit extension, and finishes source consumption
before returning the borrowed image. No uncompressed CPU readback is used.

The existing driver D3D11 and D3D12 fence transports converge on that pooled
image. NVENC's separate D3D11On12/native D3D12 encoder route is unchanged;
PyroWave compute itself uses Vulkan. FP16 scRGB capture is converted from
linear BT.709 at 80 nits/unit to BT.2020/PQ for HDR. The capture layer already
composites the cursor. Static desktops re-encode retained YCbCr planes, without
retaining a driver ring slot or pooled image.

Worker IPC version 9 carries the negotiated packet-size bound. The three
regular codec capability arrays and the 32 MiB worker message cap remain
unchanged. The experimental codec has a separate bounded dispatch. Pending
GPU resources are retained on fence timeout until worker exit.

## Verification

Run the regular guard suite and the new contract checks after each change:

```powershell
.\build\tests\test_sunshine.exe --gtest_filter='Vgd*:VideoWorker*:WebRtcExclusivity.*:VideoPacketQueue.*:VideoPacketQos.*:PyrowaveContract.*:PyrowavePacing.*'
```

The opt-in `PyrowaveGpu.*` checks exercise actual D3D11 texture import and SDR /
HDR encoding without modifying displays. They require
`LUMINALSHINE_TEST_PYROWAVE_GPU=1` and a build with PyroWave enabled. They are not
a substitute for a LuminalVGD-to-Moonlight session.

Hardware acceptance still requires the pinned client: SDR and HDR playback,
color ramps/highlights, cursor, idle desktop, reconnect, resolution/HDR changes,
display restoration, and regular-codec sessions before and after PyroWave.
Repeat on NVIDIA, AMD and Intel where the required Vulkan interop is available.
Record actual bitrate, dropped frames and latency; do not infer performance
from a successful build or capability probe.

## Development checkpoint (2026-09-29)

The Windows host and test executable build with PyroWave enabled, and the web UI
production build succeeds. The final targeted run passed 87 tests: existing
VGD/worker/transport guards, configuration consistency, PyroWave contract/pacing,
and the opt-in GPU test. The GPU test uses 128x128 shared textures at a 700 Mbps
target, decodes with the pinned Aurora decoder, checks SDR/HDR colors, verifies
HDR bootstrap black, idle refresh, and keyed-mutex return. This is functional
validation, not a 1080p/4K throughput measurement. Eleven targeted frontend
settings tests also pass. The modified video/RTSP/HTTP/worker integration files
pass syntax compilation with PyroWave disabled.

Broader validation has existing failures: three locale consistency checks report
legacy locale lists versus the English-only General page; repository-wide
frontend lint and typechecking report errors outside this change. These remain
unresolved. No installed service or driver was replaced, and no live remote
client session has been certified.

Remaining acceptance sequence:

1. Use the pinned Aurora revision for paired 1080p60 SDR and HDR sessions at
   700 Mbps, then repeat at the intended resolution and refresh rate.
2. Exercise each existing LuminalVGD transport configuration (D3D11,
   D3D11On12 where applicable, and D3D12), checking display transitions,
   restoration, reconnects and GPU ownership. PyroWave encoding remains Vulkan.
3. Before and after each change, run the guard suite and live H.264, HEVC and AV1
   sessions on ordinary clients. Repeat GPU interop checks across supported
   vendors before widening availability.
4. Measure bitrate, frame loss, latency and color fidelity. Keep the feature
   opt-in until those checks pass. Extending the full 10 Gbps range requires a
   separate transport/client agreement and corresponding validation.
