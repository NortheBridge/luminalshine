#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,
    [string]$MsiPath = "",
    [string]$OutputName = "",
    [switch]$UninstallOnly
)

$ErrorActionPreference = "Stop"

function Resolve-PathStrict([string]$Path) {
    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    return $resolved.ProviderPath
}

function Find-LatestMsi([string]$Directory) {
    if (-not (Test-Path -LiteralPath $Directory)) {
        throw "MSI output directory does not exist: $Directory"
    }

    $candidate = Get-ChildItem -LiteralPath $Directory -Filter "*.msi" -File |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if (-not $candidate) {
        throw "No MSI payload found in $Directory"
    }

    return $candidate.FullName
}

function Get-GitTagVersion([string]$RepoRoot) {
    $tagPatterns = @(
        '[0-9]*.[0-9]*.[0-9]*',
        'v[0-9]*.[0-9]*.[0-9]*'
    )

    try {
        foreach ($tagPattern in $tagPatterns) {
            $rawCandidates = git -C $RepoRoot tag --merged HEAD --sort=-version:refname --list $tagPattern 2>$null
            foreach ($candidate in $rawCandidates) {
                $rawTag = [string]$candidate
                if ([string]::IsNullOrWhiteSpace($rawTag)) {
                    continue
                }

                $rawTag = $rawTag.Trim()
                if ($rawTag -match '^v?(\d+)\.(\d+)\.(\d+)(?:([.-][0-9A-Za-z.-]+))?$') {
                    return @{
                        Tag = $rawTag
                        Major = [int]$matches[1]
                        Minor = [int]$matches[2]
                        Patch = [int]$matches[3]
                    }
                }
            }
        }
    } catch {
    }

    try {
        $rawTag = (git -C $RepoRoot describe --tags --abbrev=0 2>$null).Trim()
        if ($rawTag -match '^v?(\d+)\.(\d+)\.(\d+)(?:([.-][0-9A-Za-z.-]+))?$') {
            return @{
                Tag = $rawTag
                Major = [int]$matches[1]
                Minor = [int]$matches[2]
                Patch = [int]$matches[3]
            }
        }
    } catch {
    }

    return $null
}

function Get-GitInformationalVersion([string]$RepoRoot, [string]$fallbackTag) {
    try {
        $desc = (git -C $RepoRoot describe --tags --dirty --always 2>$null).Trim()
        if (-not [string]::IsNullOrWhiteSpace($desc)) {
            return $desc
        }
    } catch {
    }
    return $fallbackTag
}

function Get-MsiProductVersion([string]$MsiPath) {
    if ([string]::IsNullOrWhiteSpace($MsiPath) -or -not (Test-Path -LiteralPath $MsiPath)) {
        return $null
    }

    $installer = $null
    $database = $null
    $view = $null
    $record = $null
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $database = $installer.GetType().InvokeMember("OpenDatabase", [System.Reflection.BindingFlags]::InvokeMethod, $null, $installer, @($MsiPath, 0))
        $view = $database.GetType().InvokeMember("OpenView", [System.Reflection.BindingFlags]::InvokeMethod, $null, $database, @("SELECT `Value` FROM `Property` WHERE `Property`='ProductVersion'"))
        $view.GetType().InvokeMember("Execute", [System.Reflection.BindingFlags]::InvokeMethod, $null, $view, $null) | Out-Null
        $record = $view.GetType().InvokeMember("Fetch", [System.Reflection.BindingFlags]::InvokeMethod, $null, $view, $null)
        if ($null -ne $record) {
            return [string]$record.StringData(1)
        }
    } catch {
    } finally {
        foreach ($com in @($record, $view, $database, $installer)) {
            if ($null -ne $com) {
                try {
                    [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($com)
                } catch {
                }
            }
        }
    }

    return $null
}

