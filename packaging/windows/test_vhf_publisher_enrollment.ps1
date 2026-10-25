# Execute production transaction functions with an in-memory store only.
# No certificate-store API, native installer or signing service is invoked.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$sourcePath = Join-Path $repo 'src_assets/windows/drivers/vhf-gamepad/install.ps1.in'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($sourcePath, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors -join "`n") }
foreach ($function in $ast.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.FunctionDefinitionAst] }) {
    # Exclude the real Windows boundaries entirely, then supply mocks below.
    if ($function.Name -eq 'Install-VhfWithVerifiedPublisher') { Invoke-Expression $function.Extent.Text }
}
$expectedSigner = '0123456789ABCDEF0123456789ABCDEF01234567'
$certificate = [pscustomobject]@{ Thumbprint = $expectedSigner }
$otherCertificate = [pscustomobject]@{ Thumbprint = 'FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF' }
function New-VhfPublisherStore { $script:storeRequests++; return $script:store }
function Invoke-VhfDriverStaging { param($InfPath); $script:stages++; return $script:stageCode }
function Invoke-VhfDeviceSetup { param($SetupPath, $InfPath); $script:setups++; return $script:setupCode }
function Assert-True($Value, [string]$Message) { if (-not $Value) { throw $Message } }

$cases = @('new-success', 'existing-success', 'staging-failure', 'setup-failure',
    'existing-failure', 'wrong-signer', 'untrusted', 'add-failure', 'add-not-persisted',
    'rollback-failure', 'close-failure', 'staging-reboot', 'setup-reboot')
foreach ($case in $cases) {
    $script:storeRequests = 0; $script:stages = 0; $script:setups = 0
    $script:stageCode = 0; $script:setupCode = 0
    $script:store = [pscustomobject]@{
        Entries = [Collections.ArrayList]::new(); Adds = 0; Removes = 0; Opens = 0; Closes = 0
        AddFails = $false; DropAdd = $false; RemoveFails = $false; CloseFails = $false
    }
    $script:store | Add-Member ScriptProperty Certificates { return $this.Entries.ToArray() }
    $script:store | Add-Member ScriptMethod Open { param($Flags); $this.Opens++ }
    $script:store | Add-Member ScriptMethod Close { $this.Closes++; if ($this.CloseFails) { throw 'mock close failure' } }
    $script:store | Add-Member ScriptMethod Add {
        param($Certificate)
        $this.Adds++
        if ($this.AddFails) { throw 'mock add failure' }
        if (-not $this.DropAdd) { $null = $this.Entries.Add($Certificate) }
    }
    $script:store | Add-Member ScriptMethod Remove {
        param($Certificate)
        $this.Removes++
        if ($this.RemoveFails) { throw 'mock removal failure' }
        $this.Entries.Remove($Certificate)
    }
    $null = $store.Entries.Add($otherCertificate)
    $signature = [pscustomobject]@{ Status = 'Valid'; SignerCertificate = $certificate }
    $expectedFailure = $false
    switch ($case) {
        'existing-success' { $null = $store.Entries.Add($certificate) }
        'staging-failure' { $script:stageCode = 5; $expectedFailure = $true }
        'setup-failure' { $script:setupCode = 5; $expectedFailure = $true }
        'existing-failure' { $null = $store.Entries.Add($certificate); $script:stageCode = 5; $expectedFailure = $true }
        'wrong-signer' { $signature.SignerCertificate = $otherCertificate; $expectedFailure = $true }
        'untrusted' { $signature.Status = 'NotTrusted'; $expectedFailure = $true }
        'add-failure' { $store.AddFails = $true; $expectedFailure = $true }
        'add-not-persisted' { $store.DropAdd = $true; $expectedFailure = $true }
        'rollback-failure' { $store.RemoveFails = $true; $script:stageCode = 5; $expectedFailure = $true }
        'close-failure' { $store.CloseFails = $true }
        'staging-reboot' { $script:stageCode = 3010 }
        'setup-reboot' { $script:setupCode = 3010 }
    }
    $failure = $null; $restart = $false
    try { $restart = Install-VhfWithVerifiedPublisher $signature 'fixture.inf' 'fixture.exe' }
    catch { $failure = $_ }
    Assert-True (($null -ne $failure) -eq $expectedFailure) "Unexpected outcome for $case"
    Assert-True ($store.Entries.Contains($otherCertificate)) "Unrelated publisher removed in $case"
    if ($case -in @('wrong-signer', 'untrusted')) {
        Assert-True ($storeRequests -eq 0 -and $stages -eq 0 -and $setups -eq 0) "Invalid signature touched Windows boundary: $case"
    } else {
        Assert-True ($store.Opens -eq 1 -and $store.Closes -eq 1) "Store lifecycle incomplete in $case"
    }
    if ($case -like 'existing-*') {
        Assert-True ($store.Adds -eq 0 -and $store.Removes -eq 0 -and $store.Entries.Contains($certificate)) "Preexisting trust changed: $case"
    }
    if ($case -in @('staging-failure','setup-failure','add-not-persisted')) {
        Assert-True ($store.Removes -eq 1 -and -not $store.Entries.Contains($certificate)) "New enrollment not rolled back: $case"
    }
    if ($case -in @('staging-failure','existing-failure','rollback-failure','add-failure','add-not-persisted')) {
        Assert-True ($setups -eq 0) "Setup ran after preceding failure: $case"
    }
    if ($case -eq 'rollback-failure') {
        Assert-True ($failure.ToString() -like '*Driver staging failed (5)*' -and $store.Entries.Contains($certificate)) 'Rollback masked original failure'
    }
    if (-not $expectedFailure) {
        Assert-True ($store.Entries.Contains($certificate)) "Success did not retain publisher: $case"
        Assert-True ($restart -eq ($case -in @('staging-reboot','setup-reboot'))) "Wrong reboot result in $case"
    }
    Write-Output "PASS $case"
}

# Verify the actual top-level transaction remains behind validation/admin and
# retained-version early returns; the standalone function tests above bypass
# those main-program gates intentionally, without executing Windows APIs.
$text = Get-Content -LiteralPath $sourcePath -Raw
$call = $text.LastIndexOf('$restartRequired = Install-VhfWithVerifiedPublisher')
foreach ($guard in @('if ($ValidateOnly)', 'Administrator rights are required.', 'Newer shared VHF package retained.', 'Equal/newer shared VHF driver retained.')) {
    Assert-True ($text.IndexOf($guard) -ge 0 -and $text.IndexOf($guard) -lt $call) "Missing pre-enrollment gate: $guard"
}
Write-Output 'PASS enrollment follows validation/admin/version gates'
