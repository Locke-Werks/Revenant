// Engine refusals are written for the developer reading a log: several
// sentences, offsets relative to the tuned centre, the API to call instead.
// On a phone one short sentence says what happened; the raw text goes to the
// console for whoever is debugging.

const KNOWN: [RegExp, string][] = [
  [/outside the source's (baseband )?span|outside the source's band/i, "Outside the radio's range"],
  [/no source|source is not open|no radio/i, "No radio is open"],
  [/timed out|timeout/i, "The engine did not answer in time"],
  [/not connected|socket (is )?(closed|not ready)|link (is )?down/i, "Not connected to the engine"],
  [/unknown vrx|no such receiver|not owned/i, "That receiver is gone"],
];

// Past this a message is cut to its first sentence.
const MAX_CHARS = 90;

export function shortError(raw: string): string {
  for (const [re, text] of KNOWN) if (re.test(raw)) return text;
  const line = raw.split("\n")[0]!.trim();
  if (line.length <= MAX_CHARS) return line;
  const stop = line.indexOf(". ");
  if (stop > 0 && stop < MAX_CHARS) return line.slice(0, stop + 1);
  return `${line.slice(0, MAX_CHARS - 1).trimEnd()}...`;
}

// The same sentence again inside this window is not news.
export const REPEAT_MS = 3_000;

export class ToastGate {
  private seen = new Map<string, number>();

  admit(text: string, now: number): boolean {
    for (const [t, at] of this.seen) if (now - at >= REPEAT_MS) this.seen.delete(t);
    if (this.seen.has(text)) return false;
    this.seen.set(text, now);
    return true;
  }
}
