// The full-screen notice for an engine this phone does not hold. The engine
// serves one user at a time: a phone that finds it in use is told by whom and
// since when and offered Take over; a phone someone took it from is told who
// and offered Take back. Nothing else of the app is usable meanwhile, so the
// notice covers all of it rather than leaving a partial UI underneath.

import type { JsonValue, LeaseEvent } from "../protocol";

export interface LeaseScreenText {
  title: string;
  detail: string;
  button: string;
}

// "2026-11-03T09:14:00-07:00" as the phone's local time: the clock time when
// it is today, the date and time when it is not. The gateway's text as it
// came when it does not parse.
export function formatSince(iso: string, now: Date = new Date()): string {
  const at = new Date(iso);
  if (iso === "" || Number.isNaN(at.getTime())) return iso;
  const time = at.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  const sameDay =
    at.getFullYear() === now.getFullYear() && at.getMonth() === now.getMonth() && at.getDate() === now.getDate();
  if (sameDay) return time;
  return `${at.toLocaleDateString([], { month: "short", day: "numeric" })} ${time}`;
}

// What the notice says, or null when the phone holds the engine.
export function leaseScreenText(e: LeaseEvent | null, now?: Date): LeaseScreenText | null {
  if (e === null || e.state === "held") return null;
  const holder = e.holder !== "" ? e.holder : "another client";
  if (e.state === "lost") {
    return { title: "Taken over", detail: `Taken over by ${holder}`, button: "Take back" };
  }
  const since = typeof e.since === "string" && e.since !== "" ? ` since ${formatSince(e.since, now)}` : "";
  return { title: "In use", detail: `Revenant is in use by ${holder}${since}`, button: "Take over" };
}

export interface LeaseLink {
  readonly lease: LeaseEvent | null;
  takeOver(): Promise<JsonValue>;
  on(name: "lease", fn: (e: LeaseEvent) => void): () => void;
}

export class LeaseScreen {
  private readonly title: HTMLElement;
  private readonly detail: HTMLElement;
  private readonly button: HTMLButtonElement;
  private readonly error: HTMLElement;
  private busy = false;

  constructor(
    private readonly el: HTMLElement,
    private readonly link: LeaseLink,
  ) {
    this.title = el.querySelector<HTMLElement>(".lease-title")!;
    this.detail = el.querySelector<HTMLElement>(".lease-detail")!;
    this.button = el.querySelector<HTMLButtonElement>(".lease-take")!;
    this.error = el.querySelector<HTMLElement>(".lease-error")!;
    this.button.addEventListener("click", () => this.take());
    link.on("lease", () => this.render());
    this.render();
  }

  private render(): void {
    const text = leaseScreenText(this.link.lease);
    this.el.hidden = text === null;
    if (text === null) {
      this.error.textContent = "";
      return;
    }
    this.title.textContent = text.title;
    this.detail.textContent = text.detail;
    this.button.textContent = text.button;
    this.button.disabled = this.busy;
  }

  private take(): void {
    if (this.busy) return;
    this.busy = true;
    this.error.textContent = "";
    this.render();
    this.link.takeOver().then(
      () => {
        this.busy = false;
        this.render();
      },
      (e: unknown) => {
        this.busy = false;
        this.error.textContent = e instanceof Error ? e.message : String(e);
        this.render();
      },
    );
  }
}

export function leaseScreenHtml(): string {
  return `
<section class="lease" id="lease" role="alertdialog" aria-labelledby="lease-title" hidden>
  <h1 class="lease-title" id="lease-title"></h1>
  <p class="lease-detail"></p>
  <button type="button" class="lease-take"></button>
  <p class="lease-error" role="status"></p>
</section>`;
}
