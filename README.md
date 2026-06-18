# LevelMate

LevelMate is a local browser audio-leveling experiment for shows, movies, and other video playback. Some programs move abruptly between quiet dialogue and loud music or action, leaving the viewer repeatedly adjusting the volume. LevelMate explores whether a small, understandable Web Audio processor can make those changes less disruptive.

The project currently contains a local media playground and an experimental browser extension. It is a prototype, not a promise of compatibility with any particular streaming service.

## What it does

For media the browser allows it to access, LevelMate samples the waveform, estimates RMS loudness, and smoothly adjusts gain toward a preset target. Gain correction is bounded, and a compressor reduces peaks after automatic gain control.

The processing chain is:

```text
media element
  -> input gain
  -> analyser / RMS estimator
  -> automatic gain
  -> compressor
  -> output gain
  -> destination
```

Three presets provide different amounts of correction:

| Preset | Target RMS | Gain range | Compressor threshold | Ratio |
| --- | ---: | ---: | ---: | ---: |
| Light | -24 dB | -6 to +6 dB | -20 dB | 2.5:1 |
| Normal | -22 dB | -10 to +10 dB | -24 dB | 4:1 |
| Night | -20 dB | -14 to +14 dB | -28 dB | 6:1 |

## Privacy

LevelMate runs locally. It does not:

- Record or save audio.
- Upload audio or media files.
- Use cloud services.
- Include analytics or tracking.

The playground uses a temporary browser object URL for the file you select and revokes it when the media is replaced or the page closes. The extension remembers only the selected preset.

## Run the playground

Requirements: a current Node.js release and npm.

```bash
npm install
npm run dev
```

Open the local URL printed by Vite, select an audio or video file, start playback, and enable leveling. The bypass checkbox provides a direct before/after comparison without forgetting the selected preset.

To create production builds:

```bash
npm run build
```

The playground is written to `dist/demo` and the extension to `dist/extension`.

## Load the extension temporarily

The current extension package can be tested in Firefox:

1. Run `npm run build:extension`.
2. Open `about:debugging`.
3. Select **This Firefox**.
4. Select **Load Temporary Add-on**.
5. Choose `dist/extension/manifest.json`.
6. Open a page with an ordinary HTML5 `<video>`, open LevelMate, choose a preset, and select **Enable on this page**.

Opening the popup does not alter page audio. Attachment occurs only after the Enable button is selected, and it must be selected again after a page load. If playback becomes silent, use **Reload page to restore playback**.

## Development commands

```bash
npm run typecheck       # strict TypeScript validation
npm test                # deterministic audio utility tests
npm run build:demo      # playground production build
npm run build:extension # extension production build
npm run build           # typecheck and build both targets
```

Shared audio code lives in `src/audio`, while the demo and browser integration are isolated in `src/demo` and `src/extension`. This keeps the audio algorithm independent of browser-specific APIs and allows additional browser integrations to be evaluated later.

## Known limitations

- Web Audio access to a page's media is constrained by browser security rules. Cross-origin media may be silent when routed through `createMediaElementSource()`.
- Protected or DRM-controlled players may hide their media, block usable audio access, or use a playback path the extension cannot process.
- A media element cannot always be returned to its original audio path after attachment. Reloading the page is the reliable recovery when an unsupported source becomes silent.
- The extension currently tries the first sourced, non-ended `<video>` element. Pages with custom players, multiple videos, iframes, or dynamically replaced elements may not work.
- RMS is a simple signal-level estimate, not a broadcast loudness measurement such as LUFS. The initial algorithm may pump or react poorly to unusual material.
- Compatibility with Netflix, Crunchyroll, YouTube, and other individual services has not been established and is not claimed.

## Roadmap

- Test the processing behavior on unprotected HTML5 media and tune its response from real listening sessions.
- Add better media selection and lifecycle handling for dynamic pages.
- Evaluate additional browser extension integrations after the prototype proves useful.
- Explore an optional native helper or system-audio route only as a later, separately reviewed phase. That work would require substantially different permissions, installation, and security design.
