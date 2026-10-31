[CmdletBinding()]
param(
 [Parameter(Position=0)][ValidateSet('doctor','deps','clean','build','quick','test','bench','all','capture','flash','microdos','compare','arm-build')][string]$Action='quick',
 [string]$Repo='C:\blitz86_v2',
 [string]$MicroDos='C:\microDOS',
 [string]$Port='COM5',
 [int]$Baud=115200,
 [int]$Seconds=60,
 [string]$Uf2='',
 [string]$BootDrive='',
 [int]$FuzzCount=1000,
 [switch]$NoClean
)
$ErrorActionPreference='Stop'
$Repo=[IO.Path]::GetFullPath($Repo)
if (!(Test-Path -LiteralPath $Repo -PathType Container)) { throw "Missing repository: $Repo" }
$logDir=Join-Path $Repo 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log=Join-Path $logDir ('b86-{0}-{1}.log' -f $Action,(Get-Date -Format 'yyyyMMdd-HHmmss'))
$latest=Join-Path $logDir 'latest.txt'
$script:failed=$false
function Log([string]$s) {
 $line='[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'),$s
 Write-Host $line
 Add-Content -LiteralPath $log -Value $line
}
function Run([string]$label,[string]$program,[string[]]$argv) {
 Log "BEGIN $label"
 # PS 5.1 with ErrorActionPreference=Stop turns native stderr into
 # NativeCommandError even when the native process has not failed.
 $oldPreference=$ErrorActionPreference
 $ErrorActionPreference='Continue'
 try {
  $global:LASTEXITCODE=0
  & $program @argv 2>&1 | ForEach-Object {
   $line= [string]$_
   Write-Host $line
   Add-Content -LiteralPath $log -Value $line
  }
  $rc=$global:LASTEXITCODE
 } finally { $ErrorActionPreference=$oldPreference }
 if ($rc -ne 0) { throw "$label exited $rc; see $log" }
 Log "PASS $label"
}
function Need([string]$p) {
 if (!(Test-Path (Join-Path $Repo $p))) { throw "Missing $p" }
}
function PythonExe {
 foreach($n in @('py.exe','python.exe','python3.exe')) {
  $x=Get-Command $n -ErrorAction SilentlyContinue
  if ($x) { return $x.Source }
 }
 throw 'Python 3 is required for native vector downloads and serial capture.'
}
function NativeBuild {
 Need 'src\interp.c'
 Need 'tests\sst_interp.c'
 Need 'include\b86.h'
 Need 'tools\windows\CMakeLists.txt'
 if (!(Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
  throw 'CMake not found. Install the VS2022 CMake component (already used by microDOS).'
 }
 $buildDir=Join-Path $Repo 'build-win'
 $cache=Join-Path $buildDir 'CMakeCache.txt'
 if (Test-Path -LiteralPath $cache) {
  $cacheText=Get-Content -LiteralPath $cache -Raw
  if ($cacheText -notmatch 'CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022' -or
      $cacheText -notmatch 'CMAKE_GENERATOR_PLATFORM:INTERNAL=x64') {
   throw "Existing build-win cache has the wrong generator/architecture. Run '.\b86.bat clean' first."
  }
 }
 # Identical generator and architecture selection to microDOS's working host build.
 Run 'CMake configure VS2022 x64' 'cmake.exe' @('-S',(Join-Path $Repo 'tools\windows'),'-B',$buildDir,'-G','Visual Studio 17 2022','-A','x64')
 Run 'MSVC x64 interpreter build' 'cmake.exe' @('--build',$buildDir,'--config','Release','--target','sst_interp','--parallel','4')
}
function EnsureVectors([string]$which) {
 $target=Join-Path $Repo ('sst\' + $which + '.bin')
 if ((Test-Path -LiteralPath $target) -and (Get-Item -LiteralPath $target).Length -gt 0) { return }
 Log "Missing $target - preparing physical-8086 vectors automatically"
 Need 'tools\sst_convert.py'
 $py=PythonExe
 Run 'fetch and convert silicon vectors' $py @((Join-Path $PSScriptRoot 'fetch_sst_win.py'),$Repo)
 if (!(Test-Path -LiteralPath $target) -or (Get-Item -LiteralPath $target).Length -le 0) {
  throw "Vector generation did not create $target"
 }
}
function WindowsTest([string]$which) {
 EnsureVectors $which
 NativeBuild
 Run "Windows physical-8086 vectors ($which)" (Join-Path $Repo 'build-win\Release\sst_interp.exe') @("sst\$which.bin")
}
function ArmBuild {
 # Compile objects, not an executable: executable translator tests require real ARM runtime or emulator.
 $out=Join-Path $Repo 'build-arm-win'
 New-Item -ItemType Directory -Force -Path $out | Out-Null
 $targets=@(
  @{compiler='arm-none-eabi-gcc'; name='thumb2'; backend='src/be_t2.c'; flags=@('-mthumb','-mcpu=cortex-m33','-mfloat-abi=soft')},
  @{compiler='aarch64-none-elf-gcc'; name='a64'; backend='src/be_a64.c'; flags=@('-march=armv8-a')}
 )
 foreach($t in $targets) {
  if (!(Get-Command $t.compiler -ErrorAction SilentlyContinue)) {
   Log "SKIP $($t.name): $($t.compiler) not installed; no Linux needed"
   continue
  }
  foreach($source in @('src/interp.c','src/jit.c',$t.backend)) {
   $base=[IO.Path]::GetFileNameWithoutExtension($source)
   $obj=Join-Path $out ("{0}_{1}.o" -f $t.name,$base)
   $args=@('-O2','-Wall','-Wextra','-Iinclude') + $t.flags + @('-c',$source,'-o',$obj)
   Run "cross compile $($t.name) $source" $t.compiler $args
  }
 }
 Log 'ARM compilation checks only. No UF2 or Pi bare-metal image was produced.'
}
function Doctor {
 foreach($p in @('src\interp.c','src\jit.c','src\be_t2.c','src\be_a64.c','tests\sst_interp.c','include\b86.h','tools\windows\CMakeLists.txt','md.bat')) {
  $path= if($p -eq 'md.bat') { Join-Path $MicroDos $p } else { Join-Path $Repo $p }
  Log ("{0}: {1}" -f $path,$(if(Test-Path $path){'FOUND'}else{'missing'}))
 }
 foreach($name in @('cl.exe','py.exe','python.exe','arm-none-eabi-gcc','aarch64-none-elf-gcc','picotool.exe')) {
  $x=Get-Command $name -ErrorAction SilentlyContinue
  Log ("{0}: {1}" -f $name,$(if($x){$x.Source}else{'not on PATH'}))
 }
 Log 'Windows host build: CMake Visual Studio 17 2022 -A x64 (no vcvars/cmd quoting)'
 $cmake=Get-Command cmake.exe -ErrorAction SilentlyContinue
 Log ('cmake.exe: ' + $(if($cmake){$cmake.Source}else{'not on PATH'}))
 $uf2=Get-ChildItem -Path $Repo -Filter '*.uf2' -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 5
 if(!$uf2){Log 'No UF2 present. blitz86 core requires firmware integration before flashing.'}
 else { foreach($f in $uf2){Log "UF2: $($f.FullName)"} }
}
function Micro {
 $md=Join-Path $MicroDos 'md.bat'
 if(!(Test-Path $md)){throw "Missing $md"}
 Push-Location $MicroDos
 try { Run 'microDOS test all' 'cmd.exe' @('/d','/c','md.bat test all') }
 finally { Pop-Location }
}
function Capture {
 $p=PythonExe
 $f=Join-Path $PSScriptRoot 'capture_serial.py'
 if(!(Test-Path $f)){throw "Missing $f"}
 Run "serial capture $Port" $p @($f,'--port',$Port,'--baud',"$Baud",'--seconds',"$Seconds")
}
function Flash {
 if(!$Uf2){throw 'Specify -Uf2 file. There is no standalone blitz86 UF2 build target yet.'}
 $image=[IO.Path]::GetFullPath($Uf2)
 if(!(Test-Path -LiteralPath $image -PathType Leaf) -or !($image.EndsWith('.uf2',[StringComparison]::OrdinalIgnoreCase))){throw "Invalid UF2: $image"}
 $disks=@(Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=2' -ErrorAction SilentlyContinue | Where-Object { $_.VolumeName -in @('RP2350','RPI-RP2') })
 if(!$BootDrive) {
  if($disks.Count -ne 1){throw 'Enter BOOTSEL and specify -BootDrive E: if automatic detection is ambiguous.'}
  $BootDrive=$disks[0].DeviceID
 }
 $id=$BootDrive.TrimEnd('\')
 $matched=@($disks | Where-Object { $_.DeviceID -eq $id })
 if($matched.Count -ne 1){throw "Refusing to write: $id is not a recognized BOOTSEL volume"}
 $dest=$id+'\'
 Log "Copying $image to $dest"
 Copy-Item -LiteralPath $image -Destination $dest -Force
 Log 'UF2 copied. USB drive may disappear as device reboots.'
}
try {
 Push-Location $Repo
 Log "blitz86 native Windows runner / action=$Action"
 switch($Action) {
  doctor { Doctor }
  deps {
   $p=PythonExe
   Need 'tools\sst_convert.py'
   Run 'fetch and convert silicon vectors' $p @((Join-Path $PSScriptRoot 'fetch_sst_win.py'),$Repo)
  }
  clean {
   foreach($folder in @('build-win','build-arm-win')) {
    Remove-Item -LiteralPath (Join-Path $Repo $folder) -Recurse -Force -ErrorAction SilentlyContinue
   }
   Log 'Windows native outputs cleaned (no Linux build folder removed).'
  }
  build { NativeBuild; ArmBuild }
  'arm-build' { ArmBuild }
  quick { WindowsTest 'quick' }
  test { WindowsTest 'all' }
  bench { Log 'Native Windows JIT benchmark unavailable: ARM generated code cannot execute on x64 Windows. Use the hardware benchmark adapter once integrated.' }
  all {
   Doctor
   if(!$NoClean){foreach($f in @('build-win','build-arm-win')){Remove-Item -LiteralPath (Join-Path $Repo $f) -Recurse -Force -ErrorAction SilentlyContinue}}
   EnsureVectors 'quick'
   EnsureVectors 'all'
   NativeBuild; ArmBuild
   WindowsTest 'quick'
   WindowsTest 'all'
  }
  microdos { Micro }
  compare { Micro; WindowsTest 'all'; Log 'These are regression tests, not a throughput comparison.' }
  capture { Capture }
  flash { Flash }
 }
 Log 'WORKFLOW PASS'
} catch {
 $script:failed=$true
 Log "WORKFLOW FAIL: $_"
} finally {
 Pop-Location
 Copy-Item -LiteralPath $log -Destination $latest -Force
 Write-Host "Log: $log"
 Write-Host "Latest: $latest"
}
if($script:failed){exit 1}else{exit 0}
