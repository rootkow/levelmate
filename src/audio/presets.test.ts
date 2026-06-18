import { describe, expect, it } from "vitest";
import { PRESETS, isPresetName } from "./presets";

describe("LevelMate presets", () => {
  it("defines the expected light preset", () => {
    expect(PRESETS.light).toMatchObject({
      targetRmsDb: -24,
      maxGainDb: 6,
      minGainDb: -6,
      compressorRatio: 2.5,
      compressorThresholdDb: -20,
    });
  });

  it("defines the expected normal and night gain ranges", () => {
    expect(PRESETS.normal).toMatchObject({ targetRmsDb: -22, maxGainDb: 10, minGainDb: -10 });
    expect(PRESETS.night).toMatchObject({ targetRmsDb: -20, maxGainDb: 14, minGainDb: -14 });
  });

  it("recognizes only supported preset names", () => {
    expect(isPresetName("normal")).toBe(true);
    expect(isPresetName("off")).toBe(false);
    expect(isPresetName(null)).toBe(false);
  });
});
