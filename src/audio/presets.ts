export type PresetName = "light" | "normal" | "night";

export interface LevelMatePreset {
  readonly label: string;
  readonly targetRmsDb: number;
  readonly maxGainDb: number;
  readonly minGainDb: number;
  readonly compressorRatio: number;
  readonly compressorThresholdDb: number;
}

export const PRESETS: Readonly<Record<PresetName, LevelMatePreset>> = Object.freeze({
  light: Object.freeze({
    label: "Light",
    targetRmsDb: -24,
    maxGainDb: 6,
    minGainDb: -6,
    compressorRatio: 2.5,
    compressorThresholdDb: -20,
  }),
  normal: Object.freeze({
    label: "Normal",
    targetRmsDb: -22,
    maxGainDb: 10,
    minGainDb: -10,
    compressorRatio: 4,
    compressorThresholdDb: -24,
  }),
  night: Object.freeze({
    label: "Night",
    targetRmsDb: -20,
    maxGainDb: 14,
    minGainDb: -14,
    compressorRatio: 6,
    compressorThresholdDb: -28,
  }),
});

export function isPresetName(value: unknown): value is PresetName {
  return typeof value === "string" && value in PRESETS;
}
