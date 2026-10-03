<#
.SYNOPSIS
Waits for the local LuminalShine GameStream control plane to become ready.

.DESCRIPTION
Polls the unauthenticated HTTP /serverinfo endpoint used by GameStream
discovery. PowerShell may expose a response without a Content-Type as either
a String or a Byte array, so the response body is normalized to UTF-8 before
XML validation.

Use -SelfTest to exercise both response representations without requiring a
running LuminalShine installation.
#>

[CmdletBinding(DefaultParameterSetName = 'Live')]
param(
    [Parameter(ParameterSetName = 'Live')]
    [uri] $Uri = 'http://127.0.0.1:47989/serverinfo',

    [Parameter(ParameterSetName = 'Live')]
    [ValidateRange(1, 600)]
    [int] $TimeoutSeconds = 180,

    [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
    [switch] $SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function ConvertFrom-LuminalShineServerInfoContent {
    param(
        [Parameter(Mandatory)]
        [object] $Content
    )

    $body = if ($Content -is [byte[]]) {
        [Text.Encoding]::UTF8.GetString([byte[]] $Content)
    }
    elseif ($Content -is [string]) {
        [string] $Content
    }
    else {
        throw "Unsupported serverinfo response type: $($Content.GetType().FullName)"
    }

    try {
        [xml] $document = $body
    }
    catch {
        throw "LuminalShine serverinfo did not contain valid XML: $($_.Exception.Message)"
    }

    if ([string] $document.root.status_code -ne '200') {
        throw "LuminalShine serverinfo returned protocol status '$($document.root.status_code)'."
    }

    foreach ($requiredElement in @('hostname', 'appversion', 'state')) {
        if ([string]::IsNullOrWhiteSpace([string] $document.root.$requiredElement)) {
            throw "LuminalShine serverinfo omitted '$requiredElement'."
        }
    }

    $state = [string] $document.root.state
    if ($state -notin @('SUNSHINE_SERVER_FREE', 'SUNSHINE_SERVER_BUSY')) {
        throw "LuminalShine serverinfo returned unexpected state '$state'."
    }

    return $document
}

function Invoke-SelfTest {
    $fixture = @'
<?xml version="1.0" encoding="utf-8"?>
<root status_code="200">
  <hostname>TestHost</hostname>
  <appversion>7.1.431.-1</appversion>
  <state>SUNSHINE_SERVER_FREE</state>
</root>
'@

    $stringResult = ConvertFrom-LuminalShineServerInfoContent -Content $fixture
    if ([string] $stringResult.root.hostname -ne 'TestHost') {
        throw 'String response self-test returned the wrong hostname.'
    }

    $bytes = [Text.Encoding]::UTF8.GetBytes($fixture)
    $byteResult = ConvertFrom-LuminalShineServerInfoContent -Content $bytes
    if ([string] $byteResult.root.state -ne 'SUNSHINE_SERVER_FREE') {
        throw 'Byte-array response self-test returned the wrong state.'
    }

    $invalidFixture = $fixture.Replace('status_code="200"', 'status_code="503"')
    try {
        $null = ConvertFrom-LuminalShineServerInfoContent -Content $invalidFixture
        throw 'Invalid protocol status was accepted.'
    }
    catch {
        if ($_.Exception.Message -eq 'Invalid protocol status was accepted.') {
            throw
        }
    }

    Write-Output 'LuminalShine serverinfo health-check self-test passed.'
}

if ($SelfTest) {
    Invoke-SelfTest
    exit 0
}

$timer = [Diagnostics.Stopwatch]::StartNew()
$lastFailure = $null

while ($timer.Elapsed -lt [TimeSpan]::FromSeconds($TimeoutSeconds)) {
    try {
        $response = Invoke-WebRequest `
            -UseBasicParsing `
            -Uri $Uri `
            -TimeoutSec 3

        if ($response.StatusCode -ne 200) {
            throw "HTTP status $($response.StatusCode)"
        }

        $serverInfo = ConvertFrom-LuminalShineServerInfoContent -Content $response.Content

        [pscustomobject]@{
            Uri        = $Uri.AbsoluteUri
            Hostname   = [string] $serverInfo.root.hostname
            AppVersion = [string] $serverInfo.root.appversion
            State      = [string] $serverInfo.root.state
            ElapsedMs  = [int64] $timer.Elapsed.TotalMilliseconds
        }
        exit 0
    }
    catch {
        $lastFailure = $_.Exception.Message
    }

    Start-Sleep -Seconds 1
}

throw "LuminalShine GameStream did not become ready within $TimeoutSeconds seconds. Last failure: $lastFailure"
