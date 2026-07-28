# SPDX-License-Identifier: AGPL-3.0-only
# LuminalVGD driver install/uninstall for the LuminalShine MSI.
#
# Install mode (default), run on every install, update, and reinstall:
#   1. Remove SudoVDA whenever detected (decision 2026-07-23: SudoVDA is
#      unmaintained and no LuminalShine version ships or keeps it).
#   2. Install the bundled LuminalVGD driver package: seed the signer into
#      LocalMachine\TrustedPublisher, add the package, ensure the
#      root\luminal_vgd devnode exists, and force-bind the driver.
# -Uninstall: remove the LuminalVGD devnode(s) and driver package(s).
#
# Invoked by WixQuietExec with Return="ignore" — this script is
# best-effort by contract and must never block the MSI transaction, but
# still exits non-zero on install failure so the MSI log shows it.
param(
    [switch]$Uninstall
)
$ErrorActionPreference = 'Continue'

# 64-bit re-exec guard: the driver tooling this script depends on does
# not work from a 32-bit host on 64-bit Windows — pnputil does not
# resolve in WOW64 (no SysWOW64\pnputil.exe) and newdev's
# UpdateDriverForPlugAndPlayDevices returns ERROR_IN_WOW64. If a caller
# launched us in WOW64 (e.g. an installer resolving the 32-bit
# interpreter), relaunch through Sysnative and forward the exit code.
if (-not [Environment]::Is64BitProcess -and [Environment]::Is64BitOperatingSystem) {
    $native = Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe'
    if (Test-Path $native) {
        Write-Host "[LuminalVGD] 32-bit host detected; relaunching via 64-bit PowerShell."
        $fwd = @()
        if ($Uninstall) { $fwd += '-Uninstall' }
        & $native -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass -File $MyInvocation.MyCommand.Path @fwd
        exit $LASTEXITCODE
    }
    Write-Warning "[LuminalVGD] 32-bit host and Sysnative unavailable; driver operations may fail."
}
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$packageDir = Join-Path $scriptDir 'driver-package'

# --- LuminalVGD ETW persistence ---------------------------------------------
# The driver traces through TraceLogging provider "NortheBridge.LuminalVGD",
# but TraceLogging events are LOST unless a session is recording when they
# fire — every failed-TDR-recovery postmortem so far (2026-07-27, 2026-07-28)
# ended at "no trace was capturing, duck-out behavior unverifiable". A small
# circular AutoLogger records from boot; an identical live (-ets) session
# covers the current boot. ~8 MB circular file, negligible overhead. The
# crash-bundle exporter flushes and bundles the file as luminalvgd-etw.etl.
$etwSessionName = 'LuminalVGD'
$etwProviderGuid = '{c501990d-df12-5581-60a8-f55d593d7f7c}'
# Stable session identity for the AutoLogger registration itself.
$etwSessionGuid = '{b7e5e8ab-6a7c-4e64-9f9e-4d1a2c50c5a1}'
# Deliberately OUTSIDE ProgramData\LuminalShine\config: that directory
# carries a protected DACL + System-integrity no-write-up label, which
# breaks directory creation from a manual elevated (non-SYSTEM) run of
# this script. The kernel logger and the bundle exporter both run as
# SYSTEM and can use either location; this one works for everyone.
$etwLogDir = Join-Path $env:ProgramData 'LuminalShine\etw'
$etwLogFile = Join-Path $etwLogDir 'luminalvgd.etl'

