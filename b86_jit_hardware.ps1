[CmdletBinding()]
param([Parameter(Position=0)][ValidateSet('pico','pi','all')][string]$Platform='pico',
 [string]$MicroDos='C:\microDOS',[string]$Blitz86='C:\blitz86_v2',
 [string]$Port='COM5',[int]$CaptureSeconds=90,[switch]$NoBuild)
$ErrorActionPreference='Stop'
$logDir=Join-Path $Blitz86 'logs';New-Item -Force -ItemType Directory $logDir | Out-Null
$log=Join-Path $logDir ('b86-jit-'+$Platform+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'.txt')
function Run([string]$what,[string]$cmd,[string[]]$a){
 Write-Host "BEGIN $what"; & $cmd @a; if($LASTEXITCODE -ne 0){throw "$what exit=$LASTEXITCODE"};Write-Host "PASS $what"
}
function Pico {
 $build="$MicroDos\build-pico\out"
 if(!$NoBuild){Run 'Pico configure' cmake.exe @('-S',"$MicroDos\pico",'-B',$build,('-DBLITZ86_ROOT='+$Blitz86.Replace([char]92,[char]47)));Run 'Pico JIT build' cmake.exe @('--build',$build,'--target','blitz86_pico_jit','--parallel','4')}
 if(!(Test-Path "$build\blitz86_pico_jit.uf2")){throw 'Pico JIT UF2 missing'}
 & "$Blitz86\b86_jit_pico_run.ps1" run -MicroDos $MicroDos -Blitz86 $Blitz86 -Port $Port -Seconds $CaptureSeconds
 if($LASTEXITCODE -ne 0){throw 'Pico native JIT firmware did not pass'}
}
function Pi {
 $build="$MicroDos\build-pi0w\out"
 if(!$NoBuild){$env:PATH='C:\Program Files\Arm\GNU Toolchain mingw-w64-x86_64-aarch64-none-elf\bin;'+$env:PATH;Run 'Pi configure' cmake.exe @('-S',"$MicroDos\pi0w",'-B',$build,('-DBLITZ86_ROOT='+$Blitz86.Replace([char]92,[char]47)));Run 'Pi JIT build' cmake.exe @('--build',$build,'--target','blitz86_pi_jit.elf','--parallel','4')}
 $img="$build\kernel8_blitz86_jit.img";if(!(Test-Path $img)){throw "Missing $img"}
 $script:pass=$false;$script:completed=$false
 & "$MicroDos\md_pi0w_run.ps1" -Repo $MicroDos -KernelImage $img -NoBuild -CaptureSeconds $CaptureSeconds 2>&1 | ForEach-Object {
  $line=[string]$_;Write-Host $line;Add-Content $log $line
  if($line.Contains('[b86-pi-jit] COMPLETE result=PASS')){$script:pass=$true}
  if($line.Contains('=== RUN COMPLETE ===')){$script:completed=$true}
 }
 if(!$script:pass -or !$script:completed){throw "Pi JIT missing PASS or successful transport: $log"}
}
try {switch($Platform){'pico'{Pico};'pi'{Pi};'all'{Pico;Pi}};Write-Host 'NATIVE JIT WORKFLOW PASS'}catch{Write-Host "NATIVE JIT WORKFLOW FAIL: $_";exit 1}
