[CmdletBinding()]
param([string]$Blitz86='C:\blitz86_v2',[string]$Port='COM5',[switch]$AnalyzeOnly)
$ErrorActionPreference='Stop'
$auto=Join-Path $PSScriptRoot 'auto_compare.ps1'
$logDir=Join-Path $Blitz86 'logs\compare'
if(!$AnalyzeOnly){
 if(!(Test-Path $auto)){throw "Existing v3 auto-runner missing: $auto"}
 & $auto all -Port $Port
 if($LASTEXITCODE -ne 0){throw 'Four-case hardware workflow failed; inspect console output.'}
}
$csv=Get-ChildItem -LiteralPath $logDir -Filter 'comparison-*.csv' -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if(!$csv){throw 'No comparison CSV found.'}
$stamp=$csv.BaseName.Substring('comparison-'.Length)
$cases=@('blitz-pico','microdos-pico','blitz-pi','microdos-pi')
$records=@{}
foreach($case in $cases){
 $file=Join-Path $logDir "$stamp-$case.txt"
 if(!(Test-Path -LiteralPath $file)){throw "Missing audit log $file"}
 foreach($line in (Get-Content -LiteralPath $file)){
  if($line -notmatch '^\[(bench|compare|audit)\]'){continue}
  $v=@{}
  foreach($m in [regex]::Matches($line,'([A-Za-z_][A-Za-z_0-9-]*)=([^\s]+)')){$v[$m.Groups[1].Value]=$m.Groups[2].Value}
  if($v['phase'] -eq 'warm-median-7' -and $v['result'] -eq 'PASS'){
   $records["$case/$($v['workload'])"]=$v
  }
  if($line -match '^\[audit\].*test=loop-scale.*count='){
   if($v['result'] -ne 'PASS' -or [uint64]$v['retired_expected'] -ne [uint64]$v['retired_observed'] -or [uint64]$v['ticks'] -eq 0){throw "Scaling check failed: $line"}
   Write-Host $line
  }
 }
}
function Get-Hash($raw,$case){
 if(!$raw){throw "norm64 missing for $case"}
 if($case -eq 'blitz-pi'){return [uint32][uint64]::Parse($raw,[Globalization.NumberStyles]::Integer)}
 return [uint32]::Parse($raw,[Globalization.NumberStyles]::HexNumber)
}
$out=@()
$allOK=$true
foreach($arch in @('pico','pi')){
 foreach($work in @('loop','regmix','callmix')){
  $b=$records["blitz-$arch/$work"]
  $m=$records["microdos-$arch/$work"]
  if(!$b -or !$m){throw "Missing warm benchmark: $arch $work"}
  $bh=Get-Hash $b['norm64'] "blitz-$arch"
  $mh=Get-Hash $m['norm64'] "microdos-$arch"
  $match=($bh -eq $mh)
  if(!$match){$allOK=$false}
  $retB=[uint64]$b['retired_ref'];$retM=[uint64]$m['retired']
  if($retB -ne $retM){throw "Retired mismatch $arch/$work : $retB versus $retM"}
  $bt=[uint64]$b['us'];$mt=[uint64]$m['us']
  if(!$bt -or !$mt){throw "Zero-microsecond sample: $arch/$work"}
  $ratio=[math]::Round(([double]$bt/$mt),3)
  $note=if($arch -eq 'pi' -and $work -eq 'loop' -and $mt -lt 10){'OPTIMIZED_EQUIVALENT_WORK_NOT_LITERAL_MIPS'}else{''}
  $out+=[pscustomobject]@{arch=$arch;workload=$work;retired=$retB;blitz_us=$bt;microdos_us=$mt;microdos_speedup=$ratio;norm64_match=$match;blitz_norm64=$bh;microdos_norm64=$mh;note=$note}
 }
}
$outFile=Join-Path $logDir "audit-$stamp.csv"
$out|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath $outFile
$out|Format-Table -AutoSize | Out-Host
Write-Host "AUDIT CSV: $outFile"
if(!$allOK){throw 'Normalized guest memory hashes differ; investigate initialization or guest memory state.'}
Write-Host 'MEMORY HASH AUDIT PASS (same first 65536 physical guest bytes)'
# Architectural state fingerprint uses defined FLAGS mask 0x08D7 and 14 uint16 values.
# Both Pi outputs report decimal; both Pico outputs report hex.
$states=@()
$stateOK=$true
foreach($arch in @('pico','pi')){
 foreach($work in @('loop','regmix','callmix')){
  $b=$records["blitz-$arch/$work"]; $m=$records["microdos-$arch/$work"]
  if(!$b['state32'] -or !$m['state32']){throw "Missing state32: $arch/$work"}
  if($arch -eq 'pi'){
   $bv=[uint32][uint64]::Parse($b['state32']);$mv=[uint32][uint64]::Parse($m['state32'])
  }else{
   $bv=[uint32]::Parse($b['state32'],[Globalization.NumberStyles]::HexNumber)
   $mv=[uint32]::Parse($m['state32'],[Globalization.NumberStyles]::HexNumber)
  }
  $match=$bv -eq $mv
  if(!$match){$stateOK=$false}
  $states += [pscustomobject]@{arch=$arch;workload=$work;state_match=$match;blitz_state=('{0:X8}' -f $bv);microdos_state=('{0:X8}' -f $mv)}
 }
}
$stateCSV=Join-Path $logDir "states-$stamp.csv"
$states|Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath $stateCSV
$states|Format-Table -AutoSize | Out-Host
Write-Host "STATE CSV: $stateCSV"
if(!$stateOK){throw 'Cross-engine architectural-state mismatch: inspect states CSV; both engines still passed within-engine checks.'}
Write-Host 'CROSS-ENGINE ARCHITECTURAL STATE AUDIT PASS (GPR, segment, IP, FLAGS defined mask 08D7)'

