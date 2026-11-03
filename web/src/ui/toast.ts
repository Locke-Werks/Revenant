// Sentences that come and go (a refused command, a removal) and notes that
// stay until whatever they describe stops being true.

export const TOAST_MS = 5_000;
export const MAX_TOASTS = 3;

export class Toasts {
  constructor(
    private readonly el: HTMLElement,
    private readonly setTimeout: (fn: () => void, ms: number) => unknown = (fn, ms) => window.setTimeout(fn, ms),
  ) {}

  show(text: string): void {
    const t = document.createElement("div");
    t.className = "toast";
    t.textContent = text;
    this.el.append(t);
    while (this.el.childElementCount > MAX_TOASTS) this.el.firstElementChild?.remove();
    this.setTimeout(() => t.remove(), TOAST_MS);
  }
}

// One line per key; null removes it. Each carries a close button, since a
// note the user has read should not have to wait for its condition to clear.
export class Notes {
  private items = new Map<string, HTMLElement>();

  constructor(private readonly el: HTMLElement) {}

  set(key: string, text: string | null): void {
    const was = this.items.get(key);
    if (text === null) {
      was?.remove();
      this.items.delete(key);
      return;
    }
    if (was) {
      was.querySelector(".note-text")!.textContent = text;
      return;
    }
    const n = document.createElement("div");
    n.className = "note";
    const span = document.createElement("span");
    span.className = "note-text";
    span.textContent = text;
    const close = document.createElement("button");
    close.type = "button";
    close.className = "note-close";
    close.setAttribute("aria-label", "Dismiss");
    close.textContent = "×";
    close.addEventListener("click", () => this.set(key, null));
    n.append(span, close);
    this.el.append(n);
    this.items.set(key, n);
  }
}
