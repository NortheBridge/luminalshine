param(
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $PSCommandPath
$hardwarePrefix = 'ROOT\MttVDD'
$hardwareId = $hardwarePrefix.ToLowerInvariant()
$classGuid = '{4D36E968-E325-11CE-BFC1-08002BE10318}'
$infPath = Join-Path $scriptDir 'MttVDD.inf'
$catPath = Join-Path $scriptDir 'mttvdd.cat'
$dllPath = Join-Path $scriptDir 'MttVDD.dll'
$nefConc = Join-Path $scriptDir '..\sudovda\nefconc.exe'
$settingsTemplate = Join-Path $scriptDir 'vdd_settings.xml.template'

# Settings live in %ProgramData%\LuminalShine\vdd_settings.xml. The MTT driver
# reads HKLM\SOFTWARE\MikeTheTech\VirtualDisplayDriver\VDDPATH for the
# directory, so we point it at our managed location and seed a default file.
$settingsDir = Join-Path $env:ProgramData 'LuminalShine'
$settingsPath = Join-Path $settingsDir 'vdd_settings.xml'

$script:rebootRequired = $false

Import-Module PnpDevice -ErrorAction SilentlyContinue | Out-Null

function Resolve-SystemToolPath {
    param([Parameter(Mandatory = $true)][string]$ToolName)

    $systemRoot = if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) { 'C:\Windows' } else { $env:SystemRoot }
    $candidates = @(
        (Join-Path -Path $systemRoot -ChildPath ("Sysnative\" + $ToolName))
        (Join-Path -Path $systemRoot -ChildPath ("System32\" + $ToolName))
        (Join-Path -Path $systemRoot -ChildPath ("SysWOW64\" + $ToolName))
    )
    foreach ($c in $candidates) {
        if (Test-Path -Path $c -PathType Leaf) { return $c }
    }
    return $candidates[1]
}

$pnputil = Resolve-SystemToolPath -ToolName 'pnputil.exe'

function Invoke-Process {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [string]$WorkingDirectory = $scriptDir
    )

    $stdoutPath = [System.IO.Path]::GetTempFileName()
    $stderrPath = [System.IO.Path]::GetTempFileName()

    try {
        $process = Start-Process -FilePath $FilePath `
                                 -ArgumentList $ArgumentList `
                                 -WorkingDirectory $WorkingDirectory `
                                 -WindowStyle Hidden `
                                 -Wait -PassThru `
                                 -RedirectStandardOutput $stdoutPath `
                                 -RedirectStandardError $stderrPath
        $stdout = if (Test-Path $stdoutPath) { Get-Content -Path $stdoutPath -Raw -ErrorAction SilentlyContinue } else { '' }
        $stderr = if (Test-Path $stderrPath) { Get-Content -Path $stderrPath -Raw -ErrorAction SilentlyContinue } else { '' }
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            StdOut   = $stdout
            StdErr   = $stderr
        }
    } finally {
        Remove-Item -LiteralPath $stdoutPath, $stderrPath -ErrorAction SilentlyContinue
    }
}

function Write-ProcessOutput {
    param([Parameter(Mandatory = $true)]$Result)
    if ($Result.StdOut) { Write-Host $Result.StdOut.TrimEnd() }
    if ($Result.StdErr) { Write-Host $Result.StdErr.TrimEnd() }
}

function Invoke-DriverStep {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $true)][string]$Description
    )
    $r = Invoke-Process -FilePath $FilePath -ArgumentList $ArgumentList
    Write-ProcessOutput -Result $r
    switch ($r.ExitCode) {
        0    { return }
        3010 { $script:rebootRequired = $true; return }
        default { throw "[MTT VDD] $Description failed with exit code $($r.ExitCode)." }
    }
}

function Assert-RequiredArtifact {
    param([string]$Path, [string]$DisplayName)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "[MTT VDD] Required artifact missing: $DisplayName ($Path)"
    }
    if ((Get-Item -LiteralPath $Path).Length -le 0) {
        throw "[MTT VDD] Required artifact is empty: $DisplayName ($Path)"
    }
}

