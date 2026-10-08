# blitz86 Windows-native toolkit v5

This version replaces the broken nested `cmd.exe /c` MSVC command with the **same Visual Studio 2022 x64 CMake generator already used successfully by microDOS**.

Extract the ZIP into `C:\blitz86_v2`, maintaining the `tools\windows\CMakeLists.txt` subdirectory. Replace all old toolkit files. Do not overwrite blitz86's repository-root Makefile.

```powershell
cd C:\blitz86_v2
.\b86.bat doctor
.\b86.bat deps
.\b86.bat all
```

The build runs `cmake -S tools/windows -B build-win -G "Visual Studio 17 2022" -A x64`, then `cmake --build build-win --config Release --target sst_interp`.
The actual executable is `build-win\Release\sst_interp.exe`.
No calls to vcvars64.bat, cmd.exe /c quoting, WSL, Make, or QEMU are used to build the Windows interpreter.

Other actions: `build`, `quick`, `test`, `clean`, `arm-build`, `microdos`, `compare`, `capture`, `flash`. ARM compilation remains optional and requires separately installed Windows ARM toolchains. Firmware flashing requires an existing integrated UF2 and does not build one. ARM JIT execution is not possible within a native Windows x64 interpreter test.

Logs: `logs\latest.txt` and timestamped `logs\b86-*.log`.

This package has NOT been executed with your Visual Studio installation. If your local blitz86_v2 source differs from the public repo or uses POSIX-only functionality, a source-level MSVC compatibility issue may be reported after the generator fix; the log will now include that compiler diagnostic.
