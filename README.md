# blitz86 unified hardware v5 — HID preflight PowerShell quoting fix

The v4 Pi preflight used `python -c` with nested quotes, which PowerShell passed incorrectly, producing a Python SyntaxError. This package replaces it with an actual Python script and preserves the existing microDOS v29 HID / rpiboot runner.

Extract **both** `b86_hardware.ps1` and `tools/b86_hid_preflight.py` into `C:\blitz86_v2`, maintaining the `tools` folder.

```powershell
cd C:\blitz86_v2
Unblock-File .\b86_hardware.ps1
.\b86_hardware.ps1 pi
```

To independently diagnose bridge discovery:

```powershell
python .\tools\b86_hid_preflight.py
```

Result exit codes: 0 found, 12 not found, 13 missing hidapi, 14 USB enumeration error. The Pi runner will then stream rpiboot/HID output and check its blitz86 PASS marker. No firmware or CMake changes. Windows execution is not verified here.
