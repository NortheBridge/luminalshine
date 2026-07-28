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
    # Stage the package. The devnode is SERVICE-OWNED as of 26.08.0-beta.7:
    # LuminalShineService adopts a present devnode or creates a software
    # device (SwDeviceCreate) at startup and rebinds the newest staged
    # driver itself — the no-reboot switchover. This script no longer
    # creates devnodes; it only stages, and migrates stuck legacy nodes.
    pnputil /add-driver $inf | Out-Null
    if ($LASTEXITCODE -notin 0, 3010, 259) { throw "[LuminalVGD] pnputil /add-driver failed ($LASTEXITCODE)" }
    if ($LASTEXITCODE -eq 3010) { Write-Host "[LuminalVGD] Staged with a pending file operation; the service-side rebind resolves it without a reboot." }

    $existing = @(Get-DevicesByHardwareId 'root\luminal_vgd')
    if ($existing.Count -eq 0) {
        Write-Host "[LuminalVGD] Driver staged. The LuminalShine service creates the device at startup."
        Write-Host "[LuminalVGD] Driver install complete."
        return
    }

    # Legacy persistent devnode (created by pre-beta.7 installers or the
    # LuminalVGD dev script): attempt the in-place force-bind exactly as
    # before. The MSI has the service stopped here, so the device is idle
    # and the update usually succeeds without a reboot flag.
    Write-Host "[LuminalVGD] Binding driver to existing root\luminal_vgd devnode..."
    Add-Type -Namespace LuminalVgd -Name NewDev -MemberDefinition @'
[DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwndParent, string HardwareId, string FullInfPath, uint InstallFlags, out bool bRebootRequired);
'@
    $reboot = $false
    $infFull = (Resolve-Path $inf).Path
    $bound = [LuminalVgd.NewDev]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, 'root\luminal_vgd', $infFull, 0x1, [ref]$reboot)
    if (-not $bound -or $reboot) {
        # The in-place update could not fully take (device busy / files in
        # use). Instead of leaving the OLD driver running until a reboot,
        # remove the devnode: the service recreates it as a software
        # device at next start, and a brand-new device instance re-ranks
        # drivers from scratch — the newest staged package wins, no
        # reboot. This is how SudoVDA-era installers stayed reboot-free.
        if (-not $bound) {
            Write-Warning "[LuminalVGD] In-place bind failed ($([Runtime.InteropServices.Marshal]::GetLastWin32Error())); migrating the devnode to service ownership."
        } else {
            Write-Host "[LuminalVGD] In-place bind wants a reboot; migrating the devnode to service ownership instead."
        }
        foreach ($dev in $existing) {
            Write-Host "[LuminalVGD] Removing devnode $($dev.InstanceId) (service recreates it at startup)."
            pnputil /remove-device $dev.InstanceId | Out-Null
        }
    }
    Write-Host "[LuminalVGD] Driver install complete."
}

try {
    if ($Uninstall) {
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
    }
    exit 0
} catch {
    Write-Error "[LuminalVGD] $($_.Exception.Message)"
    exit 1
}
