param()

# Best-effort installer migrations. This script intentionally never aborts
# the MSI transaction — see RunInstallerMigrations (Return="ignore") in
# custom_actions.wxs. The C++ runtime carries the same migration logic and
# fires on the first launch after install, so anything we skip here is
# picked up there with no service contention.

$ErrorActionPreference = 'Continue'

# Tee output to a stable log path under %TEMP% so users can attach a
# single file when reporting upgrade issues. The MSI session log captures
# our stdout too, but it lives in a path that varies per install and is
# discarded when the user closes the installer UI.
$transcriptPath = $null
try {
    $tempDir = if ([string]::IsNullOrWhiteSpace($env:TEMP)) {
        [System.IO.Path]::GetTempPath()
    } else {
        $env:TEMP
    }
    if (-not [string]::IsNullOrWhiteSpace($tempDir)) {
        $transcriptPath = Join-Path $tempDir 'luminalshine-installer-migration.log'
        Start-Transcript -Path $transcriptPath -Append -Force | Out-Null
        Write-Output ("=== LuminalShine installer-migrations.ps1 @ {0} ===" -f (Get-Date -Format 's'))
    }
} catch {
    # Transcript is purely diagnostic; never let its failure derail the
    # rest of the script.
    $transcriptPath = $null
}

function Convert-LegacySplitEncodeValue {
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Value
    )

    if ($null -eq $Value) {
        return $Value
    }

    if ($Value -is [bool]) {
        return $(if ($Value) { 'enabled' } else { 'disabled' })
    }

    if ($Value -is [byte] -or $Value -is [int16] -or $Value -is [int32] -or $Value -is [int64]) {
        if ([int64]$Value -eq 1) {
            return 'enabled'
        }
        if ([int64]$Value -eq 0) {
            return 'disabled'
        }
        return $Value
    }

    if ($Value -isnot [string]) {
        return $Value
    }

    $rawValue = $Value.Trim().ToLowerInvariant()
    switch ($rawValue) {
        { $_ -in @('true', 'yes', 'on', 'enable', 'enabled', '1') } { return 'enabled' }
        { $_ -in @('false', 'no', 'off', 'disable', 'disabled', '0') } { return 'disabled' }
        default { return $Value.Trim() }
    }
}

function Test-JsonFileParseable {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )
    if (-not (Test-Path -LiteralPath $Path)) {
        return $false
    }
    try {
        $item = Get-Item -LiteralPath $Path -ErrorAction Stop
        if ($item.Length -le 0) {
            return $false
        }
    } catch {
        return $false
    }
    try {
        $raw = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
        if ([string]::IsNullOrWhiteSpace($raw)) {
            return $false
        }
        $null = $raw | ConvertFrom-Json -ErrorAction Stop
        return $true
    } catch {
        return $false
    }
}

