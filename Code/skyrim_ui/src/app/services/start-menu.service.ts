import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { BehaviorSubject } from 'rxjs';

export interface StartMenuList<T> {
  entries: T[];
  /** The game's selection, -1 for none. */
  selected: number;
}

export interface StartMenuEntry {
  text: string;
  /** The game's own number for it (main list only). */
  index?: number;
  disabled?: boolean;
}

/** A settings option: 0 a slider, 1 a stepper over its options, 2 a checkbox (value 0 or 1). */
export interface StartMenuOption {
  text: string;
  type: 0 | 1 | 2;
  value: number;
  id: number;
  options: string[];
  min: number;
  max: number;
  step: number;
}

export interface StartMenuControl {
  text: string;
  key: string;
  button: string;
}

export interface StartMenuSave {
  text: string;
  playTime: string;
}

/**
 * The game's own start menu (MainMenu.swf) as StartMenuBridge mirrors it: its screen (the movie's state) and that
 * screen's list. Only the parts of the screen shown are there.
 */
export interface StartMenuState {
  menu: 'pause' | 'main';
  state: string;
  main?: StartMenuList<StartMenuEntry>;
  categories?: StartMenuList<StartMenuEntry>;
  options?: StartMenuList<StartMenuOption>;
  guide?: string;
  controls?: StartMenuList<StartMenuControl>;
  remapping?: boolean;
  /** The game's word on the key just pressed, for a moment ("That button is reserved."). */
  remapMessage?: string;
  help?: StartMenuList<StartMenuEntry>;
  helpTitle?: string;
  helpText?: string;
  saves?: StartMenuList<StartMenuSave>;
  saving?: boolean;
  /** Settings screens: an option changed since the settings were last saved. */
  settingsChanged?: boolean;
  /** The game is writing the settings ("Saving..."): back waits until it's done. */
  savingSettings?: boolean;
  saveInfo?: { player: string; time: string; location: string; level: string };
  characters?: StartMenuList<StartMenuEntry>;
  /** The game's "are you sure?" text, on its confirmation screens. */
  confirm?: string;
  /** The confirmation's answers, as the game's buttons: quitting from the pause menu has MAIN MENU and DESKTOP. */
  buttons?: StartMenuButton[];
}

export interface StartMenuButton {
  id: 'accept' | 'desktop' | 'cancel';
  text: string;
}

export type StartMenuListName = 'main' | 'category' | 'option' | 'control' | 'help' | 'save' | 'character';

/**
 * Fallout 4: the pause menu and the main menu's screens past its main list, drawn by the overlay over the game's own
 * menu (hidden, it keeps running them). The game sends its state (startMenu events) as it changes; the overlay's
 * choices go back as the movie's own input: press or select a row, change an option, Accept, Cancel, X, Y.
 */
@Injectable({
  providedIn: 'root',
})
export class StartMenuService implements OnDestroy {
  readonly state$ = new BehaviorSubject<StartMenuState | null>(null);

  constructor(private readonly zone: NgZone) {
    skyrimtogether.on('startMenu', this.onState.bind(this));
  }

  ngOnDestroy(): void {
    skyrimtogether.off('startMenu');
  }

  press(list: StartMenuListName, row: number): void {
    this.send('press', `${list}:${row}`);
  }

  select(list: StartMenuListName, row: number): void {
    this.send('select', `${list}:${row}`);
  }

  setOption(row: number, value: number): void {
    this.send('set', `${row}:${value}`);
  }

  /** The movie's own buttons: Accept, Cancel (back), XButton (delete a save), YButton (defaults). */
  event(name: 'Accept' | 'Cancel' | 'XButton' | 'YButton'): void {
    this.send('event', name);
  }

  /** A confirmation's answer: confirm (MAIN MENU when quitting), or quit to the desktop. */
  button(id: 'accept' | 'desktop'): void {
    this.send('button', id);
  }

  /** Saves the settings now, as the game does when they're left: it goes back to the pause menu once they're written. */
  save(): void {
    this.send('save', '');
  }

  private send(action: string, payload: string): void {
    if (typeof skyrimtogether.mainMenuAction === 'function') {
      skyrimtogether.mainMenuAction(`startMenu.${action}`, payload);
    }
  }

  private onState(json: string): void {
    let state: StartMenuState | null = null;
    try {
      state = JSON.parse(json) as StartMenuState | null;
    } catch {
      state = null;
    }
    this.zone.run(() => this.state$.next(state));
  }
}
