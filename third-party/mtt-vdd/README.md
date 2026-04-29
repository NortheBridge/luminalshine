## Bundled MTT Virtual Display Driver

This directory contains a vendored release of MikeTheTech's Virtual Display
Driver (VDD), an Indirect Display Driver (IDD) used by LuminalShine as the
default virtual display backend.

Upstream: https://github.com/VirtualDrivers/Virtual-Display-Driver
License: MIT (see [LICENSE](LICENSE))

### Bundled artifacts

| File | Purpose |
|------|---------|
| `MttVDD.dll` | UMDF host DLL loaded by `WUDFRd.sys` |
| `MttVDD.inf` | Driver package metadata (DriverVer 11.30.4.434) |
| `mttvdd.cat` | Catalog file, signed by SignPath Foundation via GlobalSign |
| `vdd_settings.xml.template` | Default settings consumed by the driver on init/reload |

### Signing

The catalog (`mttvdd.cat`) is signed by `SignPath Foundation`, issued by
`GlobalSign GCC R45 CodeSigning CA 2020`, rooted at `GlobalSign Code Signing
Root R45`. GlobalSign is trusted by default on every supported Windows install,
so no user-side certificate bootstrap is required.

### Runtime control

LuminalShine controls this driver through:

1. The named pipe `\\.\pipe\MTTVirtualDisplayPipe` (commands: `RELOAD_DRIVER`,
   `SETDISPLAYCOUNT N`, `PING`/`PONG`, `SETGPU`, etc.)
2. The settings XML at `%ProgramData%\LuminalShine\vdd_settings.xml` (path
   redirected from MTT's default `C:\VirtualDisplayDriver\` via the registry
   value `HKLM\SOFTWARE\MikeTheTech\VirtualDisplayDriver\VDDPATH`).

See `src/platform/windows/virtual_display_mtt.{h,cpp}` for the integration.
