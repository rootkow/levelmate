import { LevelMateProcessor } from "../../audio/LevelMateProcessor";
import { type PresetName } from "../../audio/presets";
import type { LevelMateCommand, LevelMateStatus } from "../messages";

declare global {
  interface Window {
    __levelMateContentLoaded?: boolean;
  }
}

const FAILURE_MESSAGE =
  "This site’s video/audio could not be processed. DRM or cross-origin restrictions may prevent LevelMate from accessing the audio.";

if (!window.__levelMateContentLoaded) {
  window.__levelMateContentLoaded = true;

  let processor: LevelMateProcessor | undefined;
  let preset: PresetName = "normal";
  let lastMessage = "LevelMate is ready to try this page.";

  browser.runtime.onMessage.addListener((command: LevelMateCommand) => {
    return handleCommand(command);
  });

  async function handleCommand(command: LevelMateCommand): Promise<LevelMateStatus> {
    switch (command.type) {
      case "enable":
        preset = command.preset;
        return enableProcessing();
      case "disable":
        processor?.setBypassed(true);
        lastMessage = processor ? "Leveling disabled. Audio is using the direct path." : "LevelMate is not attached.";
        return currentStatus(true);
      case "set-preset":
        preset = command.preset;
        processor?.setPreset(preset);
        lastMessage = processor ? "Preset updated." : "Preset saved. Enable LevelMate to attach.";
        return currentStatus(true);
      case "get-status":
        return currentStatus(true);
    }
  }

  async function enableProcessing(): Promise<LevelMateStatus> {
    if (processor) {
      processor.setPreset(preset);
      processor.setBypassed(false);
      lastMessage = "Leveling is active on this video.";
      return currentStatus(true);
    }

    const video = findPlayableVideo();
    if (!video) {
      lastMessage = "No playable HTML5 video was found on this page.";
      return currentStatus(false);
    }

    try {
      processor = await LevelMateProcessor.attach(video, preset);
      lastMessage = "Leveling is active on this video.";
      return currentStatus(true);
    } catch (error) {
      console.warn("LevelMate could not attach to this video.", error);
      lastMessage = FAILURE_MESSAGE;
      return currentStatus(false);
    }
  }

  function findPlayableVideo(): HTMLVideoElement | undefined {
    return [...document.querySelectorAll("video")].find((video) => {
      const hasSource = Boolean(video.currentSrc || video.src || video.querySelector("source[src]"));
      return hasSource && !video.ended;
    });
  }

  function currentStatus(ok: boolean): LevelMateStatus {
    return {
      ok,
      attached: Boolean(processor),
      active: processor?.metrics.active ?? false,
      preset,
      message: lastMessage,
    };
  }
}
