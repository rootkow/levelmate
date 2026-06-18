const MIN_LINEAR_VALUE = 1e-8;

export function calculateRms(samples: Float32Array): number {
  if (samples.length === 0) return 0;

  let sumOfSquares = 0;
  for (const sample of samples) sumOfSquares += sample * sample;
  return Math.sqrt(sumOfSquares / samples.length);
}

export function rmsToDb(rms: number): number {
  if (rms <= 0) return Number.NEGATIVE_INFINITY;
  return 20 * Math.log10(Math.max(rms, MIN_LINEAR_VALUE));
}

export function dbToLinear(db: number): number {
  return 10 ** (db / 20);
}

export function clamp(value: number, minimum: number, maximum: number): number {
  return Math.min(maximum, Math.max(minimum, value));
}