function Resolve-CscPath {
    <#
        Locate a Roslyn-class csc.exe — i.e. one that supports C# 6 and
        newer language features. We deliberately do NOT fall back to the
        .NET Framework 4.x csc at
            $env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe
        because that compiler caps at C# 5 and silently rejects modern
        syntax (read-only auto-properties, null-conditional `?.`, string
        interpolation, nameof, expression-bodied members, etc.) that
        LuminalShineInstaller.cs is allowed to use under the
        `-langversion:7.3` flag passed to csc later in this script.

        Resolution order:
          1. $env:LUMINALSHINE_CSC_PATH — explicit override, escape
             hatch for an unusual environment.
          2. `vswhere.exe` — the Visual Studio installer's locator tool,
             always present alongside the VS installer. Asks for the
             latest VS instance with MSBuild and derives the Roslyn
             csc.exe path relative to its installation root. Works for
             VS Community / Professional / Enterprise / Build Tools at
             any install path.
          3. A short hardcoded list — GitHub Actions windows-2022
             runners (VS 2022 Enterprise at the default path) and the
             historical custom dev install at `D:\Software\Visual Studio\`
             that an earlier maintainer used. Pure defence-in-depth in
             case vswhere returns something unexpected.

        If none of those produce a working compiler, throw with a clear
        message rather than silently downgrading to the .NET 4 csc and
        producing CS0840 / CS1525 errors at compile time.
    #>

    if (-not [string]::IsNullOrWhiteSpace($env:LUMINALSHINE_CSC_PATH) -and
        (Test-Path -LiteralPath $env:LUMINALSHINE_CSC_PATH)) {
        return (Resolve-PathStrict $env:LUMINALSHINE_CSC_PATH)
    }

    # vswhere.exe is shipped by the VS installer at a stable location
    # under Program Files (x86) on every Windows host that has any
    # edition of VS 2017+ installed.
    $vswhereCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
        (Join-Path $env:ProgramFiles            "Microsoft Visual Studio\Installer\vswhere.exe")
    )
    foreach ($vswhereCandidate in $vswhereCandidates) {
        if (-not (Test-Path -LiteralPath $vswhereCandidate)) {
            continue
        }
        try {
            $installRoot = (& $vswhereCandidate `
                -latest `
                -products * `
                -requires Microsoft.Component.MSBuild `
                -property installationPath 2>$null) | Select-Object -First 1
        } catch {
            $installRoot = $null
        }
        if (-not [string]::IsNullOrWhiteSpace($installRoot)) {
            $candidate = Join-Path $installRoot "MSBuild\Current\Bin\Roslyn\csc.exe"
            if (Test-Path -LiteralPath $candidate) {
                return (Resolve-PathStrict $candidate)
            }
        }
    }

    $hardcodedCandidates = @(
        # GitHub Actions windows-2022 runner default path
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\Roslyn\csc.exe",
        # VS Build Tools 2022 (when Enterprise isn't installed)
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe",
        # Historical custom dev install on D:
        "D:\Software\Visual Studio\MSBuild\Current\Bin\Roslyn\csc.exe"
    )
    foreach ($candidate in $hardcodedCandidates) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-PathStrict $candidate)
        }
    }

    throw @"
Could not locate a Roslyn-class csc.exe (C# 6+ compiler).

The legacy .NET Framework 4.x csc at
    $env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe
is intentionally NOT used as a fallback because it caps at C# 5
and rejects modern C# syntax we depend on
(read-only auto-properties, null-conditional `?.`, etc.).

To fix, do one of:
  - Install Visual Studio 2022 Build Tools or Community (free), which
    bundles Roslyn at <VS install root>\MSBuild\Current\Bin\Roslyn\csc.exe.
  - Set the LUMINALSHINE_CSC_PATH environment variable to the absolute
    path of a Roslyn csc.exe you already have.
"@
}

$scriptDir = Split-Path -Parent $PSCommandPath
$repoRoot = Resolve-PathStrict (Join-Path $scriptDir "..\..\..")
$buildRoot = Resolve-PathStrict $BuildDir
$artifactDir = Join-Path $buildRoot "cpack_artifacts"

if (-not $UninstallOnly) {
    if ([string]::IsNullOrWhiteSpace($MsiPath)) {
        $MsiPath = Find-LatestMsi -Directory $artifactDir
    } else {
        $MsiPath = Resolve-PathStrict $MsiPath
    }
}

$sourceFile = Resolve-PathStrict (Join-Path $scriptDir "LuminalShineInstaller.cs")
$manifestFile = Resolve-PathStrict (Join-Path $scriptDir "app.manifest")
# Keep the branded product icon on the bootstrapper; the installer now uses
# explicit process/window shell metadata to stay distinct from the installed app.
$iconPath = Resolve-PathStrict (Join-Path $repoRoot "sunshine.ico")
$licensePath = Resolve-PathStrict (Join-Path $repoRoot "LICENSE")
$cscPath = Resolve-CscPath
$frameworkRoot = Resolve-PathStrict "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$wpfRoot = Resolve-PathStrict (Join-Path $frameworkRoot "WPF")

if (-not (Test-Path -LiteralPath $artifactDir)) {
    New-Item -ItemType Directory -Path $artifactDir -Force | Out-Null
}

if ([string]::IsNullOrWhiteSpace($OutputName)) {
    if ($UninstallOnly) {
        $OutputName = "uninstall.exe"
    } else {
        $OutputName = "LuminalShineSetup.exe"
    }
}

$outputPath = Join-Path $artifactDir $OutputName

$tagVersion = Get-GitTagVersion -RepoRoot $repoRoot
$fallbackTag = if ($null -eq $tagVersion) { "" } else { $tagVersion.Tag }
$informationalVersion = Get-GitInformationalVersion -RepoRoot $repoRoot -fallbackTag $fallbackTag
$assemblyVersion = $null

if (-not $UninstallOnly) {
    $msiProductVersion = Get-MsiProductVersion -MsiPath $MsiPath
    if (-not [string]::IsNullOrWhiteSpace($msiProductVersion) -and $msiProductVersion -match '^(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?') {
        $revision = if ([string]::IsNullOrWhiteSpace($matches[4])) { 0 } else { [int]$matches[4] }
        $assemblyVersion = "{0}.{1}.{2}.{3}" -f [int]$matches[1], [int]$matches[2], [int]$matches[3], $revision
        if ([string]::IsNullOrWhiteSpace($informationalVersion)) {
            $informationalVersion = $msiProductVersion
        }
    }
}

if ([string]::IsNullOrWhiteSpace($assemblyVersion) -and $null -ne $tagVersion) {
    $assemblyVersion = "{0}.{1}.{2}.0" -f $tagVersion.Major, $tagVersion.Minor, $tagVersion.Patch
}

if ([string]::IsNullOrWhiteSpace($assemblyVersion)) {
    $assemblyVersion = "0.0.0.0"
    Write-Warning "Could not determine installer assembly version from MSI payload or git tag. Falling back to $assemblyVersion."
}

if ([string]::IsNullOrWhiteSpace($informationalVersion)) {
    $informationalVersion = $assemblyVersion
}
if ($UninstallOnly) {
    $assemblyInfoPath = Join-Path $artifactDir "LuminalShineUninstall.AssemblyInfo.cs"
    $assemblyTitle = "LuminalShine Uninstaller"
} else {
    $assemblyInfoPath = Join-Path $artifactDir "LuminalShineInstaller.AssemblyInfo.cs"
    $assemblyTitle = "LuminalShine Installer"
}
$assemblyInfoContent = @(
    "using System.Reflection;",
    "[assembly: AssemblyTitle(""$assemblyTitle"")]",
    "[assembly: AssemblyDescription(""$assemblyTitle"")]",
    "[assembly: AssemblyProduct(""$assemblyTitle"")]",
    "[assembly: AssemblyCompany(""NortheBridge Foundation"")]",
    "[assembly: AssemblyVersion(""$assemblyVersion"")]",
    "[assembly: AssemblyFileVersion(""$assemblyVersion"")]",
    "[assembly: AssemblyInformationalVersion(""$informationalVersion"")]"
)
Set-Content -Path $assemblyInfoPath -Value $assemblyInfoContent -Encoding UTF8

$references = @(
    (Join-Path $frameworkRoot "System.dll"),
    (Join-Path $frameworkRoot "System.Core.dll"),
    (Join-Path $frameworkRoot "System.Data.dll"),
    (Join-Path $frameworkRoot "System.Xml.dll"),
    (Join-Path $frameworkRoot "System.Xaml.dll"),
    (Join-Path $frameworkRoot "System.Windows.Forms.dll"),
    (Join-Path $wpfRoot "WindowsBase.dll"),
    (Join-Path $wpfRoot "PresentationCore.dll"),
    (Join-Path $wpfRoot "PresentationFramework.dll")
)

$args = @(
    "/nologo",
    "/target:winexe",
    "/optimize+",
    "/utf8output",
    # Explicit language level so LuminalShineInstaller.cs can use C# 6 / 7 features
    # (read-only auto-properties, null-conditional `?.`, string interpolation,
    # nameof, expression-bodied members, etc.). Resolve-CscPath above guarantees
    # the csc.exe we picked is Roslyn-class and supports this level. We pin to 7.3
    # rather than `latest` for reproducibility — `latest` would silently drift as
    # the runner's VS install gets newer compilers, and we don't need anything
    # beyond C# 7.3 in this file. Bump deliberately if a later language feature
    # becomes useful.
    "/langversion:7.3",
    "/out:$outputPath",
    "/win32manifest:$manifestFile",
    "/win32icon:$iconPath",
    "/resource:$licensePath,License.txt"
)

if ($UninstallOnly) {
    $args += "/define:UNINSTALL_ONLY"
} else {
    $args += "/resource:$MsiPath,Payload.msi"
}

foreach ($reference in $references) {
    if (-not (Test-Path -LiteralPath $reference)) {
        throw "Missing reference assembly: $reference"
    }
    $args += "/reference:$reference"
}

$args += $assemblyInfoPath
$args += $sourceFile

if ($UninstallOnly) {
    Write-Host "[bootstrapper] Building lightweight uninstaller EXE..."
} else {
    Write-Host "[bootstrapper] Building custom installer EXE..."
    Write-Host "[bootstrapper] Input MSI: $MsiPath"
}
Write-Host "[bootstrapper] Output EXE: $outputPath"
Write-Host "[bootstrapper] Compiler: $cscPath"
Write-Host "[bootstrapper] Version: $assemblyVersion ($informationalVersion)"

& $cscPath @args
if ($LASTEXITCODE -ne 0) {
    throw "C# compiler failed with exit code $LASTEXITCODE"
}

if ($UninstallOnly) {
    Write-Host "[bootstrapper] Lightweight uninstaller build complete."
} else {
    Write-Host "[bootstrapper] Custom installer build complete."
}
