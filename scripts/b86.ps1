# blitz86 command dispatcher. Started by b86.bat; Windows PowerShell 5.1 compatible.
# Settings: b86.config.psd1 (a variable already set in the window wins).
$ErrorActionPreference = 'Stop'
$Root    = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Scripts = $PSScriptRoot
$DevActions = @('doctor','deps','clean','build','quick','test','bench','all','capture','flash','microdos','compare','arm-build')

function Use-B86Config {
    $file = Join-Path $Root 'b86.config.psd1'
    if (-not (Test-Path -LiteralPath $file)) { throw "Missing settings file: $file" }
    $cfg = Import-PowerShellDataFile -LiteralPath $file
    $rows = @()
    foreach ($name in ($cfg.Keys | Sort-Object)) {
        $current = [Environment]::GetEnvironmentVariable($name, 'Process')
        if ($null -eq $current) {
            [Environment]::SetEnvironmentVariable($name, [string]$cfg[$name], 'Process')
            $rows += [pscustomobject]@{ Name = $name; Value = [string]$cfg[$name]; Source = 'b86.config.psd1' }
        } else {
            $src = if ($current -eq [string]$cfg[$name]) { 'window (same as config)' } else { 'WINDOW OVERRIDE' }
            $rows += [pscustomobject]@{ Name = $name; Value = $current; Source = $src }
        }
    }
    return $rows
}

function Split-B86Args([object[]]$list) {
    $named = @{}; $pos = @(); $i = 0
    while ($i -lt $list.Count) {
        $a = [string]$list[$i]
        if ($a -match '^-([A-Za-z][A-Za-z0-9]*)$') {
            $n = $Matches[1]
            if ($i + 1 -lt $list.Count -and -not ([string]$list[$i + 1] -match '^-[A-Za-z]')) {
                $named[$n] = $list[$i + 1]; $i += 2
            } else { $named[$n] = $true; $i += 1 }
        } else { $pos += $a; $i += 1 }
    }
    return @{ Named = $named; Positional = $pos }
}

function Add-Default([hashtable]$named, [string]$key, [string]$value) {
    if (-not $named.ContainsKey($key) -and -not [string]::IsNullOrEmpty($value)) { $named[$key] = $value }
}

function Invoke-B86Script([string]$name, [hashtable]$named, [object[]]$pos) {
    $path = Join-Path $Scripts $name
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing script: $path" }
    if ($null -eq $pos) { $pos = @() }
    $global:LASTEXITCODE = 0
    & $path @named @pos | Out-Host
    return [int]$global:LASTEXITCODE
}

function Show-Help {
@'
blitz86

Host development (Windows: MSVC + WSL; same as the old b86.bat actions):
  b86 doctor | deps | build | quick | test | bench | all | clean | arm-build
  b86 capture [-Port COMx] [-Baud N] [-Seconds N]     serial capture
  b86 flash -Uf2 FILE [-BootDrive E:]                 copy a UF2 to a Pico in BOOTSEL
  b86 microdos | compare                              microDOS md.bat tests / side by side
  (options: -FuzzCount N, -NoClean, -Seconds N, ...)

Standalone blitz86 probe on hardware (microDOS sidecar target):
  b86 sidecar                                 install the probe target into microDOS
  b86 hw       doctor|build|flash|capture|run [-Port COMx] [-NoFlash]
  b86 jit-pico doctor|build|flash|capture|run [-Port COMx] [-NoFlash]
  b86 hardware doctor|pico|pi|all|pico-capture|pi-capture
  b86 jit-hardware pico|pi|all

  b86 config        effective settings and where each comes from
  b86 help

Linux/WSL test suite: make quick | make test | make bench  (see README.md)
'@ | Write-Host
}

$argv    = @($args)
$command = if ($argv.Count) { ([string]$argv[0]).ToLowerInvariant() } else { 'help' }
$rest    = @(if ($argv.Count -gt 1) { $argv[1..($argv.Count - 1)] })
$parsed  = Split-B86Args $rest
$named   = $parsed.Named
$pos     = $parsed.Positional
$rc = 0

if ($DevActions -contains $command) {
    [void](Use-B86Config)
    $named['Action'] = $command
    Add-Default $named 'Repo'     $Root
    Add-Default $named 'MicroDos' $env:B86_MICRODOS
    Add-Default $named 'Port'     $env:B86_PORT
    $rc = Invoke-B86Script 'b86_dev.ps1' $named $pos
} else {
    switch ($command) {
        { $_ -in @('hw', 'jit-pico', 'hardware') } {
            [void](Use-B86Config)
            $script = @{ 'hw' = 'b86_hw.ps1'; 'jit-pico' = 'b86_jit_pico_run.ps1'; 'hardware' = 'b86_hardware.ps1' }[$command]
            if ($pos.Count -ge 1) { $named['Action'] = $pos[0]; $pos = @(if ($pos.Count -gt 1) { $pos[1..($pos.Count - 1)] }) }
            Add-Default $named 'Blitz86'  $Root
            Add-Default $named 'MicroDos' $env:B86_MICRODOS
            Add-Default $named 'Port'     $env:B86_PORT
            $rc = Invoke-B86Script $script $named $pos
        }
        'jit-hardware' {
            [void](Use-B86Config)
            if ($pos.Count -ge 1) { $named['Platform'] = $pos[0]; $pos = @(if ($pos.Count -gt 1) { $pos[1..($pos.Count - 1)] }) }
            Add-Default $named 'Blitz86'  $Root
            Add-Default $named 'MicroDos' $env:B86_MICRODOS
            Add-Default $named 'Port'     $env:B86_PORT
            $rc = Invoke-B86Script 'b86_jit_hardware.ps1' $named $pos
        }
        'sidecar' {
            [void](Use-B86Config)
            Add-Default $named 'Blitz86'  $Root
            Add-Default $named 'MicroDos' $env:B86_MICRODOS
            $rc = Invoke-B86Script 'install_blitz86_sidecar.ps1' $named $pos
        }
        'config' {
            $rows = Use-B86Config
            Write-Host "[b86] settings file: $(Join-Path $Root 'b86.config.psd1')"
            foreach ($r in $rows) { Write-Host ('       {0,-13} = {1,-14} {2}' -f $r.Name, $(if ($r.Value) { $r.Value } else { "''" }), $r.Source) }
        }
        { $_ -in @('help', '-h', '--help', '/?') } { Show-Help }
        default { Write-Host "Unknown command: $command"; Show-Help; $rc = 2 }
    }
}
exit $rc