function Install-VgdAutologger {
    New-Item -ItemType Directory -Force -Path $etwLogDir | Out-Null
    $key = "HKLM:\SYSTEM\CurrentControlSet\Control\WMI\Autologger\$etwSessionName"
    New-Item -Path $key -Force | Out-Null
    New-ItemProperty -Path $key -Name 'GUID' -Value $etwSessionGuid -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $key -Name 'Start' -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $key -Name 'FileName' -Value $etwLogFile -PropertyType String -Force | Out-Null
    # 0x2 = EVENT_TRACE_FILE_MODE_CIRCULAR
    New-ItemProperty -Path $key -Name 'LogFileMode' -Value 2 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $key -Name 'MaxFileSize' -Value 8 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $key -Name 'BufferSize' -Value 16 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $key -Name 'FlushTimer' -Value 5 -PropertyType DWord -Force | Out-Null
    # CRITICAL: rotate per boot. The wedge this trace exists to diagnose
    # is cleared BY REBOOTING, and without FileMax the AutoLogger
    # overwrites the file at that very reboot — destroying the incident
    # boot's Tdr* breadcrumbs before the user can export a bundle. With
    # FileMax, ETW keeps per-boot instances (sequence-numbered) and the
    # bundle exporter globs the whole directory.
    New-ItemProperty -Path $key -Name 'FileMax' -Value 3 -PropertyType DWord -Force | Out-Null
    $prov = Join-Path $key $etwProviderGuid
    New-Item -Path $prov -Force | Out-Null
    New-ItemProperty -Path $prov -Name 'Enabled' -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $prov -Name 'EnableLevel' -Value 5 -PropertyType DWord -Force | Out-Null
    # All keyword bits: -1 as Int64 carries the 0xFFFFFFFFFFFFFFFF pattern.
    New-ItemProperty -Path $prov -Name 'MatchAnyKeyword' -Value ([Int64]-1) -PropertyType QWord -Force | Out-Null

    # The AutoLogger only starts at the NEXT boot; cover the current boot
    # with an equivalent live session unless one is already running. A
    # distinct file name keeps it clear of the AutoLogger's rotation set.
    # -ft 5 matches the AutoLogger's FlushTimer: without it, buffers only
    # reach disk when full (16 KB) or at clean close — a hard power cut
    # mid-wedge would lose every buffered event of this boot.
    logman query $etwSessionName -ets *> $null
    if ($LASTEXITCODE -ne 0) {
        $liveFile = Join-Path $etwLogDir 'luminalvgd-live.etl'
        logman create trace $etwSessionName -ets -o $liveFile -f bincirc -max 8 -bs 16 -ft 5 -p $etwProviderGuid 0xffffffffffffffff 0x5 *> $null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "[LuminalVGD] ETW trace session started ($liveFile)."
        } else {
            Write-Warning "[LuminalVGD] Could not start the live ETW session (logman exit $LASTEXITCODE); boot-time AutoLogger is registered."
        }
    }
    Write-Host "[LuminalVGD] ETW AutoLogger registered (circular 8 MB x3 boots, provider $etwProviderGuid)."
}

function Remove-VgdAutologger {
    logman stop $etwSessionName -ets *> $null
    Remove-Item -Path "HKLM:\SYSTEM\CurrentControlSet\Control\WMI\Autologger\$etwSessionName" -Recurse -Force -Confirm:$false -ErrorAction SilentlyContinue
    Remove-Item -Path $etwLogDir -Recurse -Force -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "[LuminalVGD] ETW AutoLogger removed."
}

function Get-DevicesByHardwareId([string]$HardwareId) {
    # Every caller targets Display-class root devnodes (LuminalVGD,
    # SudoVDA), and -Class Display is what keeps this fast: each
    # Get-PnpDeviceProperty round-trip costs ~0.8 s, so an unfiltered scan
    # of every devnode ran minutes per call inside the MSI. Phantom
    # (not-present) devnodes keep their class and still match.
    Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | Where-Object {
        $ids = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_HardwareIds' -ErrorAction SilentlyContinue).Data
        $ids -and (@($ids) -contains $HardwareId)
    }
}

function Get-PublishedDriverPackages([string]$OriginalInfPattern, [string]$ProviderPattern) {
    $out = pnputil /enum-drivers | Out-String
    $packages = @()
    $current = $null
    foreach ($line in ($out -split "`r?`n")) {
        if ($line -match 'Published Name:\s+(oem\d+\.inf)') { $current = $Matches[1] }
        elseif ($current -and ($line -match "Original Name:\s+$OriginalInfPattern" -or $line -match "Provider Name:\s+.*$ProviderPattern")) {
            $packages += $current
            $current = $null
        }
    }
    $packages | Select-Object -Unique
}

