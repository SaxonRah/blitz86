[CmdletBinding()]
param([string]$MicroDos='C:\microDOS',[string]$Blitz86='C:\blitz86_v2')
$ErrorActionPreference='Stop'
$source=Split-Path -Parent $MyInvocation.MyCommand.Path
if(!(Test-Path "$MicroDos\pico\CMakeLists.txt")){throw "microDOS Pico CMake not found: $MicroDos"}
if(!(Test-Path "$Blitz86\include\b86.h")){throw "blitz86 API not found: $Blitz86"}
$cmake=Join-Path $MicroDos 'pico\CMakeLists.txt'
$line='include("${MD_ROOT}/pico/blitz86_target.cmake")'
$body=[IO.File]::ReadAllText($cmake)
if($body -notmatch 'blitz86_target\.cmake') {
 [IO.File]::AppendAllText($cmake,"`r`n# Optional blitz86 sidecar (does not modify existing targets)`r`n$line`r`n")
 Write-Host 'Added sidecar include to microDOS Pico CMakeLists.txt'
} else { Write-Host 'Sidecar already registered' }
Copy-Item -LiteralPath (Join-Path $source 'pico\blitz86_pico_probe.c') -Destination (Join-Path $MicroDos 'pico\blitz86_pico_probe.c') -Force
Copy-Item -LiteralPath (Join-Path $source 'integration\blitz86_target.cmake') -Destination (Join-Path $MicroDos 'pico\blitz86_target.cmake') -Force
Write-Host 'Sidecar files installed. Existing microDOS source engines were not changed.'
Write-Host 'Build: .\b86_hw.ps1 build'
