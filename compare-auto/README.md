# blitz86 + microDOS unified hardware automation — v3

Full source files (not Git patches). Reuses the existing microDOS Windows CMake, RP2350 picotool, and Pi Zero 2 W v29 HID UART/rpiboot pipeline.

## Install and run

Extract the ZIP and replace these two files under `C:\blitz86_v2\compare-auto`:

- `auto_compare.ps1`
- `pico_flash_capture.ps1`

From Windows PowerShell:

```powershell
cd C:\blitz86_v2\compare-auto
Unblock-File .\*.ps1
.\auto_compare.ps1 all -Port COM5
```

You can run `pico`, `pi`, `blitz-pico`, `microdos-pico`, `blitz-pi`, or `microdos-pi` individually. `-ContinueOnFailure` continues after an error. `-NoBuild` reuses existing firmware. Logs and CSV go to `C:\blitz86_v2\logs\compare`.

## Changes from v2

1. Picotool reboot errors like `The RP2350 device returned an error: rebooting` are not treated as final flash errors: disconnect/re-enumeration can happen during reset.
2. Waits 1.2 seconds for USB to settle before `picotool load -v -x`.
3. Retries flash up to three times. **Only a successful load exit is considered a successful flash.** If all attempts fail, prints an explicit error and stops.
4. Recognizes the actual microDOS `[n3-compare] COMPLETE result=PASS` marker, so it no longer waits the whole capture timeout after the program finishes.
5. Requires all three `[compare] ... phase=warm-median-7 result=PASS` lines and overall PASS before accepting a microDOS run.
6. Keeps the known-good Pi `md_pi0w_run.ps1` untouched.

This package has not been run on your Windows hardware. The firmware and original build scripts are not modified. In particular, if `picotool load` fails after all retries, the automation will report failure instead of claiming the image was successfully flashed.
