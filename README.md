# LevelMate

LevelMate is a lightweight Windows utility that keeps Firefox audio at a more
consistent listening level. It captures audio from a Firefox process tree with
WASAPI, measures the signal in real time, and automatically reduces loud spikes
or boosts quiet content.

It is useful for streaming services, videos, and other browser audio with large
volume differences between scenes or episodes.

## Features

- Real-time automatic gain control (AGC)
- Fast attenuation of loud peaks
- Optional digital boost for content that is quiet at 100% Firefox volume
- Live terminal meters for raw level, smoothed level, target, and gain
- Firefox-only process targeting; other applications remain unchanged
- Automatic restoration of original Firefox session volumes on clean exit
- No recording, saved audio, network communication, or telemetry

## Operating modes

LevelMate has two modes:

| Mode | Behavior | Best for |
| --- | --- | --- |
| Standard | Adjusts the Windows volume of the selected Firefox sessions. It can reduce loud audio but cannot exceed the 100% session-volume limit. | Controlling spikes with minimal processing |
| Digital boost (`--rerender`) | Attenuates the direct Firefox feed, applies gain to the captured samples, and renders the processed audio to the default output. | Boosting genuinely quiet content as well as controlling spikes |

Both modes target -16 dBFS, use a 50 ms attack and 3000 ms release, ignore
signals below -60 dBFS, and limit automatic gain to +20 dB.

## Requirements

- Windows 10 build 20348 or later, or Windows 11
- x64 system
- Visual Studio 2022 Build Tools with **Desktop development with C++**
- A Windows SDK containing `audioclientactivationparams.h`
- CMake 3.25 or later

Administrator privileges are not required.

## Build

From PowerShell in the repository directory:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable is created at:

```text
build\Release\levelmate-wasapi-probe.exe
```

## Find the Firefox root PID

Start Firefox and begin playing audio. Then list its processes:

```powershell
Get-CimInstance Win32_Process -Filter "Name = 'firefox.exe'" |
  Sort-Object CreationDate |
  Format-Table ProcessId, ParentProcessId, CreationDate -AutoSize
```

Use the oldest Firefox process whose parent is not another Firefox process.
LevelMate validates that the supplied PID belongs to `firefox.exe` before making
any volume changes.

## Usage

### Standard mode

```powershell
.\build\Release\levelmate-wasapi-probe.exe <firefox-root-pid>
```

This mode is the safest starting point. It controls loud content through the
Firefox session volume but cannot amplify audio beyond that session's 100%
ceiling.

### Digital boost mode

```powershell
.\build\Release\levelmate-wasapi-probe.exe <firefox-root-pid> --rerender
```

Digital boost mode keeps a -40 dB Firefox monitor feed available for capture,
compensates for that attenuation, applies AGC directly to the samples, and
renders the result to the current default output device. An instantaneous
limiter prevents processed packet peaks from exceeding the -16 dBFS target.

### Run for a fixed duration

Add `--duration` in either mode:

```powershell
.\build\Release\levelmate-wasapi-probe.exe <firefox-root-pid> --duration 60
.\build\Release\levelmate-wasapi-probe.exe <firefox-root-pid> --rerender --duration 60
```

Without `--duration`, LevelMate runs until you press Ctrl+C.

## Multiple audio sources

LevelMate only processes the selected Firefox process tree. Audio from games,
music players, voice chat, and other applications continues through its normal
Windows audio session.

For example, when World of Warcraft and Crunchyroll are playing together:

- Crunchyroll and other Firefox audio are normalized together.
- World of Warcraft is not modified.
- Windows mixes the processed Firefox stream with the game audio at the output.

The limiter protects Firefox audio, not the final mix of every application.
Multiple Firefox tabs playing simultaneously are treated as one combined
Firefox stream.

## Safety and cleanup

- Begin at a comfortable speaker or headphone volume. Digital boost mode can
  add up to 20 dB to quiet material.
- Stop LevelMate with Ctrl+C or let `--duration` expire. Both paths restore the
  original Firefox session volumes.
- If the process is forcibly terminated or the computer loses power, cleanup
  cannot run. Restore Firefox manually through Windows Volume mixer if needed.
- If the default output device changes while digital boost mode is running,
  restart LevelMate so it can initialize against the new device.

## How it works

1. LevelMate discovers the selected Firefox process and its descendants.
2. WASAPI process loopback captures their audio without writing it to disk.
3. The AGC measures peaks and smooths changes toward the -16 dBFS target.
4. Standard mode adjusts Firefox session volume; digital boost mode processes
   and rerenders the PCM samples.
5. On clean exit, LevelMate stops its audio clients and restores the original
   session volumes.

## Privacy

All audio processing happens locally in memory. LevelMate does not record,
save, upload, or transmit audio, and it contains no network or telemetry path.
