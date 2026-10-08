[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Firmware,
    [string]$Port='COM5',
    [Parameter(Mandatory)][string]$Log,
    [int]$Seconds=90,
    [switch]$NoFlash
)
$ErrorActionPreference='Stop'
if (!(Test-Path -LiteralPath $Firmware -PathType Leaf)) { throw "UF2 not found: $Firmware" }
$Firmware=(Resolve-Path -LiteralPath $Firmware).Path
Write-Host "PICO UF2: $Firmware"
$serial=$null
try {
    if (!$NoFlash) {
        $picotool=Join-Path $HOME '.pico-sdk\picotool\2.3.0\picotool\picotool.exe'
        if (!(Test-Path -LiteralPath $picotool -PathType Leaf)) { throw "picotool missing: $picotool" }
        $flashed=$false
        for ($attempt=1; $attempt -le 3 -and !$flashed; $attempt++) {
            Write-Host "=== Pico BOOTSEL / flash attempt $attempt/3 ==="
            # On RP2350 the USB connection disappears during the reboot; the
            # reboot command can print 'device returned an error: rebooting'.
            # Treat that as transitional, and let the subsequent LOAD decide.
            & $picotool reboot -f -u 2>&1 | Out-Host
            $rebootExit=$LASTEXITCODE
            Write-Host "picotool reboot exit=$rebootExit (not a flash result)"
            Start-Sleep -Milliseconds 1200
            $loadOutput=@(& $picotool load -v -x $Firmware 2>&1)
            $loadExit=$LASTEXITCODE
            foreach ($line in $loadOutput) { Write-Host ([string]$line) }
            if ($loadExit -eq 0) {
                $flashed=$true
                Write-Host 'PASS Pico flash'
            } else {
                Write-Host "picotool load exit=$loadExit; retrying after USB re-enumeration"
                Start-Sleep -Milliseconds 1300
            }
        }
        if (!$flashed) { throw 'Pico flash failed after 3 attempts; firmware was not verified loaded' }
    }
    Write-Host "=== Locate Pico application CDC: $Port ==="
    $deadline=(Get-Date).AddSeconds(30)
    do {
        $found=Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
            Where-Object {
                $_.DeviceID -eq $Port -and
                $_.PNPDeviceID -match 'VID_2E8A' -and
                $_.PNPDeviceID -match 'PID_0009|PID_000A'
            } | Select-Object -First 1
        if (!$found) { Start-Sleep -Milliseconds 250 }
    } while (!$found -and (Get-Date) -lt $deadline)
    if (!$found) { throw "Pico application CDC not found on $Port (VID_2E8A PID_0009/000A)" }
    $serial=[System.IO.Ports.SerialPort]::new($Port,115200,[System.IO.Ports.Parity]::None,8,[System.IO.Ports.StopBits]::One)
    $serial.DtrEnable=$true
    $serial.RtsEnable=$false
    $serial.ReadTimeout=100
    $serial.Open()
    Start-Sleep -Milliseconds 200
    $deadline=(Get-Date).AddSeconds($Seconds)
    $tail=''
    $complete=$false
    while ((Get-Date) -lt $deadline) {
        $chunk=$serial.ReadExisting()
        if ($chunk) {
            Write-Host -NoNewline $chunk
            Add-Content -LiteralPath $Log -Value $chunk -NoNewline
            $tail+=$chunk
            if ($tail.Length -gt 16000) { $tail=$tail.Substring($tail.Length-16000) }
            if ($tail -match '\[(?:b86-jit|n3-compare)\] COMPLETE result=(?:PASS|FAIL)') {
                $complete=$true
                Start-Sleep -Milliseconds 250
                break
            }
        } else { Start-Sleep -Milliseconds 50 }
    }
    if (!$complete) { throw "Timed out after $Seconds seconds without firmware COMPLETE marker" }
    Write-Host "`nPASS Pico console capture completed"
} catch {
    Write-Host "PICO CAPTURE ERROR: $_"
    exit 1
} finally {
    if ($null -ne $serial) {
        if ($serial.IsOpen) { $serial.Close() }
        $serial.Dispose()
    }
}
