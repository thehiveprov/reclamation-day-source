import { Injectable, NgZone } from '@angular/core';
import { AssetService, AudioAsset, Kind } from './asset.service';

export enum Sound {
  /** Cancellation. */
  Cancel = 'cancel',

  /** Checkbox activation. */
  Check = 'check',

  /** Failure. */
  Fail = 'fail',

  /** Focus change. */
  Focus = 'focus',

  /** Success. */
  Success = 'success',

  /** Message reception. */
  Message = 'message',

  /** Affirmation. */
  Ok = 'ok',

  /** Checkbox deactivation. */
  Uncheck = 'uncheck',

  /** Player joined group. */
  PlayerJoined = 'playerjoined',

  /** Switching tabs. */
  PrevNext = 'prevnext',
}

/**
 * Fallout 4's own menu sounds (sound descriptors of Fallout4.esm), played by the game: the overlay sounds like the
 * game's menus.
 */
const GAME_SOUNDS: Record<Sound, string> = {
  [Sound.Cancel]: 'UIMenuCancel',
  [Sound.Check]: 'UIMenuOK',
  [Sound.Fail]: 'UIMenuCancel',
  [Sound.Focus]: 'UIMenuFocus',
  [Sound.Success]: 'UIMenuOK',
  [Sound.Message]: 'UIMenuPopUpGeneric',
  [Sound.Ok]: 'UIMenuOK',
  [Sound.Uncheck]: 'UIMenuOK',
  [Sound.PlayerJoined]: 'UIMenuPopUpGeneric',
  [Sound.PrevNext]: 'UIMenuPrevNext',
};

/** Without the game (the browser preview): the old sound files, named after the sounds. */
const FALLBACK_FILES: Partial<Record<Sound, string>> = {
  [Sound.PrevNext]: 'focus',
};

/** A button's own sound already played within this time: the generic one isn't played on top. */
const INTERACTION_WINDOW_MS = 100;

/** What sounds like a button when pressed or hovered. data-sound="cancel" | "prevnext" | "none" changes its sound. */
const BUTTON_SELECTOR = 'button, [role="button"]';

const BUTTON_SOUNDS: Record<string, Sound> = {
  cancel: Sound.Cancel,
  prevnext: Sound.PrevNext,
};

/** Sound player service. */
@Injectable({
  providedIn: 'root',
})
export class SoundService {
  /** Stored sound assets (only used without the game). */
  private readonly sounds = new Map<string, Symbol>();

  private lastPlayed = 0;

  /**
   * Instantiate.
   *
   * @param asset Asset service.
   */
  public constructor(
    private readonly asset: AssetService,
    zone: NgZone,
  ) {
    if (!this.hasGameSounds()) {
      for (const sound of Object.values(Sound)) {
        const file = FALLBACK_FILES[sound] ?? sound;
        this.sounds.set(sound, asset.load(Kind.Audio, `assets/sounds/${file}.wav`));
      }
    }

    // Every button sounds like the game's menus: pressed (OK, or its data-sound), hovered (focus). Outside Angular:
    // hovering mustn't run change detection.
    zone.runOutsideAngular(() => {
      document.addEventListener('click', event => this.onClick(event));
      document.addEventListener('mouseover', event => this.onMouseOver(event));
    });
  }

  /**
   * Play a sound.
   *
   * @param sound The sound to play.
   */
  public play(sound: string | Sound): void {
    this.lastPlayed = performance.now();
    this.output(sound);
  }

  /**
   * A button's sound (pressed or hovered), unless the button already played its own.
   *
   * @param sound The sound to play.
   */
  public playForInteraction(sound: Sound): void {
    if (performance.now() - this.lastPlayed >= INTERACTION_WINDOW_MS) {
      this.output(sound);
    }
  }

  private output(sound: string | Sound): void {
    if (this.hasGameSounds()) {
      const gameSound = GAME_SOUNDS[sound as Sound];
      if (gameSound) {
        skyrimtogether.playSound(gameSound);
      }
      return;
    }

    const asset = this.sounds.get(sound);
    if (asset) {
      this.asset.get<AudioAsset>(asset).subscribe(audio => audio.play());
    }
  }

  private onClick(event: MouseEvent): void {
    const button = this.findButton(event.target);
    const sound = button?.getAttribute('data-sound');
    if (button && sound !== 'none') {
      this.playForInteraction(BUTTON_SOUNDS[sound ?? ''] ?? Sound.Ok);
    }
  }

  private onMouseOver(event: MouseEvent): void {
    const button = this.findButton(event.target);
    // Only on entering a button, not moving over what's inside it
    if (button && button.getAttribute('data-sound') !== 'none' && this.findButton(event.relatedTarget) !== button) {
      this.playForInteraction(Sound.Focus);
    }
  }

  /** The enabled button an event happened on, if any. */
  private findButton(target: EventTarget | null): Element | null {
    const button = target instanceof Element ? target.closest(BUTTON_SELECTOR) : null;
    return button && !(button as HTMLButtonElement).disabled ? button : null;
  }

  private hasGameSounds(): boolean {
    return typeof skyrimtogether !== 'undefined' && typeof skyrimtogether.playSound === 'function';
  }
}
