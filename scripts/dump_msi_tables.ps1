#Requires -Version 5.1
<#
.SYNOPSIS
    Dump the upgrade-critical tables of an MSI to a normalized, sorted
    text form for use as a WiX-migration compatibility oracle.

.DESCRIPTION
    Windows-only. Uses the WindowsInstaller.Installer COM object (always
    present on Windows; the same interface build_bootstrapper.ps1 already
    uses to read ProductVersion) so this has no external dependencies.

    The output format is intentionally dumb and faithful: one section per
    table, one line per row, columns sorted by name, rows sorted lexically.
    All values are emitted verbatim — normalization of volatile fields
    (ProductCode, ProductVersion, file sequence/size, cab names) is the
    differ's job (scripts/diff_msi_tables.py), which keeps this dumper
    trivial and lets the diff logic be unit-tested on any platform.

    The set of tables dumped is the set that governs Windows Installer
    upgrade compatibility and the bootstrapper<->MSI property contract:
    Property, Upgrade, Component, Directory, Feature, FeatureComponents,
    ServiceInstall, ServiceControl, Shortcut, CustomAction,
    InstallExecuteSequence, InstallUISequence, RegistryValue (subset of
    the keys we care about is left to the differ).

.PARAMETER MsiPath
    Path to the .msi to dump.

.PARAMETER OutFile
    Optional output path. Defaults to stdout.

.EXAMPLE
    pwsh scripts/dump_msi_tables.ps1 -MsiPath build/cpack_artifacts/LuminalShine.msi -OutFile wix3_baseline.txt
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$MsiPath,
    [string]$OutFile = ""
)

$ErrorActionPreference = "Stop"

# Tables whose contents drive upgrade compatibility and the
# bootstrapper<->MSI contract. Order here is the section order in the
# output. A table that does not exist in the MSI is emitted as an empty
# section (so a table appearing/disappearing across versions is itself a
# visible diff rather than a silent omission).
$TablesToDump = @(
    "Property",
    "Upgrade",
    "Component",
    "Directory",
    "Feature",
    "FeatureComponents",
    "ServiceInstall",
    "ServiceControl",
    "Shortcut",
    "CustomAction",
    "InstallExecuteSequence",
    "InstallUISequence",
    "RegistryValue",
    # Condition table holds the feature-level Conditions emitted by
    # scripts/gen_wix_files.py for the virtual-display backends — the
    # rows that gate the mttvdd / sudovda CM_C_* features on the
    # bootstrapper-supplied INSTALL_MTTVDD / INSTALL_SUDOVDA properties.
    # Without these, the deferred install.ps1 custom actions could fire
    # against files that were never staged. Added to the dump so the
    # oracle catches regressions to the feature-condition wiring.
    "Condition"
)

function Get-MsiTableColumns {
    param($Database, [string]$Table)
    # _Columns is the MSI metadata table. Query it for the column names of
    # $Table in their defined order.
    $columns = New-Object System.Collections.Generic.List[string]
    $query = "SELECT ``Name`` FROM ``_Columns`` WHERE ``Table``='$Table' ORDER BY ``Number``"
    $view = $Database.GetType().InvokeMember("OpenView", [System.Reflection.BindingFlags]::InvokeMethod, $null, $Database, @($query))
    $view.GetType().InvokeMember("Execute", [System.Reflection.BindingFlags]::InvokeMethod, $null, $view, $null) | Out-Null
    while ($true) {
        $record = $view.GetType().InvokeMember("Fetch", [System.Reflection.BindingFlags]::InvokeMethod, $null, $view, $null)
        if ($null -eq $record) { break }
        $name = $record.GetType().InvokeMember("StringData", [System.Reflection.BindingFlags]::GetProperty, $null, $record, @(1))
        $columns.Add([string]$name)
    }
    $view.GetType().InvokeMember("Close", [System.Reflection.BindingFlags]::InvokeMethod, $null, $view, $null) | Out-Null
    return $columns
}

