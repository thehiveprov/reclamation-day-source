import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { BehaviorSubject } from 'rxjs';

/** A mod with MCM settings: its name, its pages ("" for its own page, config.json's "content"), why it can't be set. */
export interface ModConfigMod {
  name: string;
  pages: string[];
  error: string;
}

/**
 * One entry of the open page, as MCM's config.json has it, with the game's value. `i` is its place in the page (what
 * the overlay's choices name); entries hidden by their group aren't sent.
 */
export interface ModConfigItem {
  i: number;
  type: string;
  text: string;
  help: string;
  centered: boolean;
  big: boolean;
  /** A switcher's 0 or 1, a stepper's or dropdown's option, a slider's number. */
  value: number;
  valueString: string;
  options: string[];
  min: number;
  max: number;
  step: number;
  /** A hotkey's key (a Windows virtual-key code, 256 + n for mouse buttons; 0 for none) and Shift 1, Ctrl 2, Alt 4. */
  keycode: number;
  modifiers: number;
  allowModifiers: boolean;
}

/** A key another mod's keybind has: the player says whether to take it. */
export interface ModConfigConflict {
  item: number;
  keycode: number;
  modifiers: number;
  with: string;
}

export interface ModConfigState {
  mods: ModConfigMod[];
  mod: number;
  page: number;
  items: ModConfigItem[];
  conflict?: ModConfigConflict;
}

/**
 * Fallout 4: the Mod Configuration Menu's pages over the pause menu (ModConfigBridge in the game reads each mod's
 * config.json and sets its values through MCM itself). The game sends the mods and the open page (modConfig events);
 * the overlay's choices go back as mainMenuAction "modConfig.<action>".
 */
@Injectable({
  providedIn: 'root',
})
export class ModConfigService implements OnDestroy {
  readonly state$ = new BehaviorSubject<ModConfigState | null>(null);

  constructor(private readonly zone: NgZone) {
    skyrimtogether.on('modConfig', this.onState.bind(this));
  }

  ngOnDestroy(): void {
    skyrimtogether.off('modConfig');
  }

  open(): void {
    this.send('open', '');
  }

  close(): void {
    this.send('close', '');
  }

  page(mod: number, page: number): void {
    this.send('page', `${mod}:${page}`);
  }

  set(item: number, value: number): void {
    this.send('set', `${item}:${value}`);
  }

  text(item: number, text: string): void {
    this.send('text', `${item}:${text}`);
  }

  press(item: number): void {
    this.send('press', `${item}`);
  }

  /** A hotkey's new key, 0 for none. Taken from another mod's keybind only by forceKey. */
  key(item: number, keycode: number, modifiers: number, force = false): void {
    this.send(force ? 'forceKey' : 'key', `${item}:${keycode}:${modifiers}`);
  }

  cancelKey(): void {
    this.send('cancelKey', '');
  }

  private send(action: string, payload: string): void {
    if (typeof skyrimtogether.mainMenuAction === 'function') {
      skyrimtogether.mainMenuAction(`modConfig.${action}`, payload);
    }
  }

  private onState(json: string): void {
    let state: ModConfigState | null = null;
    try {
      state = JSON.parse(json) as ModConfigState | null;
    } catch {
      state = null;
    }
    this.zone.run(() => this.state$.next(state));
  }
}
