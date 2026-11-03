// The DV tab: Auto DV, trunk following on a P25 control channel, and the two
// call tables. Everything it shows comes from dv.ts; this only draws it.

import {
  callCells,
  followTarget,
  groupCells,
  newestFirst,
  pickCall,
  type CallRow,
  type DvState,
  type TrunkSummary,
  NO_DV,
} from "../dv";

export interface DvPanelDeps {
  setAutoDv(on: boolean): void;
  setFollow(vrx: number, on: boolean, maxFollowers: number): void;
  receivers(): readonly { vrx: number }[];
  select(vrx: number): void;
  // The follower count this phone last chose, over the desktop's.
  savedMaxFollowers(): number | null;
}

// Rows drawn per table. The gateway sends up to 100; a screen holds fewer.
const SHOWN_ROWS = 60;

export class DvPanel {
  readonly el: HTMLElement;
  // Mounted by the app into the Tune tab; see the constructor.
  readonly autoBtn: HTMLButtonElement;
  private readonly followRow: HTMLElement;
  private readonly followBtn: HTMLButtonElement;
  private readonly maxSel: HTMLSelectElement;
  private readonly trunkLine: HTMLElement;
  private readonly callsBody: HTMLElement;
  private readonly groupsBody: HTMLElement;
  private readonly callsEmpty: HTMLElement;
  private dv: DvState = NO_DV;
  private active: { vrx: number; mode: string } | null = null;
  private calls: CallRow[] = [];
  private groups: CallRow[] = [];
  private visible = false;

  constructor(private readonly deps: DvPanelDeps) {
    this.el = document.createElement("section");
    this.el.className = "panel dv";
    // Auto DV lives in the Tune tab, not here: this tab only shows once a DV
    // receiver exists, and Auto DV is what creates them (owner, 2026-11-03).
    this.autoBtn = document.createElement("button");
    this.autoBtn.type = "button";
    this.autoBtn.className = "btn toggle";
    this.autoBtn.id = "dv-auto";
    this.autoBtn.setAttribute("aria-pressed", "false");
    this.autoBtn.textContent = "Auto DV";
    this.el.innerHTML = `
<div class="row" id="dv-follow-row" hidden>
  <button type="button" class="btn toggle" id="dv-follow" aria-pressed="false">Follow trunk</button>
  <label for="dv-max">Followers</label>
  <select id="dv-max"></select>
</div>
<p class="panel-status" id="dv-trunk"></p>
<h2>Calls</h2>
<p class="hint" id="dv-calls-empty">No calls heard yet.</p>
<table class="dv-table"><thead><tr><th>Time</th><th>Talkgroup</th><th>Radio</th><th>Rx</th><th>State</th></tr></thead>
<tbody id="dv-calls"></tbody></table>
<h2>Talkgroups</h2>
<table class="dv-table"><thead><tr><th>Last</th><th>Talkgroup</th><th>Radio</th><th>Calls</th><th>State</th></tr></thead>
<tbody id="dv-groups"></tbody></table>`;
    this.followRow = this.el.querySelector("#dv-follow-row")!;
    this.followBtn = this.el.querySelector("#dv-follow")!;
    this.maxSel = this.el.querySelector("#dv-max")!;
    this.trunkLine = this.el.querySelector("#dv-trunk")!;
    this.callsBody = this.el.querySelector("#dv-calls")!;
    this.groupsBody = this.el.querySelector("#dv-groups")!;
    this.callsEmpty = this.el.querySelector("#dv-calls-empty")!;
    for (let n = 1; n <= 16; n++) {
      const o = document.createElement("option");
      o.value = String(n);
      o.textContent = String(n);
      this.maxSel.append(o);
    }
    this.autoBtn.addEventListener("click", () => deps.setAutoDv(!this.dv.autoDv));
    this.followBtn.addEventListener("click", () => {
      const vrx = followTarget(this.active, this.dv);
      if (vrx !== null) deps.setFollow(vrx, this.dv.follow === null, Number(this.maxSel.value));
    });
    this.maxSel.addEventListener("change", () => {
      if (this.dv.follow) deps.setFollow(this.dv.follow.vrx, true, Number(this.maxSel.value));
    });
    this.callsBody.addEventListener("click", (ev) => {
      const tr = (ev.target as HTMLElement).closest<HTMLElement>("tr[data-id]");
      const row = tr ? this.calls.find((r) => String(r.id) === tr.dataset.id) : undefined;
      if (row) pickCall(row, deps.receivers(), (vrx) => deps.select(vrx));
    });
    this.renderSwitches();
  }

  setDv(dv: DvState): void {
    this.dv = dv;
    this.renderSwitches();
  }

  setActive(r: { vrx: number; mode: string } | null): void {
    this.active = r;
    this.renderSwitches();
  }

  setTrunk(t: TrunkSummary): void {
    this.trunkLine.textContent = t.text;
  }

  setCalls(rows: CallRow[]): void {
    this.calls = newestFirst(rows);
    if (this.visible) this.renderCalls();
  }

  setGroups(rows: CallRow[]): void {
    this.groups = newestFirst(rows);
    if (this.visible) this.renderGroups();
  }

  setVisible(visible: boolean): void {
    this.visible = visible;
    if (visible) {
      this.renderCalls();
      this.renderGroups();
    }
  }

  private renderSwitches(): void {
    this.autoBtn.setAttribute("aria-pressed", String(this.dv.autoDv));
    this.autoBtn.title = "A receiver for every digital voice signal detected; trunk following is held off while it is on, as on the desktop";
    const target = followTarget(this.active, this.dv);
    this.followRow.hidden = target === null;
    this.followBtn.setAttribute("aria-pressed", String(this.dv.follow !== null));
    this.followBtn.textContent = target === null ? "Follow trunk" : `Follow trunk on #${target}`;
    if (document.activeElement !== this.maxSel) {
      this.maxSel.value = String(this.dv.follow?.maxFollowers ?? this.deps.savedMaxFollowers() ?? this.dv.maxFollowers);
    }
  }

  private cell(tr: HTMLElement, text: string, cls = ""): void {
    const td = document.createElement("td");
    td.textContent = text;
    if (cls) td.className = cls;
    tr.append(td);
  }

  private renderCalls(): void {
    this.callsEmpty.hidden = this.calls.length > 0;
    const out: HTMLElement[] = [];
    for (const r of this.calls.slice(0, SHOWN_ROWS)) {
      const c = callCells(r);
      const tr = document.createElement("tr");
      tr.dataset.id = String(r.id);
      tr.className = r.state;
      this.cell(tr, c.time);
      this.cell(tr, c.talkgroup + (c.emergency ? " !" : ""));
      this.cell(tr, c.radio);
      this.cell(tr, c.receiver);
      this.cell(tr, c.state + (c.encrypted ? " enc" : ""), c.encrypted ? "enc" : "");
      out.push(tr);
    }
    this.callsBody.replaceChildren(...out);
  }

  private renderGroups(): void {
    const out: HTMLElement[] = [];
    for (const r of this.groups.slice(0, SHOWN_ROWS)) {
      const c = groupCells(r);
      const tr = document.createElement("tr");
      tr.className = r.state;
      this.cell(tr, c.time);
      this.cell(tr, c.talkgroup);
      this.cell(tr, c.radio);
      this.cell(tr, c.calls);
      this.cell(tr, c.state + (c.encrypted ? " enc" : ""), c.encrypted ? "enc" : "");
      out.push(tr);
    }
    this.groupsBody.replaceChildren(...out);
  }
}
