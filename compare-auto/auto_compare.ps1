[CmdletBinding()]
param(
 [Parameter(Position=0)][ValidateSet('pico','pi','all','blitz-pico','blitz-pi','microdos-pico','microdos-pi')][string]$Mode='all',
 [string]$MicroDos='C:\microDOS', [string]$Blitz86='C:\blitz86_v2',
 [string]$Port='COM5', [int]$CaptureSeconds=90,
 [switch]$NoBuild, [switch]$NoFlash, [switch]$ContinueOnFailure
)
$ErrorActionPreference='Stop'
$MicroDos=[IO.Path]::GetFullPath($MicroDos);$Blitz86=[IO.Path]::GetFullPath($Blitz86)
$logDir=Join-Path $Blitz86 'logs\compare';New-Item -ItemType Directory -Force $logDir|Out-Null
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss';$summary=Join-Path $logDir "comparison-$stamp.csv"
$records=New-Object 'System.Collections.Generic.List[object]'
$cases=@()
switch($Mode){
 'all' {$cases=@('blitz-pico','microdos-pico','blitz-pi','microdos-pi')}
 'pico' {$cases=@('blitz-pico','microdos-pico')}
 'pi' {$cases=@('blitz-pi','microdos-pi')}
 default {$cases=@($Mode)}
}
function Invoke-Cmd([string]$Label,[string]$Command,[string[]]$Arguments){
 Write-Host "=== $Label ==="
 # Native command stdout must never flow into the function return value.
 # Get-Build is invoked in an assignment and must return ONLY the image path.
 & $Command @Arguments | Out-Host
 if($LASTEXITCODE -ne 0){throw "$Label failed: exit=$LASTEXITCODE"}
}
function Get-Build([string]$Which){
 if($Which.EndsWith('pico')){$dir=Join-Path $MicroDos 'build-pico\out';$src=Join-Path $MicroDos 'pico'}
 else{$dir=Join-Path $MicroDos 'build-pi0w\out';$src=Join-Path $MicroDos 'pi0w'}
 $target=switch($Which){
 'blitz-pico' {'blitz86_pico_jit'}
 'blitz-pi' {'blitz86_pi_jit.elf'}
 'microdos-pico' {'microdos_pico_native3_bench'}
 'microdos-pi' {'microdos_pi0w_native3_bench.elf'}
 }
 $image=switch($Which){
 'blitz-pico' {'blitz86_pico_jit.uf2'}
 'blitz-pi' {'kernel8_blitz86_jit.img'}
 'microdos-pico' {'microdos_pico_native3_bench.uf2'}
 'microdos-pi' {'kernel8_native3_bench.img'}
 }
 if(!$NoBuild){
   if($Which.EndsWith('pi')){$env:PATH='C:\Program Files\Arm\GNU Toolchain mingw-w64-x86_64-aarch64-none-elf\bin;'+$env:PATH}
   Invoke-Cmd "$Which configure" 'cmake.exe' @('-S',$src,'-B',$dir,('-DBLITZ86_ROOT='+$Blitz86.Replace('\','/')))
   Invoke-Cmd "$Which build" 'cmake.exe' @('--build',$dir,'--target',$target,'--parallel','4')
 }
 $path=Join-Path $dir $image
 if(!(Test-Path -LiteralPath $path -PathType Leaf)){throw "Firmware missing: $path"}
 return [string]$path
}
function Capture-Pico([string]$Which,[string]$Firmware,[string]$Log){
 $runner=Join-Path $PSScriptRoot 'pico_flash_capture.ps1'
 & $runner -Firmware $Firmware -Port $Port -Log $Log -Seconds $CaptureSeconds -NoFlash:$NoFlash
 if($LASTEXITCODE -ne 0){throw "$Which Pico capture returned failure"}
}
function Capture-Pi([string]$Which,[string]$Firmware,[string]$Log){
 if($NoFlash){throw 'Pi no-flash mode is unsupported: a Pi image needs staging/boot'}
 $runner=Join-Path $MicroDos 'md_pi0w_run.ps1'
 if(!(Test-Path $runner)){throw "Pi runner missing: $runner"}
 $script:piTransport=$false
 $script:piDone=$false
 $old=$ErrorActionPreference;$ErrorActionPreference='Continue'
 try{
  & $runner -Repo $MicroDos -KernelImage $Firmware -NoBuild -CaptureSeconds $CaptureSeconds 2>&1 | ForEach-Object {
    $line=[string]$_;Write-Host $line;Add-Content -LiteralPath $Log -Value $line
    if($line.Contains('=== RUN COMPLETE ===')){$script:piDone=$true}
  }
  $rc=$LASTEXITCODE
 }finally{$ErrorActionPreference=$old}
 if($rc -ne 0 -or !$script:piDone){throw "$Which Pi HID/rpiboot transport incomplete or exit=$rc"}
}
function Validate-Case([string]$Which,[string]$Log){
 $content=Get-Content -LiteralPath $Log -Raw
 if($Which.StartsWith('blitz')){
   $pattern=if($Which.EndsWith('pico')){'\[b86-jit\] COMPLETE result=PASS'}else{'\[b86-pi-jit\] COMPLETE result=PASS'}
   if($content -notmatch $pattern){throw "Blitz86 missing overall PASS marker: $Log"}
   foreach($w in @('loop','regmix','callmix')){
     $p='(?m)^\[bench\].*workload='+$w+'\s+engine=blitz86-jit\s+phase=warm-median-7\s+result=PASS'
     if($content -notmatch $p){throw "Blitz86 missing warm PASS for $w : $Log"}
   }
 }else{
   if($content -notmatch '\[n3-compare\] COMPLETE result=PASS'){
     throw "microDOS missing overall COMPLETE PASS: $Log"
   }
   if($content -match '\[compare\].*result=FAIL'){
     throw "microDOS emitted a FAIL result: $Log"
   }
   foreach($w in @('loop','regmix','callmix')){
     $p='(?m)^\[compare\].*workload='+$w+'\s+phase=warm-median-7\s+result=PASS'
     if($content -notmatch $p){throw "microDOS missing warm median PASS for $w : $Log"}
   }

 }
}
function Export-Rows([string]$Which,[string]$Log){
 foreach($line in (Get-Content -LiteralPath $Log)){
   if($line -notmatch '^\[(bench|compare)\]') {continue}
   $values=@{};foreach($pair in ([regex]::Matches($line,'([A-Za-z_][A-Za-z_0-9-]*)=([^\s]+)'))){$values[$pair.Groups[1].Value]=$pair.Groups[2].Value}
   $records.Add([pscustomobject]@{case=$Which;arch=$values['arch'];engine=$values['engine'];workload=$values['workload'];phase=$values['phase'];result=$values['result'];us=$values['us'];retired=$values['retired'];retired_ref=$values['retired_ref'];native=$values['native'];interp=$values['interp'];nv2=$values['nv2'];source_log=$Log})
 }
}
$failures=0
foreach($which in $cases){
 $log=Join-Path $logDir "$stamp-$which.txt"
 try{
  Write-Host "`n========== $which ==========" -ForegroundColor Cyan
  $firmware=[string](Get-Build $which)
  if($firmware -match "[\r\n]" -or !(Test-Path -LiteralPath $firmware -PathType Leaf)){ throw "Build returned invalid firmware path: $firmware" }
  Write-Host "FIRMWARE: $firmware"
  "CASE=$which`nFIRMWARE=$firmware" | Set-Content -LiteralPath $log
  if($which.EndsWith('pico')){Capture-Pico $which $firmware $log}else{Capture-Pi $which $firmware $log}
  Validate-Case $which $log
  Export-Rows $which $log
  Write-Host "PASS $which" -ForegroundColor Green
 }catch{
  $failures++;Write-Host "FAIL $which : $_" -ForegroundColor Red
  Add-Content -LiteralPath $log -Value "AUTOMATION_FAIL: $_"
  if(!$ContinueOnFailure){break}
 }
}
if($records.Count){$records|Export-Csv -Path $summary -NoTypeInformation -Encoding UTF8}
Write-Host "`nCSV: $summary"
Write-Host "Logs: $logDir"
if($failures){Write-Host "AUTOMATED COMPARISON FAIL ($failures case(s))";exit 1}
Write-Host 'AUTOMATED COMPARISON PASS'
