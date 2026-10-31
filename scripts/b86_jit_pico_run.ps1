[CmdletBinding()]
param(
    [Parameter(Position=0)]
    [ValidateSet('doctor','build','flash','capture','run')]
    [string]$Action='doctor',
    [string]$MicroDos='C:\microDOS',
    [string]$Blitz86='C:\blitz86_v2',
    [string]$Port='',
    [int]$Seconds=90,
    [int]$PortWaitSeconds=40,
    [switch]$NoFlash
)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$build=Join-Path $MicroDos 'build-pico\out'
$uf2=Join-Path $build 'blitz86_pico_jit.uf2'
$logs=Join-Path $Blitz86 'logs'
New-Item -ItemType Directory -Path $logs -Force | Out-Null
$log=Join-Path $logs ('b86-hw-{0}-{1}.txt' -f $Action,(Get-Date -Format 'yyyyMMdd-HHmmss'))
$latest=Join-Path $logs 'latest.txt'
$script:bad=$false
function Log([string]$message) {
    Write-Host $message
    Add-Content -LiteralPath $log -Value $message
}
function Cmd([string]$label,[string]$exe,[string[]]$argv) {
    Log "BEGIN $label"
    Log ('COMMAND: '+$exe+' '+($argv -join ' '))
    $global:LASTEXITCODE=0
    & $exe @argv 2>&1 | ForEach-Object {
        $item=[string]$_
        Log $item
    }
    $rc=$global:LASTEXITCODE
    if($rc -ne 0){throw "$label exit=$rc"}
    Log "PASS $label"
}
function GetPicotool {
    $paths=@(
      (Join-Path $HOME '.pico-sdk\picotool\2.3.0\picotool\picotool.exe'),
      (Join-Path $HOME '.pico-sdk\picotool\2.1.1\picotool\picotool.exe')
    )
    $paths+=@(Get-ChildItem -Path (Join-Path $HOME '.pico-sdk\picotool') -Filter picotool.exe -File -Recurse -ErrorAction SilentlyContinue | ForEach-Object {$_.FullName})
    foreach($p in $paths){if($p -and (Test-Path $p)){return $p}}
    $cmd=Get-Command picotool.exe -ErrorAction SilentlyContinue
    if($cmd){return $cmd.Source}
    throw 'picotool.exe not found in microDOS Pico SDK environment'
}
function GetPorts {
    # The Pico Debug Probe is VID_2E8A PID_000C (COM4 in this setup).
    # Blitz86 firmware is a Pico SDK USB CDC application, PID_0009/000A.
    $present=@([System.IO.Ports.SerialPort]::GetPortNames())
    $list=@()
    try {
        $all=@(Get-CimInstance Win32_PnPEntity -ErrorAction Stop | Where-Object {
            $_.Name -match '\(COM[0-9]+\)' -and
            $_.PNPDeviceID -match 'VID_2E8A&PID_(0009|000A)'
        })
        foreach($d in $all){
            if($d.Name -match '\(COM[0-9]+\)') {
                $name=$Matches[0].Trim('(',')')
                if($present -contains $name) {
                    $priority=if($d.PNPDeviceID -match 'PID_0009'){0}else{1}
                    $list+= [pscustomobject]@{Port=$name;Name=$d.Name;PNP=$d.PNPDeviceID;Priority=$priority}
                }
            }
        }
    } catch {Log "PnP query warning: $_"}
    return @($list | Sort-Object Priority,Port -Unique)
}
function ResolvePort {
    $until=(Get-Date).AddSeconds($PortWaitSeconds)
    $last=''
    do {
        $all=@([System.IO.Ports.SerialPort]::GetPortNames())
        $devices=@(GetPorts)
        $names=@($devices | ForEach-Object {$_.Port})
        $snapshot=($all -join ',')+' / Pico app CDC: '+($names -join ',')
        if($snapshot -ne $last){Log "Serial scan: $snapshot";$last=$snapshot}
        if($Port) {
            if(($all -contains $Port) -and ($names -contains $Port)){return $Port}
            if(($all -contains $Port) -and !($names -contains $Port)) {
                Log "Requested $Port is not identified as Pico application CDC; refusing to open a possible debug probe"
            }
        } elseif($devices.Count -gt 0) {
            return $devices[0].Port
        }
        Start-Sleep -Milliseconds 350
    }while((Get-Date) -lt $until)
    if($Port){throw "Pico application CDC $Port did not appear within $PortWaitSeconds seconds; no fallback to a debug probe"}
    throw "No Pico application CDC (VID_2E8A PID_0009/000A) appeared in $PortWaitSeconds seconds"
}
function Capture {
    $chosen=ResolvePort
    Log "Capturing $chosen (timeout ${Seconds}s, DTR enabled)"
    $serial=New-Object System.IO.Ports.SerialPort($chosen,115200,'None',8,'One')
    $serial.DtrEnable=$true
    $serial.RtsEnable=$true
    $serial.ReadTimeout=200
    $serial.NewLine="`n"
    $complete=$false
    $pass=$false
    $buf=''
    try {
        $serial.Open()
        Start-Sleep -Milliseconds 450
        # Firmware waits for DTR then prints its first result; R repeats it
        # if the initial output arrived before the capture loop started.
        $serial.Write('R')
        $deadline=(Get-Date).AddSeconds($Seconds)
        while((Get-Date) -lt $deadline -and -not $complete){
            try { $buf += $serial.ReadExisting() } catch [System.TimeoutException] {}
            while($buf.IndexOf("`n") -ge 0) {
                $pos=$buf.IndexOf("`n")
                $line=$buf.Substring(0,$pos).TrimEnd("`r")
                $buf=$buf.Substring($pos+1)
                Log $line
                if($line -match '\[b86-jit\] COMPLETE result=(PASS|FAIL)') {
                    $pass=$Matches[1] -eq 'PASS'
                    $complete=$true
                }
            }
            Start-Sleep -Milliseconds 30
        }
    } finally {
        if($serial.IsOpen){$serial.Close()}
        $serial.Dispose()
    }
    if(-not $complete){throw "Serial opened, but no [b86-jit] COMPLETE marker arrived in $Seconds seconds"}
    if(-not $pass){throw 'Pico reported FAIL'}
    Log '[b86-jit] Native Thumb-2 probe verified PASS'
}
function Flash {
    if(-not(Test-Path $uf2)){throw "UF2 missing: $uf2. Run build."}
    $tool=GetPicotool
    # Reboot into BOOTSEL, then program. No COM port assumption.
    Cmd 'Request Pico BOOTSEL' $tool @('reboot','-f','-u')
    Start-Sleep -Milliseconds 800
    Cmd 'Flash blitz86 test firmware' $tool @('load','-v','-x',$uf2)
}
try {
    Log "blitz86 microDOS hardware workflow: $Action"
    switch($Action) {
        doctor {
            foreach($path in @((Join-Path $MicroDos 'pico\blitz86_target.cmake'),
                              (Join-Path $MicroDos 'pico\blitz86_pico_probe.c'),
                              (Join-Path $Blitz86 'src\be_t2.c'),
                              (Join-Path $build 'CMakeCache.txt'),$uf2)) {
                Log "Path: $path = $(Test-Path $path)"
            }
            foreach($d in @(GetPorts)){Log "Pico serial: $($d.Port) $($d.Name) $($d.PNP)"}
            Log ('All serial ports: '+([System.IO.Ports.SerialPort]::GetPortNames() -join ','))
        }
        build {
            if(-not(Test-Path (Join-Path $build 'CMakeCache.txt'))){throw 'Existing microDOS Pico CMake build cache missing'}
            Cmd 'Regenerate existing microDOS Pico target graph' 'cmake.exe' @('-S',(Join-Path $MicroDos 'pico'),'-B',$build,('-DBLITZ86_ROOT='+($Blitz86 -replace '\\','/')))
            Cmd 'Build blitz86 RP2350 probe' 'cmake.exe' @('--build',$build,'--target','blitz86_pico_probe','--parallel','4')
            if(-not(Test-Path $uf2)){throw "Build succeeded but UF2 missing: $uf2"}
            Log "UF2 READY: $uf2"
        }
        flash { Flash }
        capture { Capture }
        run { if(-not $NoFlash){Flash}; Capture }
    }
    Log 'WORKFLOW PASS'
} catch {
    $script:bad=$true
    Log "WORKFLOW FAIL: $_"
} finally {
    Copy-Item -LiteralPath $log -Destination $latest -Force
    Write-Host "Log: $log"
    Write-Host "Latest: $latest"
}
if($script:bad){exit 1}else{exit 0}