function Remove-SudoVda {
    # Unconditional eviction: devices, driver packages, SudoMaker
    # publisher certs, and the SudoMaker registry key. Every step is
    # best-effort — a partially removed SudoVDA must not fail the MSI.
    $found = $false

    foreach ($dev in @(Get-DevicesByHardwareId 'root\sudomaker\sudovda')) {
        $found = $true
        Write-Host "[LuminalVGD] Removing SudoVDA device $($dev.InstanceId)"
        pnputil /remove-device $dev.InstanceId | Out-Null
    }

    foreach ($oem in @(Get-PublishedDriverPackages 'SudoVDA\.inf' 'SudoMaker')) {
        $found = $true
        Write-Host "[LuminalVGD] Deleting SudoVDA driver package $oem"
        pnputil /delete-driver $oem /uninstall /force | Out-Null
    }

    foreach ($storeName in @('TrustedPublisher', 'Root')) {
        $certs = Get-ChildItem "Cert:\LocalMachine\$storeName" -ErrorAction SilentlyContinue |
            Where-Object { $_.Subject -like '*SudoMaker*' }
        foreach ($cert in @($certs)) {
            $found = $true
            Write-Host "[LuminalVGD] Removing SudoMaker certificate from $storeName [$($cert.Thumbprint)]"
            Remove-Item -Path "Cert:\LocalMachine\$storeName\$($cert.Thumbprint)" -Confirm:$false -ErrorAction SilentlyContinue
        }
    }

    if (Test-Path 'HKLM:\SOFTWARE\SudoMaker\SudoVDA') {
        $found = $true
        Write-Host "[LuminalVGD] Removing HKLM:\SOFTWARE\SudoMaker\SudoVDA"
        Remove-Item -Path 'HKLM:\SOFTWARE\SudoMaker\SudoVDA' -Recurse -Force -Confirm:$false -ErrorAction SilentlyContinue
        # Remove the parent when SudoVDA was its only child.
        $parent = Get-Item 'HKLM:\SOFTWARE\SudoMaker' -ErrorAction SilentlyContinue
        if ($parent -and $parent.SubKeyCount -eq 0 -and $parent.ValueCount -eq 0) {
            Remove-Item -Path 'HKLM:\SOFTWARE\SudoMaker' -Force -Confirm:$false -ErrorAction SilentlyContinue
        }
    }

    if ($found) { Write-Host "[LuminalVGD] SudoVDA removal pass complete." }
    else { Write-Host "[LuminalVGD] No SudoVDA remnants detected." }
}

function Remove-LuminalVgd {
    foreach ($dev in @(Get-DevicesByHardwareId 'root\luminal_vgd')) {
        Write-Host "[LuminalVGD] Removing device $($dev.InstanceId)"
        pnputil /remove-device $dev.InstanceId | Out-Null
    }
    foreach ($oem in @(Get-PublishedDriverPackages 'luminalvgd\.inf' 'NortheBridge')) {
        Write-Host "[LuminalVGD] Deleting driver package $oem"
        pnputil /delete-driver $oem /uninstall /force | Out-Null
    }
    Write-Host "[LuminalVGD] Uninstall pass complete."
}

