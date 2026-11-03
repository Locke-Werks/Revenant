// What passes between LiveAudio on the main thread and the worklet. Its own
// module so the app bundle can name these without pulling in worklet.ts,
// whose registerProcessor call exists only in the worklet scope.

export const PROCESSOR_NAME = "revenant-live";
export const WORKLET_URL = "/worklet.js";

export type ToWorklet = { t: "push"; seq: number; pcm: Float32Array } | { t: "reset" };

// Sent when playback starts or stops, and about once a second besides.
export interface WorkletState {
  t: "state";
  playing: boolean;
  targetMs: number;
  bufferedMs: number;
  underruns: number;
  late: number;
  concealed: number;
  resets: number;
  trimmed: number;
}
