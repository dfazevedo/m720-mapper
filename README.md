# M720 Mapper

[Português do Brasil](README.pt-BR.md)

Native Windows x64 mapper for the Logitech M720 Triathlon paired via Bluetooth LE (product ID B015).

Logitech manufactures the M720 Triathlon mouse. M720 Mapper is an independent project with no affiliation to Logitech.

| Button | Shortcut |
| --- | --- |
| Forward | Ctrl+C |
| Back | Ctrl+V |
| Thumb / gesture | Print Screen |

The middle click, wheel tilt and scrolling retain their normal behavior. The action for Print Screen follows Windows settings.

## Use

Run `M720Mapper.exe` to start it without a window. Leave it running when switching to another computer with Easy-Switch. When Windows reports the mouse back on this computer, the Mapper reads a fresh HID++ state and rearms the three shortcuts. If Windows reports the device connected before its GATT service is ready, the Mapper retries after up to 2.5 seconds plus Windows Bluetooth response time.

```powershell
.\M720Mapper.exe --status
.\M720Mapper.exe --stop
```

`--stop` waits for the Mapper to restore temporary reporting settings on the current connection. A lost Bluetooth link invalidates its old snapshot. The Mapper avoids replaying that snapshot into a later connection. On an unclean shutdown, the temporary device state may remain until reconnection.

`--status` exit codes are 0 for shortcuts active, 1 for waiting for a disconnected mouse, 3 for stopped, 4 for starting or retrying, and 2 for an error. The Mapper runs at the user's privilege level. Windows can block keystrokes destined for an elevated application.

## Start at sign-in

From an ordinary PowerShell session, run the release's installer. No administrator privileges are required.

```powershell
.\scripts\Install-M720Mapper.ps1
```

The executable is copied to `%LOCALAPPDATA%\M720Mapper` and registered for the current user in HKCU Run. The installer can migrate the previous version when its original graceful-stop helper is present. It backs up the earlier executable, helpers and startup entry. Use `-RequireActive` to require the mouse to connect and the shortcuts to arm before installation succeeds.

The installer checks `--status` before committing the startup entry. It accepts status 0 or, without `-RequireActive`, status 1. Both scripts abort if an existing instance fails to stop gracefully. If startup fails, the installer attempts to restore the previous files and startup entry from its backup.

```powershell
.\scripts\Uninstall-M720Mapper.ps1
```

Uninstall stops the Mapper, removes its executable and clears its startup entry. The log and migration backups remain. The log is `%LOCALAPPDATA%\M720Mapper\Mapper.log`, capped at 1 MiB per file with one rotated copy.

## Diagnose and build

```powershell
.\M720Mapper.exe --diagnose
.\M720Mapper.exe --probe
.\M720Mapper.exe --listen 30
```

`--diagnose` reads the GATT characteristic. `--probe` queries HID++ and the three reporting states. `--listen` records presses the mouse already diverts, for 1 to 120 seconds. Stop the resident Mapper before `--probe` or `--listen`. These commands leave button reporting intact. They enable GATT notifications temporarily if needed, then restore the previous notification setting.

Build with Visual Studio Build Tools 2022, MSVC x64, a Windows SDK with C++/WinRT headers, and CMake.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The Release build links the C++ runtime statically. The executable uses Windows system Bluetooth and input APIs. Protocol tests cover packet framing, reply matching, press order and deduplication, and reverse-order restoration planning.

## Measured on Windows

On one Windows laptop, each version was sampled 21 times at 30-second intervals over 10 minutes while idle with the mouse connected. The legacy totals include its mapper and launcher. The native build measured here uses the same resident code as the release.

| Metric | Legacy (two processes) | Native |
| --- | ---: | ---: |
| Working set, median / sampled peak | 46.094 / 46.262 MiB | 17.348 / 17.496 MiB |
| Committed private memory, median / sampled peak | 31.762 / 31.836 MiB | 2.957 / 3.168 MiB |
| Process CPU time added over 10 minutes | 0 s | 0.0157 s |
| Open handles, sampled range | 524–526 | 214–222 |

Median working set fell 62% and median committed private memory fell 91%. The native build met the 5 MiB private-memory target and exceeded the 10 MiB working-set target. In a separate working-set inspection, 15.17 MiB of 17.21 MiB were shared pages.

Hardware checks covered two Easy-Switch returns and one mouse power cycle. The user confirmed the three shortcuts. New Windows sign-in, sleep and a ten-cycle run remain unverified.

Protocol and API references: [Logitech HID++ 2.0](https://github.com/Logitech/cpg-docs/blob/master/hidpp20/README.rst), [feature 0x1B04](https://lekensteyn.nl/files/logitech/x1b04_specialkeysmsebuttons.html), [Microsoft Bluetooth LE](https://learn.microsoft.com/en-us/windows/win32/api/bluetoothleapis/).
