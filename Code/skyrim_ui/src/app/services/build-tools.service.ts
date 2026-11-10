import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { BehaviorSubject } from 'rxjs';

/** One build tools action and the key it's on (Windows virtual key codes). */
export interface BuildToolsKey {
  action: string;
  key: number;
  defaultKey: number;
}

/** The build tools as the game has them (config\BuildTools.ini). */
export interface BuildToolsState {
  placeAnywhere: boolean;
  scrapAnything: boolean;
  keys: BuildToolsKey[];
  /** Why the last change was refused: "reserved", "used:<action>", or empty. */
  error: string;
}

const keyNames: Record<number, string> = {
  0x08: 'Backspace',
  0x13: 'Pause',
  0x21: 'Page Up',
  0x22: 'Page Down',
  0x23: 'End',
  0x24: 'Home',
  0x2d: 'Insert',
  0x2e: 'Delete',
  0x6a: 'Num *',
  0x6b: 'Num +',
  0x6d: 'Num -',
  0x6e: 'Num .',
  0x6f: 'Num /',
  0x91: 'Scroll Lock',
  0xba: ';',
  0xbb: '=',
  0xbc: ',',
  0xbd: '-',
  0xbe: '.',
  0xbf: '/',
  0xc0: '`',
  0xdb: '[',
  0xdc: '\\',
  0xdd: ']',
  0xde: "'",
};

/** Readable name of a Windows virtual key code. */
export function keyName(key: number): string {
  if (key >= 0x60 && key <= 0x69) {
    return `Num ${key - 0x60}`;
  }
  if (key >= 0x70 && key <= 0x87) {
    return `F${key - 0x6f}`;
  }
  if ((key >= 0x30 && key <= 0x39) || (key >= 0x41 && key <= 0x5a)) {
    return String.fromCharCode(key);
  }
  return keyNames[key] ?? `Key ${key}`;
}

/**
 * Build tools (Fallout 4 build mode: place anywhere, scrap anything, precise controls): the game sends its state, the
 * settings change it through buildTools(request, payload).
 */
@Injectable({
  providedIn: 'root',
})
export class BuildToolsService implements OnDestroy {
  readonly state$ = new BehaviorSubject<BuildToolsState | null>(null);

  constructor(private readonly zone: NgZone) {
    skyrimtogether.on('buildToolsState', this.onState.bind(this));
    this.request('get');
  }

  ngOnDestroy(): void {
    skyrimtogether.off('buildToolsState');
  }

  bind(action: string, key: number): void {
    this.request('bind', `${action}:${key}`);
  }

  set(setting: 'placeAnywhere' | 'scrapAnything', on: boolean): void {
    this.request('set', `${setting}:${on ? 1 : 0}`);
  }

  resetKeys(): void {
    this.request('reset');
  }

  private request(request: string, payload = ''): void {
    // The game's older builds don't have it
    if (typeof skyrimtogether.buildTools === 'function') {
      skyrimtogether.buildTools(request, payload);
    }
  }

  private onState(json: string): void {
    this.zone.run(() => this.state$.next(JSON.parse(json) as BuildToolsState));
  }
}
