import { calculateRms, clamp, dbToLinear, rmsToDb } from "./loudness";
import { PRESETS, type PresetName } from "./presets";

const SAMPLE_INTERVAL_MS = 100;
const SILENCE_FLOOR_DB = -60;
const GAIN_SMOOTHING_FACTOR = 0.18;
const GAIN_TIME_CONSTANT_SECONDS = 0.25;
const BYPASS_RAMP_SECONDS = 0.03;

export interface LevelMateMetrics {
  readonly rmsDb: number;
  readonly appliedGainDb: number;
  readonly preset: PresetName;
  readonly active: boolean;
  readonly bypassed: boolean;
}

export type MetricsListener = (metrics: LevelMateMetrics) => void;

export class LevelMateProcessor {
  readonly #context: AudioContext;
  readonly #source: MediaElementAudioSourceNode;
  readonly #inputGain: GainNode;
  readonly #analyser: AnalyserNode;
  readonly #automaticGain: GainNode;
  readonly #compressor: DynamicsCompressorNode;
  readonly #outputGain: GainNode;
  readonly #processedPathGain: GainNode;
  readonly #directPathGain: GainNode;
  readonly #samples: Float32Array<ArrayBuffer>;
  readonly #listeners = new Set<MetricsListener>();

  #preset: PresetName;
  #bypassed = false;
  #appliedGainDb = 0;
  #rmsDb = Number.NEGATIVE_INFINITY;
  #timer: number | undefined;
  #disposed = false;

  private constructor(
    context: AudioContext,
    source: MediaElementAudioSourceNode,
    preset: PresetName,
  ) {
    this.#context = context;
    this.#source = source;
    this.#inputGain = this.#context.createGain();
    this.#analyser = this.#context.createAnalyser();
    this.#automaticGain = this.#context.createGain();
    this.#compressor = this.#context.createDynamicsCompressor();
    this.#outputGain = this.#context.createGain();
    this.#processedPathGain = this.#context.createGain();
    this.#directPathGain = this.#context.createGain();
    this.#samples = new Float32Array(2048);
    this.#preset = preset;

    this.#analyser.fftSize = 2048;
    this.#analyser.smoothingTimeConstant = 0;
    this.#compressor.knee.value = 20;
    this.#compressor.attack.value = 0.01;
    this.#compressor.release.value = 0.25;

    // The direct and processed branches enable an immediate, click-free A/B bypass.
    this.#source.connect(this.#directPathGain).connect(this.#context.destination);
    this.#source
      .connect(this.#inputGain)
      .connect(this.#analyser)
      .connect(this.#automaticGain)
      .connect(this.#compressor)
      .connect(this.#outputGain)
      .connect(this.#processedPathGain)
      .connect(this.#context.destination);

    this.#directPathGain.gain.value = 0;
    this.#processedPathGain.gain.value = 1;
    this.#applyPreset();
  }

  static async attach(media: HTMLMediaElement, preset: PresetName): Promise<LevelMateProcessor> {
    const context = new AudioContext();
    let processor: LevelMateProcessor | undefined;
    try {
      const source = context.createMediaElementSource(media);
      processor = new LevelMateProcessor(context, source, preset);
      await processor.#context.resume();
      processor.#startMetering();
      return processor;
    } catch (error) {
      if (processor) await processor.dispose();
      else await context.close();
      throw error;
    }
  }

  get metrics(): LevelMateMetrics {
    return {
      rmsDb: this.#rmsDb,
      appliedGainDb: this.#appliedGainDb,
      preset: this.#preset,
      active: !this.#bypassed && !this.#disposed,
      bypassed: this.#bypassed,
    };
  }

  subscribe(listener: MetricsListener): () => void {
    this.#listeners.add(listener);
    listener(this.metrics);
    return () => this.#listeners.delete(listener);
  }

  setPreset(preset: PresetName): void {
    this.#assertUsable();
    this.#preset = preset;
    this.#applyPreset();
    this.#emitMetrics();
  }

  setBypassed(bypassed: boolean): void {
    this.#assertUsable();
    this.#bypassed = bypassed;

    const now = this.#context.currentTime;
    this.#rampGain(this.#directPathGain.gain, bypassed ? 1 : 0, now);
    this.#rampGain(this.#processedPathGain.gain, bypassed ? 0 : 1, now);
    this.#emitMetrics();
  }

  async dispose(): Promise<void> {
    if (this.#disposed) return;
    this.#disposed = true;
    if (this.#timer !== undefined) window.clearInterval(this.#timer);
    this.#listeners.clear();

    this.#source.disconnect();
    this.#inputGain.disconnect();
    this.#analyser.disconnect();
    this.#automaticGain.disconnect();
    this.#compressor.disconnect();
    this.#outputGain.disconnect();
    this.#processedPathGain.disconnect();
    this.#directPathGain.disconnect();
    await this.#context.close();
  }

  #startMetering(): void {
    this.#sampleAndAdjust();
    this.#timer = window.setInterval(() => this.#sampleAndAdjust(), SAMPLE_INTERVAL_MS);
  }

  #sampleAndAdjust(): void {
    if (this.#disposed) return;
    this.#analyser.getFloatTimeDomainData(this.#samples);
    this.#rmsDb = rmsToDb(calculateRms(this.#samples));

    const preset = PRESETS[this.#preset];
    const desiredGainDb =
      Number.isFinite(this.#rmsDb) && this.#rmsDb >= SILENCE_FLOOR_DB
        ? clamp(preset.targetRmsDb - this.#rmsDb, preset.minGainDb, preset.maxGainDb)
        : 0;

    // Smooth the control signal before scheduling the AudioParam transition.
    this.#appliedGainDb += (desiredGainDb - this.#appliedGainDb) * GAIN_SMOOTHING_FACTOR;
    const now = this.#context.currentTime;
    this.#automaticGain.gain.setTargetAtTime(
      dbToLinear(this.#appliedGainDb),
      now,
      GAIN_TIME_CONSTANT_SECONDS,
    );
    this.#emitMetrics();
  }

  #applyPreset(): void {
    const preset = PRESETS[this.#preset];
    this.#compressor.threshold.value = preset.compressorThresholdDb;
    this.#compressor.ratio.value = preset.compressorRatio;
  }

  #rampGain(parameter: AudioParam, target: number, now: number): void {
    parameter.cancelScheduledValues(now);
    parameter.setValueAtTime(parameter.value, now);
    parameter.linearRampToValueAtTime(target, now + BYPASS_RAMP_SECONDS);
  }

  #emitMetrics(): void {
    const metrics = this.metrics;
    for (const listener of this.#listeners) listener(metrics);
  }

  #assertUsable(): void {
    if (this.#disposed) throw new Error("This LevelMate processor has been disposed.");
  }
}
