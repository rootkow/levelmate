# LevelMate

LevelMate is a command-line audio utility that keeps one application's volume
at a more consistent listening level. It monitors a selected application,
measures its audio in real time, and reduces loud passages. An optional digital
mode can also boost quiet content.

It is useful with browsers, media players, games, and other desktop
applications. The current implementation supports Windows audio.

## Features

- Real-time automatic gain control (AGC)
- Fast attenuation of loud peaks
- Optional digital boost for content that remains quiet at 100% app volume
- Live terminal meters for raw level, smoothed level, target, and gain
- Application targeting that leaves unrelated audio unchanged
- Automatic restoration of the application's original volume on clean exit
- No recording, saved audio, network communication, or telemetry

## Operating modes

| Mode | Behavior | Best for |
| --- | --- | --- |
| Standard (default) | Adjusts the selected application's volume. It can reduce loud audio but cannot raise the application above 100%. | Controlling loud passages with minimal processing |
| Digital boost (`--rerender`) | Attenuates the application's direct feed, applies gain to captured samples, and sends the processed audio to the default output device. | Boosting quiet content as well as controlling loud passages |

Both modes target -16 dBFS, use a 50 ms attack and 3000 ms release, avoid
boosting signals below -60 dBFS, and limit automatic gain to +20 dB.

## Platform support

LevelMate currently uses the Windows Audio Session API (WASAPI). Building and
running it requires:

- Windows 11 or later (Windows build 20348+)
- x64 system
- Visual Studio 2022 Build Tools with **Desktop development with C++**
- A Windows SDK containing `audioclientactivationparams.h`
- CMake 3.25 or later

Administrator privileges are not required.

## Quick start

Open PowerShell in the repository directory, then build a release executable:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Choose the root PID of an application that is already playing audio, then run:

```powershell
.\build\Release\levelmate-wasapi-probe.exe <root-pid>
```

Press Ctrl+C to stop. LevelMate restores the application volume it changed
before exiting.

The executable is located at:

```text
build\Release\levelmate-wasapi-probe.exe
```

## Run the tests

Build and run the native unit tests with CTest:

```powershell
cmake --build build --config Release --target levelmate-tests
ctest --test-dir build -C Release --output-on-failure
```

The tests cover CLI parsing, silence gating, gain limits, peak limiting, PCM
sample conversion, clipping, and the audio ring buffer. They do not require an
active audio device or target application.

## Choose a target process

LevelMate identifies the application to normalize by its root process ID (PID).
Audio from that process and its descendants is treated as one source. LevelMate
discovers active audio streams when it starts, so start playback first. If the
application creates a new audio process or stream later, restart LevelMate.

List candidate processes in PowerShell:

```powershell
Get-CimInstance Win32_Process |
  Sort-Object CreationDate |
  Format-Table Name, ProcessId, ParentProcessId, CreationDate -AutoSize
```

For a multi-process application such as Firefox or Chrome, choose the oldest
long-lived application process whose parent is not another process from the
same application. For a game or media player, the main executable is usually
the correct target.

## Usage

### Standard mode

```powershell
.\build\Release\levelmate-wasapi-probe.exe <root-pid>
```

Start with this mode. It controls loud content through the application's volume
but cannot amplify beyond the 100% ceiling.

### Digital boost mode

```powershell
.\build\Release\levelmate-wasapi-probe.exe <root-pid> --rerender
```

Digital boost mode reduces the application's normal output by 40 dB, captures
it, compensates for that reduction, and applies AGC directly to the samples. It
then sends the processed audio to the current default output device. An
instantaneous limiter keeps processed packet peaks at or below the -16 dBFS
target.

### Fixed duration

Use `--duration <seconds>` to stop automatically after a positive whole number
of seconds:

```powershell
.\build\Release\levelmate-wasapi-probe.exe <root-pid> --duration 60
.\build\Release\levelmate-wasapi-probe.exe <root-pid> --rerender --duration 60
```

Options may appear in either order. Without `--duration`, LevelMate runs until
you press Ctrl+C.

## Multiple audio sources

LevelMate processes only the selected application tree. Audio from games,
browsers, music players, voice chat, and other unrelated applications continues
through its normal output path.

For example, if LevelMate targets a browser while a game is running:

- Audio from the targeted browser tree is normalized together.
- The game's audio is not modified.
- The system mixes the processed browser stream with the game at the output.

The limiter protects the targeted application's audio, not the final mix of
every application. Multiple audio sources inside one target process tree are
treated as one combined stream.

Run only one LevelMate instance against a given target process. Multiple
instances targeting the same app would compete over volume control and cleanup.

## Safety and cleanup

- Begin at a comfortable speaker or headphone volume. Digital boost mode can
  add up to 20 dB to quiet material.
- Stop LevelMate with Ctrl+C or let `--duration` expire. Both paths restore the
  target application's volume settings captured at startup.
- If the process is forcibly terminated or the computer loses power, cleanup
  cannot run. Restore the app manually through Windows Volume mixer if needed.
- If the default output device changes while digital boost mode is running,
  restart LevelMate so it can initialize against the new device.
- Some protected, elevated, sandboxed, or exclusive-mode applications may not
  expose audio that process loopback can capture.
- LevelMate only discovers sessions on the default console output device. If it
  reports that no active sessions were found, confirm that the target is playing
  through that device, then restart LevelMate.

## How it works

1. LevelMate validates that the target PID is active and discovers its child
   processes.
2. It finds the application's active audio streams on the default output
   device.
3. Process-loopback capture reads their audio without writing it to disk.
4. The AGC measures peaks and smooths changes toward the -16 dBFS target.
5. Standard mode adjusts application volume; digital boost mode processes and
   rerenders PCM samples.
6. On clean exit, LevelMate stops audio processing and restores the original
   application volume.

## Privacy

All audio processing happens locally in memory. LevelMate does not record,
save, upload, or transmit audio, and it contains no network or telemetry path.
