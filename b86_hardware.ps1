[CmdletBinding()]
param(
 [Parameter(Position=0)][ValidateSet('doctor','pico','pi','all','pico-capture','pi-capture')][string]$Action='doctor',
 [string]$MicroDos='C:\microDOS',
 [string]$Blitz86='C:\blitz86_v2',
 [int]$CaptureSeconds=90,
 [string]$Port='',
 [switch]$NoFlash,
 [switch]$NoBuild
)
$ErrorActionPreference='Stop'
$MicroDos=[IO.Path]::GetFullPath($MicroDos)
$Blitz86=[IO.Path]::GetFullPath($Blitz86)
$logdir=Join-Path $Blitz86 'logs'
New-Item -ItemType Directory -Force -Path $logdir | Out-Null
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$log=Join-Path $logdir "b86-unified-$Action-$stamp.txt"
$script:raw=New-Object System.Collections.Generic.List[string]
function Note([string]$s){Write-Host $s;[void]$script:raw.Add($s)}
function Exec([string]$label,[string]$binary,[string[]]$arglist){
 Note "BEGIN $label"
 Note ("COMMAND: "+$binary+' '+($arglist -join ' '))
 # Preserve native output and exit code without the automatic-variable `$args` bug.
 $old=$ErrorActionPreference;$ErrorActionPreference='Continue'
 try{$output=& $binary @arglist 2>&1; $rc=$LASTEXITCODE}
 finally{$ErrorActionPreference=$old}
 foreach($line in $output){Note ([string]$line)}
 if($rc -ne 0){throw "$label failed with exit code $rc"}
 Note "PASS $label"
}
function Need([string]$path){if(!(Test-Path $path)){throw "Required file missing: $path"}}
function Verify-Target([string]$BuildDir,[string]$Target) {
 $cache=Join-Path $BuildDir 'CMakeCache.txt'
 Need $cache
 $content=[IO.File]::ReadAllText($cache)
 $normalized=$Blitz86.Replace([char]92,[char]47)
 if($content -notmatch '(?m)^BLITZ86_ROOT:[^=]*=') {
    throw "CMake did not cache BLITZ86_ROOT. Check configure arguments."
 }
 $configured=([regex]::Match($content,'(?m)^BLITZ86_ROOT:[^=]*=(.*)$')).Groups[1].Value.Trim()
 Note "BLITZ86_ROOT cached: $configured"
 if($configured.TrimEnd('/') -ine $normalized.TrimEnd('/')) {
    throw "BLITZ86_ROOT mismatch. Expected $normalized but got $configured"
 }
 $ninja=Join-Path $BuildDir 'build.ninja'
 if(Test-Path $ninja) {
    $graph=[IO.File]::ReadAllText($ninja)
    if($graph -notmatch [regex]::Escape($Target)) {
       throw "Target $Target not registered in Ninja graph; examine sidecar CMake messages"
    }
 }
 Note "PASS target registered: $Target"
}
function Pico {
 Need "$MicroDos\pico\blitz86_target.cmake"
 Need "$MicroDos\pico\blitz86_pico_probe.c"
 Need "$MicroDos\build-pico\out\CMakeCache.txt"
 Need "$Blitz86\b86_hw.ps1"
 if(!$NoBuild){
  Exec 'Pico configure' 'cmake.exe' @('-S',"$MicroDos\pico",'-B',"$MicroDos\build-pico\out",('-DBLITZ86_ROOT=' + $Blitz86.Replace([char]92,[char]47)))
  Verify-Target (Join-Path $MicroDos 'build-pico\out') 'blitz86_pico_probe'
  Exec 'Pico build' 'cmake.exe' @('--build',"$MicroDos\build-pico\out",'--target','blitz86_pico_probe','--parallel','4')
 }
 Need "$MicroDos\build-pico\out\blitz86_pico_probe.uf2"
 Note 'Pico image verified'
 $picoAction = 'run'
 if($NoFlash){ $picoAction = 'capture' }
 $params=@{Action=$picoAction;MicroDos=$MicroDos;Blitz86=$Blitz86;Seconds=$CaptureSeconds}
 if($Port){$params.Port=$Port}
 # b86_hw.ps1 is the already validated microDOS sidecar runner.
 # It handles picotool, USB re-enumeration, DTR and PASS/FAIL logging.
 $runner=Join-Path $Blitz86 'b86_hw.ps1'
 Note 'BEGIN existing Pico build/flash/capture runner'
 & $runner @params
 if($LASTEXITCODE -ne 0){throw "Pico runner exited $LASTEXITCODE"}
 $last=Join-Path $logdir 'latest.txt'
 if(!(Test-Path $last)){throw 'Pico log missing'}
 $result=Get-Content $last -Raw
 if($result -notmatch '\[b86\] COMPLETE result=PASS'){throw 'Pico completed without PASS marker'}
 Note 'PASS Pico test marker'
}
function Pi {
 Need "$MicroDos\pi0w\blitz86_pi_target.cmake"
 Need "$MicroDos\pi0w\blitz86_pi_probe.c"
 Need "$MicroDos\md_pi0w_run.ps1"
 $out=Join-Path $MicroDos 'build-pi0w\out'
 Need (Join-Path $out 'CMakeCache.txt')
 if(!$NoBuild){
  $env:PATH='C:\Program Files\Arm\GNU Toolchain mingw-w64-x86_64-aarch64-none-elf\bin;'+$env:PATH
  Exec 'Pi configure' 'cmake.exe' @('-S',"$MicroDos\pi0w",'-B',$out,('-DBLITZ86_ROOT=' + $Blitz86.Replace([char]92,[char]47)))
  Verify-Target $out 'blitz86_pi_probe.elf'
  Exec 'Pi build' 'cmake.exe' @('--build',$out,'--target','blitz86_pi_probe.elf','--parallel','4')
 }
 $img=Join-Path $out 'kernel8_blitz86_probe.img'
 Need $img
 Note "Pi image verified: $img"
 # md_pi0w_run.ps1 is the known-good v29 HID/rpiboot transport: reuses
 # existing bridge, Pi RUN pin, USB boot, UART, host-side HID checks.
 Note 'BEGIN microDOS Pi v29 HID runner'
 # PRE-FLIGHT: the v29 transport is NOT a serial COM port.
 # Without this device, rpiboot can never be triggered/released.
 $py = Get-Command python.exe -ErrorAction SilentlyContinue
 if(!$py){throw 'Python is unavailable for HID preflight. microDOS Pi runner requires Python + hidapi.'}
 # Execute a real .py script instead of `python -c` (PowerShell 5.1 strips
 # nested quotes from native-command arguments on some installations).
 $preflightScript=Join-Path $Blitz86 'tools\b86_hid_preflight.py'
 Need $preflightScript
 Note 'BEGIN v29 HID bridge preflight (VID CAFE PID 4028)'
 $old=$ErrorActionPreference; $ErrorActionPreference='Continue'
 try { $preflightOutput = & $py.Source $preflightScript 2>&1; $prc=$LASTEXITCODE }
 finally { $ErrorActionPreference=$old }
 foreach($line in $preflightOutput){Note ([string]$line)}
 if($prc -eq 13){throw 'Python hidapi unavailable. Install with: python -m pip install hidapi'}
 if($prc -eq 12){throw 'No microDOS v29 HID bridge detected (VID CAFE PID 4028). Check bridge firmware and USB connection.'}
 if($prc -ne 0){throw "HID preflight failed with exit code $prc"}
 Note 'PASS v29 HID preflight'
 # Live pipeline is CRITICAL: do not buffer an entire Pi boot attempt in a variable.
 # Log each line immediately, while the child handles v29 reset, rpiboot and UART.
 $piLog = Join-Path $logdir ("b86-pi-transport-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt')
 $script:piSuccess=$false
 $script:piRunnerDone=$false
 $old=$ErrorActionPreference; $ErrorActionPreference='Continue'
 try {
   & "$MicroDos\md_pi0w_run.ps1" -Repo $MicroDos -KernelImage $img -NoBuild -CaptureSeconds $CaptureSeconds 2>&1 |
    ForEach-Object {
      $line=[string]$_
      Note $line
      Add-Content -LiteralPath $piLog -Value $line
      if($line -match '\[b86-pi\] COMPLETE result=PASS'){$script:piSuccess=$true}
      if($line -match '=== RUN COMPLETE ==='){$script:piRunnerDone=$true}
    }
   $rc=$LASTEXITCODE
 } finally { $ErrorActionPreference=$old }
 if($rc -ne 0){throw "Pi HID runner failed with exit code $rc; transport log: $piLog"}
 if(!$script:piRunnerDone){throw "Pi runner did not complete; inspect $piLog"}
 if(!$script:piSuccess){throw "Pi transport completed without blitz86 PASS marker; inspect $piLog"}
 Note 'PASS Pi test marker'
}
try{
 Note "blitz86 unified hardware runner $Action"
 switch($Action){
  doctor {
   foreach($f in @("$MicroDos\pico\blitz86_target.cmake","$MicroDos\pi0w\blitz86_pi_target.cmake","$MicroDos\md_pi0w_run.ps1","$Blitz86\b86_hw.ps1")) {Note "$f : $(Test-Path $f)"}
  }
  pico { Pico }
  pi { Pi }
  all { Pico; Pi }
  'pico-capture' {$NoBuild=$true;$NoFlash=$true;Pico}
  'pi-capture' {$NoBuild=$true;Pi}
 }
 Note 'WORKFLOW PASS'
}catch{Note "WORKFLOW FAIL: $_";exit 1}
finally{
 [IO.File]::WriteAllLines($log,$script:raw.ToArray())
 Copy-Item $log (Join-Path $logdir 'unified-latest.txt') -Force
 Write-Host "Log: $log"
}