function Set-VddPathRegistry {
    param([string]$Directory)
    $key = 'HKLM:\SOFTWARE\MikeTheTech\VirtualDisplayDriver'
    if (-not (Test-Path $key)) {
        New-Item -Path $key -Force | Out-Null
    }
    $withSlash = if ($Directory.EndsWith('\')) { $Directory } else { "$Directory\" }
    Set-ItemProperty -Path $key -Name 'VDDPATH' -Value $withSlash -Type String -Force
    Write-Host "[MTT VDD] VDDPATH = $withSlash"
}

function Initialize-SettingsDirectory {
    if (-not (Test-Path $settingsDir)) {
        New-Item -Path $settingsDir -ItemType Directory -Force | Out-Null
    }
    if (-not (Test-Path $settingsPath) -and (Test-Path $settingsTemplate)) {
        Copy-Item -LiteralPath $settingsTemplate -Destination $settingsPath -Force
        Write-Host "[MTT VDD] Seeded default settings at $settingsPath"
    }
}

function Test-DriverPresent {
    try {
        $devices = Get-PnpDevice -PresentOnly -ErrorAction Stop |
            Where-Object {
                $_.HardwareID -like '*MttVDD*' -or
                $_.FriendlyName -like '*Virtual Display Driver*' -or
                $_.FriendlyName -like '*MikeTheTech*'
            }
        if ($devices) { return $true }
    } catch { $null = $_ }

    try {
        $r = Invoke-Process -FilePath $pnputil -ArgumentList @('/enum-devices', '/class', 'Display', '/connected')
        if ($r.ExitCode -eq 0 -and $r.StdOut -and $r.StdOut -match 'MttVDD') {
            return $true
        }
    } catch { $null = $_ }

    return $false
}

function Get-InstalledDriverPackages {
    $r = Invoke-Process -FilePath $pnputil -ArgumentList @('/enum-drivers')
    if ($r.ExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($r.StdOut)) { return @() }

    $entries = @()
    $current = @{}
    foreach ($line in ($r.StdOut -split "`r?`n")) {
        if ($line -match '^\s*Published Name\s*:\s*(\S+)') { $current['PublishedName'] = $matches[1] }
        elseif ($line -match '^\s*Original Name\s*:\s*(.+)$') { $current['OriginalName'] = $matches[1].Trim() }
        elseif ($line -match '^\s*Provider Name\s*:\s*(.+)$') { $current['ProviderName'] = $matches[1].Trim() }
        elseif ($line -match '^\s*$') {
            if ($current.ContainsKey('PublishedName')) { $entries += [pscustomobject]$current }
            $current = @{}
        }
    }
    if ($current.ContainsKey('PublishedName')) { $entries += [pscustomobject]$current }

    return $entries | Where-Object {
        $_.OriginalName -match '^MttVDD\.inf$' -or $_.ProviderName -match 'MikeTheTech'
    }
}

function Remove-DriverPackage {
    param([string]$PublishedName)
    Write-Host "[MTT VDD] Removing driver package $PublishedName."
    $r = Invoke-Process -FilePath $pnputil -ArgumentList @('/delete-driver', $PublishedName, '/uninstall', '/force')
    Write-ProcessOutput -Result $r
    switch ($r.ExitCode) {
        0    { return }
        3010 { $script:rebootRequired = $true; return }
        default { throw "[MTT VDD] Failed to remove driver package $PublishedName (exit code $($r.ExitCode))." }
    }
}

function Invoke-VddUninstall {
    if (-not (Test-Path -Path $pnputil -PathType Leaf)) {
        throw '[MTT VDD] Unable to locate pnputil.exe.'
    }

    if (Test-Path -Path $nefConc -PathType Leaf) {
        Write-Host '[MTT VDD] Removing existing MttVDD device node.'
        $r = Invoke-Process -FilePath $nefConc -ArgumentList @('--remove-device-node', '--hardware-id', $hardwareId, '--class-guid', $classGuid)
        Write-ProcessOutput -Result $r
        switch ($r.ExitCode) {
            0    { }
            3010 { $script:rebootRequired = $true }
            default { Write-Warning "[MTT VDD] Remove-device-node returned $($r.ExitCode). Continuing." }
        }
    } else {
        Write-Warning '[MTT VDD] nefconc.exe not found alongside SudoVDA package; skipping device-node removal.'
    }

    $packages = @(Get-InstalledDriverPackages)
    if ($packages.Count -eq 0) {
        Write-Host '[MTT VDD] No matching driver package found; trying direct INF removal.'
        $r = Invoke-Process -FilePath $pnputil -ArgumentList @('/delete-driver', 'MttVDD.inf', '/uninstall', '/force')
        Write-ProcessOutput -Result $r
        switch ($r.ExitCode) {
            0    { return }
            3010 { $script:rebootRequired = $true; return }
            default {
                if (Test-DriverPresent) {
                    throw "[MTT VDD] Direct INF removal failed (exit code $($r.ExitCode)) and the driver is still present."
                }
                Write-Host '[MTT VDD] Driver package was already absent.'
                return
            }
        }
    }
    foreach ($pkg in $packages) { Remove-DriverPackage -PublishedName $pkg.PublishedName }
}

if ($Uninstall) {
    Invoke-VddUninstall
    Write-Host '[MTT VDD] Driver uninstall complete.'
    if ($script:rebootRequired) {
        Write-Host '[MTT VDD] A reboot is required to finalize driver removal.'
    }
    $global:LastExitCode = 0
    exit 0
}

Assert-RequiredArtifact -Path $infPath -DisplayName 'MttVDD.inf'
Assert-RequiredArtifact -Path $dllPath -DisplayName 'MttVDD.dll'
Assert-RequiredArtifact -Path $catPath -DisplayName 'mttvdd.cat'

# nefconc is shipped under drivers\sudovda; if SudoVDA was deselected we still
# need it to add the root device. Fall back to pnputil-only flow if absent
# (the user can still load the driver via Device Manager > Add legacy hardware).
$haveNefcon = Test-Path -Path $nefConc -PathType Leaf
if (-not $haveNefcon) {
    Write-Warning '[MTT VDD] nefconc.exe not present; root device creation will be skipped. Add the device manually via Device Manager if needed.'
}

Set-VddPathRegistry -Directory $settingsDir
Initialize-SettingsDirectory

if (Test-DriverPresent) {
    Write-Host '[MTT VDD] Driver already present; staging the bundled INF in case it is newer.'
    $r = Invoke-Process -FilePath $pnputil -ArgumentList @('/add-driver', $infPath, '/install')
    Write-ProcessOutput -Result $r
    if ($r.ExitCode -ne 0 -and $r.ExitCode -ne 3010) {
        Write-Warning "[MTT VDD] /add-driver returned $($r.ExitCode); continuing."
    }
    if ($r.ExitCode -eq 3010) { $script:rebootRequired = $true }
    if ($script:rebootRequired) {
        Write-Host '[MTT VDD] A reboot is required to finalize driver installation.'
    }
    $global:LastExitCode = 0
    exit 0
}

if ($haveNefcon) {
    Write-Host '[MTT VDD] Removing any stale MttVDD device node.'
    $r = Invoke-Process -FilePath $nefConc -ArgumentList @('--remove-device-node', '--hardware-id', $hardwareId, '--class-guid', $classGuid)
    if ($r.ExitCode -eq 3010) { $script:rebootRequired = $true }

    Write-Host '[MTT VDD] Creating MttVDD root device node.'
    Invoke-DriverStep -FilePath $nefConc -ArgumentList @('--create-device-node', '--class-name', 'Display', '--class-guid', $classGuid, '--hardware-id', $hardwareId) -Description 'Create MttVDD device node'

    Write-Host '[MTT VDD] Staging and installing MttVDD driver via nefconc.'
    Invoke-DriverStep -FilePath $nefConc -ArgumentList @('--install-driver', '--inf-path', 'MttVDD.inf') -Description 'Install MttVDD driver'
} else {
    Write-Host '[MTT VDD] Staging MttVDD driver via pnputil.'
    Invoke-DriverStep -FilePath $pnputil -ArgumentList @('/add-driver', $infPath, '/install') -Description 'Stage MttVDD driver'
}

Write-Host '[MTT VDD] Driver install complete.'
if ($script:rebootRequired) {
    Write-Host '[MTT VDD] A reboot is required to finalize driver installation.'
}
$global:LastExitCode = 0
exit 0