function Repoint-LegacyShortcuts {
    <#
    .SYNOPSIS
        Rewrite .lnk files whose target points at the pre-26.05.1 install root.

    .DESCRIPTION
        The MSI's MajorUpgrade machinery uninstalls files from the old install
        directory (C:\Program Files\Sunshine\) and re-lays them down at the new
        location (C:\Program Files\NortheBridge\LuminalShine\). MSI-authored
        shortcuts (Start Menu, etc.) are re-created automatically as part of
        the install, but USER-pinned shortcuts (taskbar, Desktop, manual Start
        Menu entries) embed an absolute target path and would break silently.

        This function walks every user profile on the machine and rewrites any
        .lnk whose TargetPath (and WorkingDirectory) begins with the old root.
        It runs from the RunInstallerMigrations custom action which is flagged
        Return="ignore" in WiX, so any failure here is non-fatal to the
        install transaction. Per-file failures are logged and we continue.

        The custom action runs as SYSTEM (Impersonate="no"), which is why we
        enumerate `C:\Users\<profile>\...` directly instead of using
        [Environment]::GetFolderPath — that would resolve relative to the
        SYSTEM profile, not the user.

    .PARAMETER OldInstallRoot
        Absolute path of the pre-migration install directory.

    .PARAMETER NewInstallRoot
        Absolute path of the post-migration install directory.
    #>
    param(
        [Parameter(Mandatory = $true)]
        [string]$OldInstallRoot,
        [Parameter(Mandatory = $true)]
        [string]$NewInstallRoot
    )

    # Append a trailing backslash so a path like "C:\Program Files\Sunshine"
    # matches the prefix of "C:\Program Files\Sunshine\sunshine.exe" but
    # NOT of "C:\Program Files\Sunshine2\..." (any sibling that happens to
    # share a name prefix).
    $oldPrefix = ($OldInstallRoot.TrimEnd('\', '/')) + '\'
    $newPrefix = ($NewInstallRoot.TrimEnd('\', '/')) + '\'

    $shortcutHosts = New-Object System.Collections.Generic.List[string]
    try {
        $usersRoot = Join-Path $env:SystemDrive 'Users'
        if (Test-Path -LiteralPath $usersRoot) {
            foreach ($profile in Get-ChildItem -LiteralPath $usersRoot -Directory -ErrorAction SilentlyContinue) {
                $candidates = @(
                    (Join-Path $profile.FullName 'Desktop'),
                    (Join-Path $profile.FullName 'AppData\Roaming\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar'),
                    (Join-Path $profile.FullName 'AppData\Roaming\Microsoft\Windows\Start Menu\Programs')
                )
                foreach ($c in $candidates) {
                    if (Test-Path -LiteralPath $c) { $shortcutHosts.Add($c) }
                }
            }
        }
        # Public Desktop: shortcuts visible to every user, owned by no one user.
        $publicDesktop = Join-Path $env:SystemDrive 'Users\Public\Desktop'
        if (Test-Path -LiteralPath $publicDesktop) { $shortcutHosts.Add($publicDesktop) }
        # Machine-wide Start Menu — MSI re-creates its own entries here on
        # upgrade, but third-party install managers and group-policy
        # deployments sometimes place .lnks here too.
        $machineStartMenu = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs'
        if (Test-Path -LiteralPath $machineStartMenu) { $shortcutHosts.Add($machineStartMenu) }
    } catch {
        Write-Output ("Repoint-LegacyShortcuts: host enumeration failed: {0}" -f $_.Exception.Message)
        return
    }

    $shortcutHosts = $shortcutHosts | Select-Object -Unique
    if (-not $shortcutHosts) {
        Write-Output 'Repoint-LegacyShortcuts: no shortcut directories found to scan.'
        return
    }

    $shell = $null
    try {
        $shell = New-Object -ComObject WScript.Shell
    } catch {
        Write-Output ("Repoint-LegacyShortcuts: WScript.Shell unavailable ({0}); skipping." -f $_.Exception.Message)
        return
    }

    # 26.05.1 also renames the executables themselves. A pinned `.lnk` that
    # used to point at `C:\Program Files\Sunshine\sunshine.exe` should now
    # resolve to `C:\Program Files\NortheBridge\LuminalShine\luminalshine.exe`
    # in one pass — so this map handles the *filename* component while the
    # prefix replacement above handles the *directory* component.
    $exeRenames = @{
        'sunshine.exe' = 'luminalshine.exe'
        'sunshinesvc.exe' = 'luminalshinesvc.exe'
        'sunshine_display_helper.exe' = 'luminalshine_display_helper.exe'
        'sunshine_wgc_capture.exe' = 'luminalshine_wgc_capture.exe'
        'dxgi-info.exe' = 'luminalshine-dxgi-info.exe'
        'audio-info.exe' = 'luminalshine-audio-info.exe'
        'playnite-launcher.exe' = 'luminalshine-playnite-launcher.exe'
    }

    $rewritten = 0
    try {
        foreach ($dir in $shortcutHosts) {
            $links = $null
            try {
                $links = Get-ChildItem -LiteralPath $dir -Filter '*.lnk' -File -Recurse -ErrorAction SilentlyContinue
            } catch {
                Write-Output ("Repoint-LegacyShortcuts: could not enumerate {0}: {1}" -f $dir, $_.Exception.Message)
                continue
            }
            foreach ($link in $links) {
                try {
                    $sc = $shell.CreateShortcut($link.FullName)
                    $target = [string]$sc.TargetPath
                    if ([string]::IsNullOrEmpty($target)) { continue }
                    if (-not $target.StartsWith($oldPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                        continue
                    }
                    $relative = $target.Substring($oldPrefix.Length)
                    $newTarget = Join-Path $newPrefix $relative

                    # Rewrite the filename leaf in the new target. The leaf
                    # match is case-insensitive so `Sunshine.exe`,
                    # `sunshine.EXE`, etc. are all handled.
                    $leaf = Split-Path -Path $newTarget -Leaf
                    foreach ($oldLeaf in $exeRenames.Keys) {
                        if ($leaf.Equals($oldLeaf, [System.StringComparison]::OrdinalIgnoreCase)) {
                            $parent = Split-Path -Path $newTarget -Parent
                            $newTarget = Join-Path $parent $exeRenames[$oldLeaf]
                            break
                        }
                    }

                    $workdir = [string]$sc.WorkingDirectory
                    $newWorkdir = $workdir
                    if (-not [string]::IsNullOrEmpty($workdir) -and
                        $workdir.StartsWith($oldPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                        $newWorkdir = Join-Path $newPrefix $workdir.Substring($oldPrefix.Length)
                    }

                    $sc.TargetPath = $newTarget
                    $sc.WorkingDirectory = $newWorkdir
                    $sc.Save()
                    $rewritten++
                    Write-Output ("Repoint-LegacyShortcuts: repointed {0}: {1} -> {2}" -f $link.FullName, $target, $newTarget)
                } catch {
                    Write-Output ("Repoint-LegacyShortcuts: failed to repoint {0}: {1}" -f $link.FullName, $_.Exception.Message)
                    continue
                }
            }
        }
    } finally {
        if ($shell) {
            try { [System.Runtime.InteropServices.Marshal]::ReleaseComObject($shell) | Out-Null } catch {}
        }
    }

    if ($rewritten -gt 0) {
        Write-Output ("Repoint-LegacyShortcuts: rewrote {0} legacy-install-path shortcut(s)." -f $rewritten)
    } else {
        Write-Output 'Repoint-LegacyShortcuts: no shortcuts referenced the legacy install path.'
    }
}

function Update-SplitFrameEncodingInConfig {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ConfigPath
    )

    if (-not (Test-Path -LiteralPath $ConfigPath)) {
        return $false
    }

    try {
        $original = Get-Content -LiteralPath $ConfigPath -Raw -ErrorAction Stop
    } catch {
        Write-Output ("Skipped {0}: {1}" -f $ConfigPath, $_.Exception.Message)
        return $false
    }
    if ([string]::IsNullOrWhiteSpace($original)) {
        return $false
    }

    $updated = [System.Text.RegularExpressions.Regex]::Replace(
        $original,
        '(?im)^(\s*)nvenc_force_split_encode(\s*=\s*)([^#;\r\n]+?)(\s*(?:[#;].*)?)$',
        {
            param($match)

            $convertedValue = Convert-LegacySplitEncodeValue $match.Groups[3].Value
            return '{0}nvenc_split_encode{1}{2}{3}' -f `
                $match.Groups[1].Value, `
                $match.Groups[2].Value, `
                $convertedValue, `
                $match.Groups[4].Value
        }
    )

    if ($updated -ceq $original) {
        return $false
    }

    try {
        Set-Content -LiteralPath $ConfigPath -Value $updated -NoNewline -Encoding UTF8 -ErrorAction Stop
        return $true
    } catch {
        Write-Output ("Failed to rewrite {0}: {1}" -f $ConfigPath, $_.Exception.Message)
        return $false
    }
}

function Convert-SplitFrameEncodingJsonNode {
    param(
        [AllowNull()]
        [object]$Node,
        [ref]$Changed
    )

    if ($null -eq $Node) {
        return $null
    }

    if ($Node -is [System.Management.Automation.PSCustomObject]) {
        $result = [ordered]@{}
        foreach ($property in $Node.PSObject.Properties) {
            $targetName = if ($property.Name -eq 'nvenc_force_split_encode') {
                $Changed.Value = $true
                'nvenc_split_encode'
            } else {
                $property.Name
            }

            $value = Convert-SplitFrameEncodingJsonNode -Node $property.Value -Changed $Changed
            if ($targetName -eq 'nvenc_split_encode') {
                $converted = Convert-LegacySplitEncodeValue $value
                if (($converted -is [string]) -or ($converted -ne $value)) {
                    if (-not (($converted -is [string]) -and ($value -is [string]) -and $converted -ceq $value)) {
                        $Changed.Value = $true
                    }
                }
                $value = $converted
            }

            if (-not $result.Contains($targetName)) {
                $result[$targetName] = $value
            }
        }
        return [pscustomobject]$result
    }

    if ($Node -is [System.Collections.IEnumerable] -and $Node -isnot [string]) {
        $result = @()
        foreach ($item in $Node) {
            $result += ,(Convert-SplitFrameEncodingJsonNode -Node $item -Changed $Changed)
        }
        return $result
    }

    return $Node
}

function Update-SplitFrameEncodingInJson {
    param(
        [Parameter(Mandatory = $true)]
        [string]$JsonPath
    )

    if (-not (Test-Path -LiteralPath $JsonPath)) {
        return $false
    }

    # Skip files we can't even parse: an attempted rewrite would either
    # clobber whatever salvageable bytes are still on disk or throw. The
    # C++ runtime's recovery path handles those files.
    if (-not (Test-JsonFileParseable -Path $JsonPath)) {
        Write-Output ("Skipped {0}: not a parseable JSON document" -f $JsonPath)
        return $false
    }

    try {
        $original = Get-Content -LiteralPath $JsonPath -Raw -ErrorAction Stop
    } catch {
        Write-Output ("Skipped {0}: {1}" -f $JsonPath, $_.Exception.Message)
        return $false
    }
    if ([string]::IsNullOrWhiteSpace($original)) {
        return $false
    }

    try {
        $parsed = $original | ConvertFrom-Json -ErrorAction Stop
    } catch {
        Write-Output ("Skipped {0}: invalid JSON ({1})" -f $JsonPath, $_.Exception.Message)
        return $false
    }

    $changed = $false
    $updated = Convert-SplitFrameEncodingJsonNode -Node $parsed -Changed ([ref]$changed)
    if (-not $changed) {
        return $false
    }

    try {
        $serialized = $updated | ConvertTo-Json -Depth 100
        Set-Content -LiteralPath $JsonPath -Value $serialized -NoNewline -Encoding UTF8 -ErrorAction Stop
        return $true
    } catch {
        Write-Output ("Failed to rewrite {0}: {1}" -f $JsonPath, $_.Exception.Message)
        return $false
    }
}

# Resolve the canonical %ProgramData%\LuminalShine\config\ path so we can
# also inspect any state files the C++ runtime may have already created
# there. We deliberately DO NOT copy from the legacy <INSTALL_ROOT>\config
# directory anymore — that move happens in platf::appdata() at first
# launch, where there is no service contention to race with.
$programDataDir = $null
try {
    $programData = $env:ProgramData
    if ([string]::IsNullOrWhiteSpace($programData)) {
        $programData = [Environment]::GetFolderPath('CommonApplicationData')
    }
    if (-not [string]::IsNullOrWhiteSpace($programData)) {
        $programDataDir = Join-Path $programData 'LuminalShine\config'
    }
} catch {
    $programDataDir = $null
}

$rootDir = Split-Path -Parent $PSScriptRoot
$candidateConfigs = @()
if ($programDataDir) {
    $candidateConfigs += (Join-Path $programDataDir 'sunshine.conf')
}
$candidateConfigs += @(
    (Join-Path $rootDir 'config\sunshine.conf'),
    (Join-Path $rootDir 'sunshine.conf')
)
$candidateConfigs = $candidateConfigs | Select-Object -Unique

$candidateJsonFiles = @()
if ($programDataDir) {
    $candidateJsonFiles += @(
        (Join-Path $programDataDir 'apps.json'),
        (Join-Path $programDataDir 'sunshine_state.json'),
        (Join-Path $programDataDir 'luminalshine_state.json')
    )
}
$candidateJsonFiles += @(
    (Join-Path $rootDir 'config\apps.json'),
    (Join-Path $rootDir 'apps.json'),
    (Join-Path $rootDir 'config\sunshine_state.json'),
    (Join-Path $rootDir 'sunshine_state.json'),
    (Join-Path $rootDir 'config\luminalshine_state.json'),
    (Join-Path $rootDir 'luminalshine_state.json')
)
$candidateJsonFiles = $candidateJsonFiles | Select-Object -Unique

$changedAny = $false
foreach ($configPath in $candidateConfigs) {
    try {
        if (Update-SplitFrameEncodingInConfig -ConfigPath $configPath) {
            Write-Output ("Migrated nvenc_force_split_encode to nvenc_split_encode in {0}" -f $configPath)
            $changedAny = $true
        }
    } catch {
        Write-Output ("Skipped {0}: {1}" -f $configPath, $_.Exception.Message)
    }
}

foreach ($jsonPath in $candidateJsonFiles) {
    try {
        if (Update-SplitFrameEncodingInJson -JsonPath $jsonPath) {
            Write-Output ("Migrated nvenc_force_split_encode to nvenc_split_encode in {0}" -f $jsonPath)
            $changedAny = $true
        }
    } catch {
        Write-Output ("Skipped {0}: {1}" -f $jsonPath, $_.Exception.Message)
    }
}

if (-not $changedAny) {
    Write-Output 'No installer config migrations were needed.'
}

# Repoint pinned/desktop/start-menu shortcuts left over from the
# pre-26.05.1 install layout at C:\Program Files\Sunshine\. This
# only rewrites .lnk files; MSI re-creates its own shortcuts on
# upgrade automatically, and the C++ runtime carries the same logic
# on first launch for anything we miss here (e.g. user logged out
# during the install).
try {
    Repoint-LegacyShortcuts `
        -OldInstallRoot 'C:\Program Files\Sunshine' `
        -NewInstallRoot 'C:\Program Files\NortheBridge\LuminalShine'
} catch {
    Write-Output ("Repoint-LegacyShortcuts top-level failure: {0}" -f $_.Exception.Message)
}

if ($transcriptPath) {
    try {
        Stop-Transcript | Out-Null
    } catch {
        # already stopped or never started — ignore
    }
}

# Always exit 0. RunInstallerMigrations is also flagged Return="ignore"
# in WiX, but belt-and-suspenders.
exit 0