function Test-MsiTableExists {
    param($Database, [string]$Table)
    $query = "SELECT ``Name`` FROM ``_Tables`` WHERE ``Name``='$Table'"
    $view = $Database.GetType().InvokeMember("OpenView", [System.Reflection.BindingFlags]::InvokeMethod, $null, $Database, @($query))
    $view.GetType().InvokeMember("Execute", [System.Reflection.BindingFlags]::InvokeMethod, $null, $view, $null) | Out-Null
    $record = $view.GetType().InvokeMember("Fetch", [System.Reflection.BindingFlags]::InvokeMethod, $null, $view, $null)
    $view.GetType().InvokeMember("Close", [System.Reflection.BindingFlags]::InvokeMethod, $null, $view, $null) | Out-Null
    return ($null -ne $record)
}

function Get-MsiTableRows {
    param($Database, [string]$Table, $Columns)
    $rows = New-Object System.Collections.Generic.List[string]
    $query = "SELECT * FROM ``$Table``"
    $view = $Database.GetType().InvokeMember("OpenView", [System.Reflection.BindingFlags]::InvokeMethod, $null, $Database, @($query))
    $view.GetType().InvokeMember("Execute", [System.Reflection.BindingFlags]::InvokeMethod, $null, $view, $null) | Out-Null
    while ($true) {
        $record = $view.GetType().InvokeMember("Fetch", [System.Reflection.BindingFlags]::InvokeMethod, $null, $view, $null)
        if ($null -eq $record) { break }
        $cells = New-Object System.Collections.Generic.List[string]
        for ($i = 0; $i -lt $Columns.Count; $i++) {
            $colName = $Columns[$i]
            $value = ""
            try {
                $value = [string]$record.GetType().InvokeMember("StringData", [System.Reflection.BindingFlags]::GetProperty, $null, $record, @($i + 1))
            } catch {
                $value = ""
            }
            # Collapse embedded newlines/tabs so each row stays one line and
            # the pipe-delimited format is unambiguous.
            $value = $value -replace "`r`n", " " -replace "`n", " " -replace "`t", " "
            $cells.Add("$colName=$value")
        }
        # Columns are emitted in table-definition order, which is stable, so
        # we do not re-sort columns; we DO sort rows below for determinism.
        $rows.Add(($cells -join "|"))
    }
    $view.GetType().InvokeMember("Close", [System.Reflection.BindingFlags]::InvokeMethod, $null, $view, $null) | Out-Null
    $rows.Sort([System.StringComparer]::Ordinal)
    return $rows
}

$resolvedMsi = (Resolve-Path -LiteralPath $MsiPath -ErrorAction Stop).ProviderPath

$installer = $null
$database = $null
$output = New-Object System.Text.StringBuilder
try {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    # msiOpenDatabaseModeReadOnly = 0
    $database = $installer.GetType().InvokeMember("OpenDatabase", [System.Reflection.BindingFlags]::InvokeMethod, $null, $installer, @($resolvedMsi, 0))

    [void]$output.AppendLine("# MSI table dump (normalized). Generated by scripts/dump_msi_tables.ps1")
    [void]$output.AppendLine("# Source: $(Split-Path -Leaf $resolvedMsi)")
    [void]$output.AppendLine("# Volatile fields are emitted verbatim; the differ applies the allowlist.")

    foreach ($table in $TablesToDump) {
        [void]$output.AppendLine("")
        [void]$output.AppendLine("## TABLE: $table")
        if (-not (Test-MsiTableExists -Database $database -Table $table)) {
            [void]$output.AppendLine("# (table absent)")
            continue
        }
        $columns = Get-MsiTableColumns -Database $database -Table $table
        $rows = Get-MsiTableRows -Database $database -Table $table -Columns $columns
        foreach ($row in $rows) {
            [void]$output.AppendLine($row)
        }
    }
} finally {
    foreach ($com in @($database, $installer)) {
        if ($null -ne $com) {
            try { [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($com) } catch {}
        }
    }
}

if ([string]::IsNullOrWhiteSpace($OutFile)) {
    Write-Output $output.ToString()
} else {
    # UTF-8 without BOM, LF line endings, so the committed golden is stable
    # across the differ's text comparison regardless of host defaults.
    $text = $output.ToString() -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText((Resolve-Path -LiteralPath (Split-Path -Parent $OutFile) -ErrorAction SilentlyContinue).ProviderPath + [System.IO.Path]::DirectorySeparatorChar + (Split-Path -Leaf $OutFile), $text, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "[dump_msi_tables] wrote $OutFile"
}
