import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { BehaviorSubject } from 'rxjs';

/** One entry of the open page, with its value from the game. `i` is its place in the page (what choices name). */
export interface WorldSettingsItem {
  i: number;
  /** "section", "text", "switcher", "dropdown", "slider" or "button" */
  type: string;
  text: string;
  help: string;
  /** A switcher's 0 or 1, a dropdown's option, a slider's number. */
  value: number;
  options: string[];
  min: number;
  max: number;
  step: number;
}

export interface WorldSettingsState {
  pages: string[];
  page: number;
  /** Why the page can't be used (NAC X not loaded), empty otherwise. */
  error: string;
  items: WorldSettingsItem[];
}

/**
 * Fallout 4: the F2 overlay's WORLD SETTINGS, the host's weather and NAC X's settings (WorldSettings in the game reads
 * the page and sets the values). The game sends the open page as the worldSettings event; the choices go back as
 * mainMenuAction "worldSettings.<action>".
 */
@Injectable({
  providedIn: 'root',
})
export class WorldSettingsService implements OnDestroy {
  readonly state$ = new BehaviorSubject<WorldSettingsState | null>(null);

  constructor(private readonly zone: NgZone) {
    skyrimtogether.on('worldSettings', this.onState.bind(this));
  }

  ngOnDestroy(): void {
    skyrimtogether.off('worldSettings');
  }

  open(): void {
    this.send('open', '');
  }

  close(): void {
    this.send('close', '');
  }

  page(page: number): void {
    this.send('page', `${page}`);
  }

  set(item: number, value: number): void {
    this.send('set', `${item}:${value}`);
  }

  press(item: number): void {
    this.send('press', `${item}`);
  }

  private send(action: string, payload: string): void {
    if (typeof skyrimtogether.mainMenuAction === 'function') {
      skyrimtogether.mainMenuAction(`worldSettings.${action}`, payload);
    }
  }

  private onState(json: string): void {
    let state: WorldSettingsState | null = null;
    try {
      state = JSON.parse(json) as WorldSettingsState | null;
    } catch {
      state = null;
    }
    this.zone.run(() => this.state$.next(state));
  }
}
