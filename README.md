# ON AIR Clock

A native C++ Windows tray app that shows camera and microphone activity on an AWTRIX NG clock.

[![Windows build](https://github.com/zliuva/on-air-clock/actions/workflows/windows.yml/badge.svg)](https://github.com/zliuva/on-air-clock/actions/workflows/windows.yml)

**Download `OnAirClock.exe` from the [latest release](https://github.com/zliuva/on-air-clock/releases/latest) and run it on Windows 10/11 x64.** The EXE uses Windows system DLLs and a statically linked C++ standard library. There is no .NET dependency, runtime installer, self-extraction, browser extension, or third-party DLL to copy. Each release includes `SHA256SUMS.txt` to verify the download.

On a new PC, Settings opens on first launch. Enter the clock URL, username and password, then Save. The PC must be able to reach the clock on the network. Credentials from another Windows account are not portable; enter them again on the new PC.

| Windows capture state | Clock display |
| --- | --- |
| Camera active, with or without a microphone | White **ON AIR** on red |
| Microphone active, camera idle | White **ON AIR** on amber |
| Both idle | ON AIR clears; normal rotation resumes |

The lettering is six pixels tall, with blank rows above and below. The four-column gap between ON and AIR gives equal three-column side margins on the 32×8 display.

## Controls and storage

Right-click the circular tray icon for status, Settings, **Pause until resumed**, **Start at sign-in**, Open log, and Quit. Double-click the icon to open Settings. Only one host instance runs per Windows session.

Settings contains the red camera and amber microphone tests. They use the URL and credentials currently entered, so you can test before saving. The disabled buttons show a countdown while the six-second test runs; monitoring resumes afterward.

Settings and the log are written to a `data` folder next to the EXE. The password is encrypted with Windows DPAPI for the current Windows account and is never stored in plaintext. Runtime DLLs or executable payloads are never extracted to disk. For this development checkout, `start.ps1` launches `dist\OnAirClock.exe` using the repository `data` folder.

The icon is gray when idle or paused, red for camera use, amber for microphone-only use, and blue for a connection or detection error. The menu shows the error details. Start at sign-in creates a per-user Windows Run entry; turn it off before moving or deleting the EXE.

## Detection and notification behavior

Camera detection uses the Windows Media Foundation sensor activity monitor. Microphone detection enumerates running Core Audio capture sessions on every active input endpoint, including non-default USB and Bluetooth inputs. The host never opens a capture stream or records media. Native Windows capture activity is the source of truth; it does not inspect application windows or browser UI. An app such as Google Meet can keep capturing while its own mute button is on. That still counts as microphone use.

Capture is polled every 500 ms with an 800 ms release delay to avoid flicker. Camera use takes precedence. The named `on-air-clock` notification uses `hold: true` and `stack: false`. It is sent once when capture begins and updated only when its color changes; there is no periodic renewal. Stopping capture clears only this named alert. Dismissing it on the clock keeps it dismissed until the capture state changes. ON AIR replaces the active notification when sent. No persistent clock settings are changed.

Normal Quit, Windows shutdown/logoff, and sleep attempt to dismiss the held alert, retrying a failed cleanup request up to three times. Quitting during a test clears its preview as well. Startup clears a leftover held alert before applying the current capture state. Cleanup is best effort if the clock cannot be reached; a forced process termination or power loss can leave the held alert for the clock's button to dismiss.

## Build and verification

```powershell
./build.ps1 -Test
```

This builds `dist\OnAirClock.exe`. If needed, the script downloads the official portable Zig 0.17.0 C++ toolchain from ziglang.org into `.build-cache` and verifies its pinned SHA256. No compiler or SDK is needed on the destination PC. The C++ code uses Win32, COM, WinHTTP and DPAPI directly.

The native self-tests cover held alerts without renewals, state transitions, retries, cleanup after failed requests, manual dismissal, pixel layout, settings validation and encrypted credentials.

GitHub Actions runs the build, native self-tests, dependency inspection, single-file portability test and cleanup retry fixture on every branch push and pull request. The Actions page also supports manual runs. Successful builds provide a Windows x64 artifact containing only the EXE and its checksum, retained for 14 days.

To publish a release, tag the intended commit and push the tag:

```powershell
git tag -a v0.2.0 -m "ON AIR Clock v0.2.0"
git push origin v0.2.0
```

Version tags run the same checks, then publish the tested `OnAirClock.exe` and `SHA256SUMS.txt` as release assets. Tags with a suffix, such as `v0.2.0-beta.1`, create prereleases. A release stays in draft until both assets have uploaded; an already published release is never overwritten. Builds run on clean GitHub-hosted runners and do not use local settings, credentials or clock access. The EXE is unsigned. macOS builds will be added when the native macOS implementation is available.

From PowerShell, diagnostics can be run with output redirected to a file (the app is a GUI EXE):

```powershell
$exe = (Resolve-Path './dist/OnAirClock.exe').Path
Start-Process $exe -ArgumentList '--diagnose --seconds 10' -Wait -NoNewWindow -RedirectStandardOutput './activity.txt'
Get-Content './activity.txt'
```

Other switches are `--probe`, `--pulse camera`, `--pulse mic`, `--clear`, `--quit`, `--self-test`, `--settings`, and `--set-password-stdin`. A pulse sends a held alert; use `--clear` to remove it. `--quit` closes the running host through its normal cleanup path. `--verify-clock` runs the live red/amber pixel, held duration, named dismissal and shutdown-style cleanup checks; quit the tray host first so its state changes do not interfere. Pass `--data-dir <path>` to any command to choose another configuration folder. Advanced polling, network retry, test duration and colors remain in `data\settings.json`.

## References

- [AWTRIX NG HTTP API](https://blueforcer.github.io/awtrix-ng/esp32/reference/http/)
- [AWTRIX NG notification payload](https://ang.blueforcer.de/esp32/reference/payload/)
- [Windows camera activity monitor](https://learn.microsoft.com/en-us/windows/win32/api/mfidl/nn-mfidl-imfsensoractivitymonitor)
- [Windows audio session manager](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionmanager2)
