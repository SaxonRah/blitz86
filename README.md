# blitz86 hardware runner v3 — serial re-enumeration

Full replacement `b86_hw.ps1` for `C:\blitz86_v2`. The existing sidecar and Pico firmware do not need to be reinstalled or rebuilt.

```powershell
cd C:\blitz86_v2
Copy-Item "$HOME\Downloads\blitz86_hw_v3\b86_hw.ps1" .\b86_hw.ps1 -Force
Unblock-File .\b86_hw.ps1
.\b86_hw.ps1 doctor
.\b86_hw.ps1 capture -Port COM5
```

The `capture` action does NOT reflash. It searches Windows PnP devices and currently available COM ports for Raspberry Pi VID 2E8A, waits up to 40 seconds, and connects even if the device has been assigned a different COM port. It raises DTR/RTS and sends `R` to repeat the test. All output is saved in `C:\blitz86_v2\logs\latest.txt`. The `run` action still flashes and then captures; `run -NoFlash` only captures.

If capture fails with no ports, check `Get-PnpDevice -PresentOnly | Where-Object {$_.FriendlyName -match 'Pico|RP2350|Serial|USB'}` and Device Manager. It may be a real USB enumeration problem rather than a COM-number issue.

This runner has not been executed against Windows here.
