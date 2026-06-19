# LevelMate

LevelMate is a lightweight Windows utility that keeps application audio at a
more consistent listening level. It captures audio from a selected process tree
with WASAPI, measures the signal in real time, and automatically reduces loud
spikes or boosts quiet content.

It works with browsers, media players, games, and other desktop applications
that expose a Windows audio session.

## Features

- Real-time automatic gain control (AGC)
- Fast attenuation of loud peaks
- Optional digital boost for content that remains quiet at 100% app volume
- Live terminal meters for raw level, smoothed level, target, and gain
- Process-tree targeting that leaves unrelated applications unchanged
- Automatic restoration of original session volumes on clean exit
- No recording, saved audio, network communication, or telemetry

## Operating modes

| Mode | Behavior | Best for |
| --- | --- | --- |
| Standard | Adjusts the Windows volume of the selected application's sessions. It can reduce loud audio but cannot exceed the 100% session-volume limit. | Controlling spikes with minimal processing |
| Digital boost (`--rerender`) | Attenuates the app's direct feed, applies gain to captured samples, and renders the processed audio to the default output. | Boosting genuinely quiet content as well as controlling spikes |

Both modes target -16 dBFS, use a 50 ms attack and 3000 ms release, avoid
boosting signals below -60 dBFS, and limit automatic gain to +20 dB.

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

## Tests

Build and run the native unit tests with CTest:

```powershell
cmake --build build --config Release --target levelmate-tests
ctest --test-dir build -C Release --output-on-failure
```

The tests cover CLI parsing, silence gating, gain limits, peak limiting, PCM
sample conversion, clipping, and the audio ring buffer. They do not require an
active audio device or target application.

## Choose a target process

LevelMate accepts the root PID of the application to normalize. It includes
audio from that process and its descendants.

List candidate processes in PowerShell:

```powershell
Get-CimInstance Win32_Process |
  Sort-Object CreationDate |
  Format-Table Name, ProcessId, ParentProcessId, CreationDate -AutoSize
```

For a multi-process application such as Firefox or Chrome, choose the oldest
long-lived application process whose parent is not another process from the
same application. For a game or media player, the main executable is usually
the correct target. Start audio before launching LevelMate so its audio session
can be discovered.

## Usage

### Standard mode

```powershell
.\build\Release\levelmate-wasapi-probe.exe <root-pid>
```

This is the safest starting point. It controls loud content through application
session volume but cannot amplify beyond the session's 100% ceiling.

### Digital boost mode

```powershell
.\build\Release\levelmate-wasapi-probe.exe <root-pid> --rerender
```

Digital boost mode keeps a -40 dB monitor feed available for capture,
compensates for that attenuation, applies AGC directly to the samples, and
renders the result to the current default output device. An instantaneous
limiter prevents processed packet peaks from exceeding the -16 dBFS target.

### Fixed duration

Add `--duration` in either mode:

```powershell
.\build\Release\levelmate-wasapi-probe.exe <root-pid> --duration 60
.\build\Release\levelmate-wasapi-probe.exe <root-pid> --rerender --duration 60
```

Without `--duration`, LevelMate runs until you press Ctrl+C.

## Multiple audio sources

LevelMate processes only the selected application tree. Audio from games,
browsers, music players, voice chat, and other unrelated applications continues
through its normal Windows audio session.

For example, if LevelMate targets a browser while a game is running:

- Audio from the targeted browser tree is normalized together.
- The game's audio is not modified.
- Windows mixes the processed browser stream with the game at the output.

The limiter protects the targeted application's audio, not the final mix of
every application. Multiple audio sources inside one target process tree are
treated as one combined stream.

Run only one LevelMate instance against a given target process. Multiple
instances targeting the same app would compete over session volume and cleanup.

## Safety and cleanup

- Begin at a comfortable speaker or headphone volume. Digital boost mode can
  add up to 20 dB to quiet material.
- Stop LevelMate with Ctrl+C or let `--duration` expire. Both paths restore the
  original target session volumes.
- If the process is forcibly terminated or the computer loses power, cleanup
  cannot run. Restore the app manually through Windows Volume mixer if needed.
- If the default output device changes while digital boost mode is running,
  restart LevelMate so it can initialize against the new device.
- Some protected, elevated, sandboxed, or exclusive-mode applications may not
  expose audio that process loopback can capture.

## How it works

1. LevelMate validates that the target PID is active and discovers its child
   processes.
2. It finds matching sessions on the default Windows render endpoint.
3. WASAPI process loopback captures their audio without writing it to disk.
4. The AGC measures peaks and smooths changes toward the -16 dBFS target.
5. Standard mode adjusts session volume; digital boost mode processes and
   rerenders PCM samples.
6. On clean exit, LevelMate stops its audio clients and restores the original
   session volumes.

## Privacy

All audio processing happens locally in memory. LevelMate does not record,
save, upload, or transmit audio, and it contains no network or telemetry path.
