[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$OutDir)
# Fixed producer identity. Update alongside the client submodule and installer.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$tag = 'v0.1.0-beta.9'
$archiveName = 'libvirtualgamepad-0.1.0-beta.9-windows-x64.zip'
$expectedHash = '59e89ab90c6e360e3e23bb4717b43339c4ec0050aedc52fe05e4f3ef306d62ee'
$revision = '7b000a2b060b4daeb0b5c6aa7b77ae1d81857e7a'
$payloads = @('driver/VibeshineVhfGamepad.inf', 'driver/VibeshineVhfGamepad.dll',
    'driver/VibeshineVhfGamepad.cat', 'tools/VibeshineVhfGamepadDeviceSetup.exe')
$layout = @($payloads) + @('manifest.json')
$OutDir = [IO.Path]::GetFullPath($OutDir)
if (Test-Path -LiteralPath $OutDir) { throw 'Output directory must not already exist.' }
$gitlink = (& git rev-parse HEAD:third-party/libvirtualgamepad).Trim()
if ($LASTEXITCODE -ne 0 -or $gitlink -cne $revision) { throw 'Client submodule pin differs from the driver release.' }
$temp = Join-Path ([IO.Path]::GetTempPath()) ('luminalshine-vhf-download-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $temp
try {
    $archive = Join-Path $temp $archiveName
    Invoke-WebRequest -Uri "https://github.com/Nonary/libvirtualgamepad/releases/download/$tag/$archiveName" -OutFile $archive
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ine $expectedHash) {
        throw 'VHF producer archive SHA-256 mismatch; nothing will be signed.'
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        # An exact, case-sensitive layout also rejects duplicate names and traversal.
        if ((@($zip.Entries | ForEach-Object { $_.FullName } | Sort-Object) -join "`n") -cne
            (($layout | Sort-Object) -join "`n")) { throw 'Unexpected producer ZIP layout.' }
    } finally { $zip.Dispose() }
    $staged = Join-Path $temp 'package'
    [IO.Compression.ZipFile]::ExtractToDirectory($archive, $staged)
    $manifest = Get-Content -LiteralPath "$staged/manifest.json" -Raw | ConvertFrom-Json
    if ($manifest.schema_version -ne 1 -or $manifest.source_revision -cne $revision -or
        $manifest.driver_ver -cne '10/16/2026,0.1.0.53' -or $manifest.protocol_version -ne 2 -or
        $manifest.platform -cne 'x64' -or $manifest.signing.channel -cne 'msi-request-signing') {
        throw 'Pinned archive manifest has unexpected release metadata.'
    }
    if ((@($manifest.files | ForEach-Object { $_.path } | Sort-Object) -join "`n") -cne
        (($payloads | Sort-Object) -join "`n")) { throw 'Unexpected producer manifest layout.' }
    foreach ($relative in $payloads) {
        $file = Join-Path $staged $relative
        $entry = @($manifest.files | Where-Object { $_.path -ceq $relative })
        if ($entry.Count -ne 1 -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ine $entry[0].sha256) {
            throw "Producer payload hash mismatch: $relative"
        }
        if ($relative -notlike '*.inf') {
            $signature = Get-AuthenticodeSignature -LiteralPath $file
            if ($signature.Status -ne 'NotSigned' -or $null -ne $signature.SignerCertificate) {
                throw "Expected unsigned producer payload before signing: $relative"
            }
        }
    }
    Move-Item -LiteralPath $staged -Destination $OutDir
    Write-Output "Verified pinned VHF package staged at $OutDir"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force
}
