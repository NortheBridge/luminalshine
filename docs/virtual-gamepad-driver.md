# Virtual gamepad driver

LuminalShine uses Nonary's `libvirtualgamepad` UMDF/VHF driver, as implemented in
[Vibepollo](https://github.com/Nonary/Vibepollo). The application client is pinned
to commit `7b000a2b060b4daeb0b5c6aa7b77ae1d81857e7a`, protocol 2. The corresponding
x64 driver release is
[v0.1.0-beta.9](https://github.com/Nonary/libvirtualgamepad/releases/tag/v0.1.0-beta.9),
DriverVer `10/16/2026,0.1.0.53`. The driver INF requires x64 Windows 10
build **19041 (version 2004)** or newer, including Windows 11.

## Installing on a host

If a compatible VHF driver is already installed by Vibepollo, LuminalShine can
use it directly. Replacing the application backend does not remove ViGEmBus:
other applications may still depend on it. Windows boot/signing policies and
trusted roots remain unchanged. When needed for silent installation, the
installer enrolls only its verified production signer in LocalMachine
TrustedPublisher.

When a signed driver package is bundled, the MSI attempts installation unless
`INSTALL_VHF_GAMEPAD=0` is supplied. An installation failure is logged and does
not roll back the application; check the WebUI driver status before streaming.
For a portable package, run elevated **64-bit Windows PowerShell**:

```powershell
powershell.exe -NoProfile -File '.\drivers\vhf-gamepad\install.ps1'
```

Restart Windows if requested. Equal/newer active drivers and newer staged
packages are preserved. If a retained driver is broken, use its original
installer to repair it. LuminalShine intentionally leaves this shared driver
installed on application uninstall.

## Release packaging and signing

The producer release is **unsigned**. Its catalog and device setup executable
must be signed downstream using a reviewed code-signing identity trusted by
Windows. The driver DLL must **not** be Authenticode-signed after catalog
creation: doing so changes its bytes and breaks catalog membership. Signing
only the LuminalShine executable or MSI does not sign the enclosed driver.

Signed Windows release CI automatically downloads the pinned archive, verifies
its SHA-256 and exact layout, then uses the existing SSL.com eSigner credential
to sign only the CAT and setup EXE. [SSL.com documents CAT support](https://www.ssl.com/guide/esigner-supported-file-types/amp/).
Both signatures must be Windows-valid and match the certificate used to sign
that build's `luminalshine.exe`. CMake embeds that exact certificate thumbprint
in the installer and validates the signed package before staging the MSI/ZIP.
A hash check rejects any signed executable or driver payload unexpectedly changed
by the subsequent packaging target. Driver DLL bytes remain unchanged.

Unsigned nightly builds do not bundle the unsigned producer driver and never
create or trust a test certificate. Users of nightly/development builds need a
compatible driver installed separately, or can supply a reviewed signed package
using the local packaging options below. The release pipeline requires the
existing signing secrets; a missing credential, unexpected signer, or invalid
package fails the build rather than publishing an unusable driver bundle.

This pipeline has been reviewed and its portable validation tests run locally;
actual SSL.com signing, Windows driver loading and MSI upgrade behavior still
require the Windows CI and host validation described below.

On a clean host, CA-valid signatures alone may not permit unattended PnP
installation: [Windows also requires publisher approval](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/trusted-publishers-certificate-store).
After validating the complete package, administrator rights, and installed
versions, the installer adds only the exact pinned, Windows-valid catalog
signer leaf to LocalMachine TrustedPublisher if it is absent. This approval
persists and allows silent installation of other packages signed by the same
certificate; it does not establish a new trusted root. `INSTALL_VHF_GAMEPAD=0`
skips the MSI driver action and publisher enrollment. Validation-only and
retained-driver paths make no certificate-store changes.

If staging or device setup fails, only the certificate newly added by that
attempt is removed. Preexisting entries and unrelated publishers remain.
Rollback failures report the exact certificate/store without hiding the original
installation failure. Successful installation and application uninstall retain
the publisher entry because the driver may be shared. Root trust, Secure Boot,
and test-signing settings are never changed.

Prepare a signed package in this exact layout (retain the producer manifest):

```text
driver/VibeshineVhfGamepad.inf
driver/VibeshineVhfGamepad.dll
driver/VibeshineVhfGamepad.cat
tools/VibeshineVhfGamepadDeviceSetup.exe
manifest.json
```

Acquire the archive from the pinned release and verify its SHA-256 before
signing:
`59e89ab90c6e360e3e23bb4717b43339c4ec0050aedc52fe05e4f3ef306d62ee`.
Sign **only CAT and setup EXE**, with the same approved identity. Verify catalog
membership of INF/DLL with Windows SDK `signtool verify /pa /c <catalog> <file>`
and native Windows installation before publishing. Supply its independently
reviewed SHA-1 certificate thumbprint, rather than deriving trust from whatever
certificate happens to be in the package:

```powershell
cmake -S . -B build -G Ninja `
  -DSUNSHINE_VHF_GAMEPAD_SIGNED_PACKAGE_DIR=C:/packages/vhf-signed `
  -DSUNSHINE_VHF_GAMEPAD_SIGNER_THUMBPRINT=<40-hex-certificate-thumbprint>
```

Configure validates the package's revision, protocol, platform, INF version,
INF/DLL hashes, and Windows-valid CAT/setup signatures against that pin. The
configured installer repeats validation at runtime and lets Windows PnP verify
catalog membership before invoking the signed device setup tool. It refuses
unsigned, untrusted, changed, or differently signed payloads. It never imports
Root certificates or enables test signing. Verified production publisher
enrollment follows the narrowly scoped policy above; Windows can still reject
a driver under device-installation or code-integrity policy.

The installer returns 0 for success/retained driver (with a warning if reboot is
needed), and nonzero for validation or installation failure. The presence of
`drivers/vhf-gamepad/install.ps1` identifies a bundled package; `manifest.json`
records the driver version. Neither indicates that Windows has loaded it:
use the application's driver protocol probe for availability.

## Validation

`pwsh -NoProfile -File packaging/windows/test_vhf_package_validation.ps1` runs
portable package-validation fixtures with explicitly mocked Authenticode
results. It checks rejection of missing/tampered payloads, wrong signer,
invalid signature, and wrong protocol. These tests do not install a driver,
modify trust, or establish real Windows signature/driver compatibility.
Windows installation, reboot, controller feedback, Bluetooth reconnect and MSI
upgrade testing remain necessary before publishing a release.

The MSI compatibility oracle permits only the explicitly validated VHF payload,
MIT license, two install actions and opt-out property additions to the pre-VHF
baseline. All existing services, GUIDs and action order remain checked. Run
`python scripts/test_vhf_msi_compat.py` for its positive and rejection fixtures.

`pwsh -NoProfile -File packaging/windows/test_vhf_publisher_enrollment.ps1`
exercises the actual enrollment transaction with an in-memory store and mocked
staging/setup calls: rejection, existing trust, failure rollback, unrelated
certificate preservation and reboot propagation. It performs no real
certificate-store or driver operations.
