import { LevelMateProcessor, type LevelMateMetrics } from "../audio/LevelMateProcessor";
import { PRESETS, isPresetName, type PresetName } from "../audio/presets";

type DemoPreset = PresetName | "off";

const fileInput = getElement<HTMLInputElement>("media-file");
const mediaContainer = getElement<HTMLDivElement>("media-container");
const presetSelect = getElement<HTMLSelectElement>("preset");
const bypassToggle = getElement<HTMLInputElement>("bypass");
const levelingToggle = getElement<HTMLButtonElement>("leveling-toggle");
const status = getElement<HTMLParagraphElement>("status");
const rmsMetric = getElement<HTMLElement>("metric-rms");
const gainMetric = getElement<HTMLElement>("metric-gain");
const presetMetric = getElement<HTMLElement>("metric-preset");
const activeMetric = getElement<HTMLElement>("metric-active");

let media: HTMLMediaElement | undefined;
let mediaUrl: string | undefined;
let processor: LevelMateProcessor | undefined;
let unsubscribeMetrics: (() => void) | undefined;
let enabled = false;

fileInput.addEventListener("change", () => void loadSelectedFile());
levelingToggle.addEventListener("click", () => void toggleLeveling());
presetSelect.addEventListener("change", handlePresetChange);
bypassToggle.addEventListener("change", () => {
  processor?.setBypassed(!enabled || bypassToggle.checked);
  renderStaticState();
});

async function loadSelectedFile(): Promise<void> {
  const file = fileInput.files?.[0];
  if (!file) return;

  await cleanUpCurrentMedia();
  const isVideo = file.type.startsWith("video/");
  media = document.createElement(isVideo ? "video" : "audio");
  media.controls = true;
  media.preload = "metadata";
  mediaUrl = URL.createObjectURL(file);
  media.src = mediaUrl;
  mediaContainer.replaceChildren(media);

  levelingToggle.disabled = selectedPreset() === "off";
  bypassToggle.disabled = true;
  status.textContent = `Ready: ${file.name}`;
  renderStaticState();
}

async function toggleLeveling(): Promise<void> {
  if (!media) return;

  if (enabled) {
    enabled = false;
    processor?.setBypassed(true);
    bypassToggle.disabled = true;
    status.textContent = "Leveling disabled. Audio is using the direct path.";
    renderStaticState();
    return;
  }

  const preset = selectedPreset();
  if (preset === "off") return;

  levelingToggle.disabled = true;
  status.textContent = "Starting the audio processor…";
  try {
    if (!processor) {
      processor = await LevelMateProcessor.attach(media, preset);
      unsubscribeMetrics = processor.subscribe(renderMetrics);
    } else {
      processor.setPreset(preset);
    }

    enabled = true;
    processor.setBypassed(bypassToggle.checked);
    bypassToggle.disabled = false;
    status.textContent = "Leveling is active.";
  } catch (error) {
    console.error(error);
    status.textContent = "The selected media could not be processed. Try another file.";
  } finally {
    levelingToggle.disabled = false;
    renderStaticState();
  }
}

function handlePresetChange(): void {
  const preset = selectedPreset();
  if (preset === "off") {
    enabled = false;
    processor?.setBypassed(true);
    bypassToggle.disabled = true;
    status.textContent = media ? "Leveling is off." : "Choose a local file to start.";
  } else {
    processor?.setPreset(preset);
    levelingToggle.disabled = !media;
    if (enabled) status.textContent = `${PRESETS[preset].label} preset active.`;
  }
  renderStaticState();
}

function renderMetrics(metrics: LevelMateMetrics): void {
  rmsMetric.textContent = Number.isFinite(metrics.rmsDb) ? `${metrics.rmsDb.toFixed(1)} dB` : "—";
  gainMetric.textContent = `${metrics.appliedGainDb >= 0 ? "+" : ""}${metrics.appliedGainDb.toFixed(1)} dB`;
  presetMetric.textContent = PRESETS[metrics.preset].label;
  activeMetric.textContent = enabled && !metrics.bypassed ? "Active" : "Inactive";
}

function renderStaticState(): void {
  const preset = selectedPreset();
  presetMetric.textContent = preset === "off" ? "Off" : PRESETS[preset].label;
  activeMetric.textContent = enabled && !bypassToggle.checked ? "Active" : "Inactive";
  levelingToggle.textContent = enabled ? "Disable leveling" : "Enable leveling";
  levelingToggle.disabled = !media || preset === "off";
  if (!processor) {
    rmsMetric.textContent = "—";
    gainMetric.textContent = "0.0 dB";
  }
}

async function cleanUpCurrentMedia(): Promise<void> {
  enabled = false;
  unsubscribeMetrics?.();
  unsubscribeMetrics = undefined;
  await processor?.dispose();
  processor = undefined;
  media?.pause();
  media = undefined;
  if (mediaUrl) URL.revokeObjectURL(mediaUrl);
  mediaUrl = undefined;
}

function selectedPreset(): DemoPreset {
  return isPresetName(presetSelect.value) ? presetSelect.value : "off";
}

function getElement<T extends HTMLElement>(id: string): T {
  const element = document.getElementById(id);
  if (!element) throw new Error(`Missing required element: #${id}`);
  return element as T;
}

window.addEventListener("beforeunload", () => {
  if (mediaUrl) URL.revokeObjectURL(mediaUrl);
});

renderStaticState();