function Install-LuminalVgd {
    $inf = Join-Path $packageDir 'luminalvgd.inf'
    $cat = Join-Path $packageDir 'luminalvgd.cat'
    $dll = Join-Path $packageDir 'luminal_vgd_driver.dll'
    foreach ($f in @($inf, $cat, $dll)) {
        if (-not (Test-Path $f)) { throw "[LuminalVGD] missing driver artifact: $f" }
    }

    $build = [Environment]::OSVersion.Version.Build
    if ($build -lt 22000) { throw "[LuminalVGD] Windows 11 (build 22000+) required; this is build $build" }
    if ($build -lt 26100) { Write-Warning "[LuminalVGD] Windows 11 24H2 (26100+) is required for full HDR; SDR streaming works on build $build." }

    # Seed the package signer into TrustedPublisher ONLY (the OV cert
    # chains to a public root; the Root store is never touched) so the
    # driver installs without a publisher-trust prompt.
    $sig = Get-AuthenticodeSignature $cat
    if ($sig.Status -ne 'Valid') { throw "[LuminalVGD] catalog signature status is '$($sig.Status)'" }
    $inTrusted = Get-ChildItem Cert:\LocalMachine\TrustedPublisher -ErrorAction SilentlyContinue |
        Where-Object Thumbprint -eq $sig.SignerCertificate.Thumbprint
    if (-not $inTrusted) {
        Write-Host "[LuminalVGD] Seeding signer into LocalMachine\TrustedPublisher: $($sig.SignerCertificate.Subject)"
        $store = [System.Security.Cryptography.X509Certificates.X509Store]::new('TrustedPublisher', 'LocalMachine')
        $store.Open('ReadWrite')
        try { $store.Add($sig.SignerCertificate) } finally { $store.Close() }
    }

    Write-Host "[LuminalVGD] Adding driver package..."
    # Stage only — no /install: the UpdateDriverForPlugAndPlayDevices
    # force-bind below performs the one device install/start.
    pnputil /add-driver $inf | Out-Null
    if ($LASTEXITCODE -notin 0, 3010, 259) { throw "[LuminalVGD] pnputil /add-driver failed ($LASTEXITCODE)" }
    if ($LASTEXITCODE -eq 3010) { Write-Warning "[LuminalVGD] Windows reports a reboot is required to finish the driver update." }

    $existing = @(Get-DevicesByHardwareId 'root\luminal_vgd')
    if ($existing.Count -eq 0) {
        Write-Host "[LuminalVGD] Creating root\luminal_vgd devnode..."
        Add-Type -Namespace LuminalVgd -Name DevNode -MemberDefinition @'
[DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid ClassGuid, IntPtr hwndParent);
[DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool SetupDiCreateDeviceInfoW(IntPtr DeviceInfoSet, string DeviceName, ref Guid ClassGuid, string DeviceDescription, IntPtr hwndParent, int CreationFlags, ref SP_DEVINFO_DATA DeviceInfoData);
[DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, int Property, byte[] PropertyBuffer, int PropertyBufferSize);
[DllImport("setupapi.dll", SetLastError = true)]
public static extern bool SetupDiCallClassInstaller(int InstallFunction, IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData);
[DllImport("setupapi.dll", SetLastError = true)]
public static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);
[StructLayout(LayoutKind.Sequential)]
public struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }
'@
        $displayClass = [Guid]'4d36e968-e325-11ce-bfc1-08002be10318'
        $data = New-Object LuminalVgd.DevNode+SP_DEVINFO_DATA
        $data.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($data)
        $set = [LuminalVgd.DevNode]::SetupDiCreateDeviceInfoList([ref]$displayClass, [IntPtr]::Zero)
        if ($set -eq [IntPtr]::Zero -or $set -eq [IntPtr]::new(-1)) { throw "[LuminalVGD] SetupDiCreateDeviceInfoList failed" }
        try {
            if (-not [LuminalVgd.DevNode]::SetupDiCreateDeviceInfoW($set, 'Display', [ref]$displayClass, 'Luminal Video Graphics Display', [IntPtr]::Zero, 0x1, [ref]$data)) {
                throw "[LuminalVGD] SetupDiCreateDeviceInfo failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
            }
            $hwid = [Text.Encoding]::Unicode.GetBytes("root\luminal_vgd`0`0")
            if (-not [LuminalVgd.DevNode]::SetupDiSetDeviceRegistryPropertyW($set, [ref]$data, 0x1, $hwid, $hwid.Length)) {
                throw "[LuminalVGD] SetupDiSetDeviceRegistryProperty failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
            }
            if (-not [LuminalVgd.DevNode]::SetupDiCallClassInstaller(0x19, $set, [ref]$data)) {
                throw "[LuminalVGD] SetupDiCallClassInstaller(DIF_REGISTERDEVICE) failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
            }
        } finally {
            [void][LuminalVgd.DevNode]::SetupDiDestroyDeviceInfoList($set)
        }
    }

    Write-Host "[LuminalVGD] Binding driver to root\luminal_vgd..."
    Add-Type -Namespace LuminalVgd -Name NewDev -MemberDefinition @'
[DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwndParent, string HardwareId, string FullInfPath, uint InstallFlags, out bool bRebootRequired);
'@
    $reboot = $false
    $infFull = (Resolve-Path $inf).Path
    if (-not [LuminalVgd.NewDev]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, 'root\luminal_vgd', $infFull, 0x1, [ref]$reboot)) {
        throw "[LuminalVGD] UpdateDriverForPlugAndPlayDevices failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    if ($reboot) { Write-Warning "[LuminalVGD] Windows reports a reboot is required." }
    Write-Host "[LuminalVGD] Driver install complete."
}

try {
    if ($Uninstall) {
        try {
            Remove-VgdAutologger
        } catch {
            Write-Warning "[LuminalVGD] ETW AutoLogger removal failed (continuing): $($_.Exception.Message)"
        }
        Remove-LuminalVgd
    } else {
        # The sweep is best-effort by contract — its failure must never
        # abort the driver install (a terminating error here previously
        # killed the whole script before Install-LuminalVgd ran).
        try {
            Remove-SudoVda
        } catch {
            Write-Warning "[LuminalVGD] SudoVDA sweep failed (continuing): $($_.Exception.Message)"
        }
        Install-LuminalVgd
        # Best-effort by contract, like the sweep: diagnostics must never
        # fail the driver install.
        try {
            Install-VgdAutologger
        } catch {
            Write-Warning "[LuminalVGD] ETW AutoLogger setup failed (continuing): $($_.Exception.Message)"
        }
    }
    exit 0
} catch {
    Write-Error "[LuminalVGD] $($_.Exception.Message)"
    exit 1
}
