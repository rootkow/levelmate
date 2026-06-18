import type { PresetName } from "../audio/presets";

export type LevelMateCommand =
  | { readonly type: "get-status" }
  | { readonly type: "enable"; readonly preset: PresetName }
  | { readonly type: "disable" }
  | { readonly type: "set-preset"; readonly preset: PresetName };

export interface LevelMateStatus {
  readonly ok: boolean;
  readonly attached: boolean;
  readonly active: boolean;
  readonly preset: PresetName;
  readonly message: string;
}
