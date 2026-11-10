import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { BehaviorSubject, Subscription } from 'rxjs';
import { ClientService } from './client.service';

/** An emote on the wheel: its name (its chat command) and what the wheel shows. */
export interface EmoteWheelEntry {
  name: string;
  label: string;
}

/** The emote wheel while its key is held: the key's name and the eight, from the top going round to the right. */
export interface EmoteWheel {
  key: string;
  emotes: EmoteWheelEntry[];
}

/** Every emote's chat command, as the game has them (EmoteService); "/emotes" lists them in the chat. */
export const EMOTE_COMMANDS = [
  'salute',
  'bossalute',
  'point',
  'pointleft',
  'pointright',
  'pointback',
  'yes',
  'no',
  'clap',
  'cheer',
  'boo',
  'think',
  'lookaround',
  'offer',
  'victory',
  'sit',
  'kneel',
  'pray',
  'surrender',
  'bird',
  'handsup',
];

/**
 * Emotes (Fallout 4): the wheel the game opens while its key (G) is held, and the chat's emote commands. The game plays
 * the emote under the pointer when the key is let go (hover tells it which); a click plays one at once. Escape closes
 * the overlay, and the wheel with it.
 */
@Injectable({
  providedIn: 'root',
})
export class EmotesService implements OnDestroy {
  readonly wheel$ = new BehaviorSubject<EmoteWheel | null>(null);

  private readonly activation: Subscription;

  constructor(
    private readonly zone: NgZone,
    private readonly client: ClientService,
  ) {
    skyrimtogether.on('emoteWheel', this.onWheel.bind(this));
    this.activation = this.client.activationStateChange.subscribe(active => {
      if (!active && this.wheel$.value) {
        this.wheel$.next(null);
        this.request('close');
      }
    });
  }

  ngOnDestroy(): void {
    skyrimtogether.off('emoteWheel');
    this.activation.unsubscribe();
  }

  /** The emote under the pointer ("" none): the one the game plays when the key is let go. */
  hover(name: string): void {
    this.request('hover', name);
  }

  /** Plays one now (a click on the wheel). */
  play(name: string): void {
    this.wheel$.next(null);
    this.request('play', name);
  }

  /** A chat command: an emote's name, or "emotes" for the list. */
  command(name: string): void {
    this.request('command', name);
  }

  private request(request: string, payload = ''): void {
    // The game's older builds don't have it
    if (typeof skyrimtogether.emotes === 'function') {
      skyrimtogether.emotes(request, payload);
    }
  }

  private onWheel(json: string): void {
    this.zone.run(() => this.wheel$.next(JSON.parse(json) as EmoteWheel | null));
  }
}
