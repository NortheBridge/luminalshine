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
