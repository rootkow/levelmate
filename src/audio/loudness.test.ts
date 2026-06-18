import { describe, expect, it } from "vitest";
import { calculateRms, clamp, dbToLinear, rmsToDb } from "./loudness";

describe("loudness utilities", () => {
  it("calculates RMS for a waveform", () => {
    expect(calculateRms(new Float32Array([1, -1, 1, -1]))).toBe(1);
    expect(calculateRms(new Float32Array([0.5, -0.5]))).toBe(0.5);
  });

  it("returns negative infinity for silence", () => {
    expect(rmsToDb(0)).toBe(Number.NEGATIVE_INFINITY);
  });

  it("converts between decibels and linear gain", () => {
    expect(rmsToDb(1)).toBeCloseTo(0);
    expect(rmsToDb(0.5)).toBeCloseTo(-6.0206, 3);
    expect(dbToLinear(6)).toBeCloseTo(1.9953, 3);
  });

  it("clamps gain correction to safe preset bounds", () => {
    expect(clamp(12, -10, 10)).toBe(10);
    expect(clamp(-12, -10, 10)).toBe(-10);
    expect(clamp(4, -10, 10)).toBe(4);
  });
});
