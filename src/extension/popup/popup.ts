import { isPresetName, type PresetName } from "../../audio/presets";
import type { LevelMateCommand, LevelMateStatus } from "../messages";

const presetSelect = getElement<HTMLSelectElement>("preset");
const enableButton = getElement<HTMLButtonElement>("enable");
const disableButton = getElement<HTMLButtonElement>("disable");
const reloadButton = getElement<HTMLButtonElement>("reload");
const statusText = getElement<HTMLParagraphElement>("status");
const stateBadge = getElement<HTMLSpanElement>("state-badge");

let activeTabId: number | undefined;

enableButton.addEventListener("click", () => void enableOnPage());
disableButton.addEventListener("click", () => void disableOnPage());
reloadButton.addEventListener("click", () => void reloadPage());
presetSelect.addEventListener("change", () => void updatePreset());

async function initialize(): Promise<void> {
  const stored = await browser.storage.local.get("preset");
  if (isPresetName(stored.preset)) presetSelect.value = stored.preset;

  const [tab] = await browser.tabs.query({ active: true, currentWindow: true });
  activeTabId = tab?.id;
  if (activeTabId === undefined) {
    showUnavailable("No active browser tab was found.");
    return;
  }

  try {
    const response = await sendCommand({ type: "get-status" });
    renderStatus(response);
  } catch {
    renderStatus({
      ok: true,
      attached: false,
      active: false,
      preset: selectedPreset(),
      message: "Ready. Enable LevelMate to try the first HTML5 video on this page.",
    });
  }
}

async function enableOnPage(): Promise<void> {
  if (activeTabId === undefined) return;
  setBusy(true, "Attempting to attach to this page…");

  try {
    await browser.scripting.executeScript({
      target: { tabId: activeTabId },
      files: ["content/content.js"],
    });
    const response = await sendCommand({ type: "enable", preset: selectedPreset() });
    renderStatus(response);
  } catch (error) {
    console.error(error);
    showUnavailable(
      "This site’s video/audio could not be processed. DRM or cross-origin restrictions may prevent LevelMate from accessing the audio.",
    );
  } finally {
    setBusy(false);
  }
}

async function disableOnPage(): Promise<void> {
  try {
    renderStatus(await sendCommand({ type: "disable" }));
  } catch {
    showUnavailable("LevelMate is not attached to this page.");
  }
}

async function updatePreset(): Promise<void> {
  const preset = selectedPreset();
  await browser.storage.local.set({ preset });
  try {
    renderStatus(await sendCommand({ type: "set-preset", preset }));
  } catch {
    statusText.textContent = "Preset saved. Enable LevelMate to attach.";
  }
}

async function reloadPage(): Promise<void> {
  if (activeTabId !== undefined) await browser.tabs.reload(activeTabId);
  window.close();
}

async function sendCommand(command: LevelMateCommand): Promise<LevelMateStatus> {
  if (activeTabId === undefined) throw new Error("No active tab.");
  return browser.tabs.sendMessage(activeTabId, command) as Promise<LevelMateStatus>;
}

function renderStatus(status: LevelMateStatus): void {
  presetSelect.value = status.preset;
  statusText.textContent = status.message;
  stateBadge.textContent = status.active ? "Active" : "Inactive";
  stateBadge.classList.toggle("active", status.active);
  disableButton.disabled = !status.attached || !status.active;
  enableButton.textContent = status.attached ? "Enable leveling" : "Enable on this page";
}

function showUnavailable(message: string): void {
  statusText.textContent = message;
  stateBadge.textContent = "Unavailable";
  stateBadge.classList.remove("active");
  disableButton.disabled = true;
}

function setBusy(busy: boolean, message?: string): void {
  enableButton.disabled = busy;
  presetSelect.disabled = busy;
  if (message) statusText.textContent = message;
}

function selectedPreset(): PresetName {
  return isPresetName(presetSelect.value) ? presetSelect.value : "normal";
}

function getElement<T extends HTMLElement>(id: string): T {
  const element = document.getElementById(id);
  if (!element) throw new Error(`Missing required element: #${id}`);
  return element as T;
}

void initialize();
