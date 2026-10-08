# blitz86 Windows/WSL runner (non-invasive)

Install these three files into `C:\blitz86_v2`:

- `b86.ps1`
- `b86.bat`
- `capture_serial.py`

Requires Windows PowerShell 5.1+, WSL Ubuntu/Debian, Python 3 for serial, and the blitz86-style Makefile (`make all`, `make quick`, `make test`, `make bench`). No repository sources are changed. Logs go to `C:\blitz86_v2\logs\` and `logs\latest.txt` is overwritten on each run. `make clean` removes the Makefile's `build` directory (normal build artifact cleanup).

## First use

```powershell
cd C:\blitz86_v2
.\b86.bat doctor
.\b86.bat deps
.\b86.bat all
```

`deps` invokes `sudo apt-get`, so WSL can ask for the Linux password. `all` obtains SingleStepTests vectors if `sst/all.bin` is absent; this can download a large fixture set. Run `build` for compilation only; `quick` or `test` for tests; `bench` for instruction-count benchmarks.

```powershell
.\b86.bat build
.\b86.bat quick
.\b86.bat test -FuzzCount 4000
.\b86.bat bench
.\b86.bat compare -MicroDos C:\microDOS
```

`compare` performs microDOS `md.bat test all` and blitz86's own tests and instruction-count benchmark **with separate workloads**; it does not establish a true apples-to-apples speed ratio. Integrating blitz86 with microDOS's DOS runtime is a separate CPU-state/memory/interrupt adapter effort.

## Serial capture

```powershell
py -m pip install pyserial
.\b86.bat capture -Port COM5 -Baud 115200 -Seconds 120
```

This captures an existing running firmware console. It does not imply that blitz86 currently creates firmware.

## UF2 flashing (only when firmware exists)

```powershell
# Put Pico 2 in BOOTSEL mode, verify that the USB drive appears.
.\b86.bat flash -Uf2 C:\path\to\your_firmware.uf2
# Or explicitly specify the actual BOOTSEL drive:
.\b86.bat flash -Uf2 C:\path\to\your_firmware.uf2 -BootDrive E:
```

Flashing validates the destination's removable drive volume label. This wrapper neither generates firmware nor power-cycles hardware. Do not flash random UF2 files from other boards.

## Limitations

- As of the inspected public `SaxonRah/blitz86` Makefile, there is no Pico2 UF2 target or bare-metal Raspberry Pi image target; QEMU `arm-linux-gnueabihf` userspace tests are **not** Pico Cortex-M33 firmware.
- `C:\blitz86_v2` is a local path not directly accessible here, and may differ from public `blitz86`. Run `doctor` first and inspect the log if Makefile targets differ.
- For actual apples-to-apples results, add a microDOS runtime adapter and a shared guest workload harness. Avoid conflating QEMU host-instruction counts with measured hardware MIPS.
- Windows `python` must resolve to Python 3 for `capture`; if not, change that command to `py -3` locally.
