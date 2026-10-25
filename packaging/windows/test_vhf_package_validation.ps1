# Portable validation fixtures. Authenticode is mocked; no installation/trust changes.
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$template = Get-Content (Join-Path $repo 'src_assets/windows/drivers/vhf-gamepad/install.ps1.in') -Raw
$pin = '0123456789ABCDEF0123456789ABCDEF01234567'
$temp = Join-Path ([IO.Path]::GetTempPath()) ('luminalshine-vhf-test-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $temp
try {
    $script = Join-Path $temp 'install.ps1'
    [IO.File]::WriteAllText($script, $template.Replace('@SUNSHINE_VHF_GAMEPAD_SIGNER_THUMBPRINT@', $pin))
    $runner = Join-Path $temp 'runner.ps1'
    @'
param($Installer, $Package, $SignatureStatus, $Thumbprint)
function Get-AuthenticodeSignature {
    param($LiteralPath)
    return [pscustomobject]@{ Status = $SignatureStatus; SignerCertificate = [pscustomobject]@{ Thumbprint = $Thumbprint } }
}
& $Installer -ValidateOnly -PackageRoot $Package
'@ | Set-Content $runner
    foreach ($case in @('valid', 'tampered-inf', 'missing-dll', 'wrong-signer', 'untrusted', 'wrong-protocol')) {
        $root = Join-Path $temp $case
        $null = New-Item -ItemType Directory -Path "$root/driver", "$root/tools"
        $inf = "[Version]`nDriverVer=10/16/2026,0.1.0.53`n[Models]`nFake=Install,ROOT\VIBESHINEVIRTUALGAMEPAD`n"
        [IO.File]::WriteAllText("$root/driver/VibeshineVhfGamepad.inf", $inf)
        [IO.File]::WriteAllText("$root/driver/VibeshineVhfGamepad.dll", 'fixture driver')
        [IO.File]::WriteAllText("$root/driver/VibeshineVhfGamepad.cat", 'fixture catalog')
        [IO.File]::WriteAllText("$root/tools/VibeshineVhfGamepadDeviceSetup.exe", 'fixture executable never executed')
        $files = @('driver/VibeshineVhfGamepad.inf','driver/VibeshineVhfGamepad.dll','driver/VibeshineVhfGamepad.cat','tools/VibeshineVhfGamepadDeviceSetup.exe')
        $manifest = @{
            schema_version = 1; source_revision = '7b000a2b060b4daeb0b5c6aa7b77ae1d81857e7a'
            driver_ver = '10/16/2026,0.1.0.53'; protocol_version = 2; platform = 'x64'
            signing = @{ channel = 'msi-request-signing' }
            files = @($files | ForEach-Object { @{ path = $_; sha256 = (Get-FileHash "$root/$_").Hash } })
        }
        $status = 'Valid'; $signer = $pin
        switch ($case) {
            'tampered-inf' { Add-Content "$root/driver/VibeshineVhfGamepad.inf" '; modified' }
            'missing-dll' { Remove-Item "$root/driver/VibeshineVhfGamepad.dll" }
            'wrong-signer' { $signer = 'FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF' }
            'untrusted' { $status = 'NotTrusted' }
            'wrong-protocol' { $manifest.protocol_version = 1 }
        }
        $manifest | ConvertTo-Json -Depth 5 | Set-Content "$root/manifest.json"
        $output = & (Join-Path $PSHOME 'pwsh') -NoProfile -File $runner $script $root $status $signer 2>&1
        $passed = $LASTEXITCODE -eq 0
        if ($passed -ne ($case -eq 'valid')) { throw "Unexpected result for ${case}: $output" }
        Write-Output "PASS $case"
    }
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force
}
