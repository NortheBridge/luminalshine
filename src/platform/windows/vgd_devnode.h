/**
 * @file src/platform/windows/vgd_devnode.h
 * @brief Service-owned LuminalVGD device node: creation, adoption, and
 *        no-reboot driver switchover.
 *
 * Historically the MSI created a persistent `root\luminal_vgd` devnode and
 * force-bound the bundled driver in place (UpdateDriverForPlugAndPlayDevices)
 * — but an in-place update of a RUNNING IddCx device frequently cannot stop
 * it (WUDFHost hosts the attached adapter), so Windows set bRebootRequired,
 * kept the OLD driver, and every LuminalShine upgrade needed a reboot before
 * the new driver ran (falling back to WGC until then).
 *
 * This module moves device ownership into the service (the SudoVDA model):
 *
 *  - `startup_ensure_and_heal()` runs once at backend selection. It adopts
 *    a present devnode when one exists (dev boxes install a persistent one
 *    via scripts\install-driver.ps1), otherwise creates a software device
 *    (SwDeviceCreate, hardware id `root\luminal_vgd`) that lives for the
 *    service process's lifetime. Fresh MSI installs stage the driver
 *    package only — the devnode appears when the service starts.
 *
 *  - After the handshake it compares the running driver build against the
 *    bundled package (INF DriverVer's 4th field). When the bundle is NEWER
 *    it rebinds without a reboot: sessions destroyed, control handle
 *    closed, UpdateDriverForPlugAndPlayDevices against the now-idle device
 *    (no host handles -> the stop is not vetoed), falling back to a full
 *    devnode remove + recreate (fresh PnP ranking always binds the newest
 *    staged driver). A dev box running a NEWER build than the bundle is
 *    left alone.
 *
 * Windows-only, called from inside VDISPLAY::select_backend() — must never
 * call anything that recurses into backend selection (active_backend(),
 * vgd_recovery::manual_restart(), ...).
 */
#pragma once

#include <cstdint>
#include <optional>

namespace platf::vgd_devnode {

  /// Driver build carried by the bundled driver package (INF DriverVer
  /// `x.y.z.BUILD`), or nullopt when no bundled package is found/parsable.
  std::optional<std::uint32_t> bundled_driver_build();

  /**
   * @brief One-shot startup hook: make sure a `root\luminal_vgd` device
   *        exists (adopt or create), then heal a stale driver binding.
   *
   * Idempotent (std::call_once); safe to invoke from backend selection.
   * Failures are logged and non-fatal — when no device can be produced,
   * backend selection simply sees the driver as not installed, exactly
   * as before this module existed.
   */
  void startup_ensure_and_heal();

}  // namespace platf::vgd_devnode
