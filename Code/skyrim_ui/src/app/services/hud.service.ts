import { Injectable, OnDestroy } from '@angular/core';
import { BehaviorSubject } from 'rxjs';

/**
 * A compass marker as the game made it: kind (1 quest, 2 quest door, 3 the player's own, 4 enemy, 5/7/8/9 location,
 * 6 targeted enemy, 10 recon), heading, distance, height (quest kinds: 1 below, 2 level, 3 above), state (locations:
 * 1 undiscovered, 2 discovered, 3 targeted), map icon (0-89) and the colour byte.
 */
export type HudMarker = [number, number, number, number, number, number, number];

export interface HudCompass {
  /** Where the camera looks, in the markers' units (the game wraps them into -half..half: pi or 180). */
  heading: number;
  half: number;
  markers: HudMarker[];
}

export interface HudEnemy {
  name: string;
  /** 0..1 */
  value: number;
  /** The second bar (VATS: what the shot would take), 0..1. */
  optional: number;
  hostile: boolean;
  legendary: boolean;
  skull: boolean;
}

/** The game's two numbers, as it sent them (which is which: docs/HUD.md). */
export interface HudCounts {
  first: number;
  second: number;
}

/** What the HUD shows now; each piece is null while the game wouldn't show its own. */
export interface HudState {
  /** The game's HUD colour (its settings' HUD Color sliders), 0 to 255 each; absent until the game's is read. */
  color?: [number, number, number];
  compass: HudCompass | null;
  /** 0..1, and the share radiation took. */
  health: { value: number; rads: number } | null;
  /** Taking radiation. */
  radiation: boolean;
  ap: { value: number } | null;
  enemy: HudEnemy | null;
  /** The weapon drawn in the right hand (as the player named it), null with none drawn. */
  weapon?: string | null;
  ammo: HudCounts | null;
  explosives: HudCounts | null;
}

/**
 * Two guns out (DualWieldService in the game): the overlay draws the crosshair, the game's is hidden. Shown whatever
 * the HUD setting is.
 */
export interface HudCrosshair {
  on: boolean;
  /** Counts every shot of either gun: each new one kicks the crosshair out, and it closes in again. */
  shots?: number;
  /** The left gun's magazine and the rounds left to load into it. */
  left?: { clip: number; reserve: number };
}

/** On the motorcycle (BikeService in the game): what its gauges show; null off it. */
export interface HudBike {
  kmh: number;
  /** 0..1 of the tank. */
  fuel: number;
  /** The engine's running: off, the tank's empty. */
  on: boolean;
  /** The headlight. */
  lamp: boolean;
  gear: number;
}

/**
 * Fallout 4: the HUD in the overlay (HudService in the game). The game sends the whole state whenever something
 * changed, up to every frame, so it's passed on outside Angular's zone: only the HUD redraws for it.
 */
@Injectable({
  providedIn: 'root',
})
export class HudService implements OnDestroy {
  readonly state$ = new BehaviorSubject<HudState | null>(null);
  readonly crosshair$ = new BehaviorSubject<HudCrosshair | null>(null);
  readonly bike$ = new BehaviorSubject<HudBike | null>(null);

  constructor() {
    skyrimtogether.on('hud', this.onHud.bind(this));
    skyrimtogether.on('crosshair', this.onCrosshair.bind(this));
    skyrimtogether.on('bike', this.onBike.bind(this));
  }

  ngOnDestroy(): void {
    skyrimtogether.off('hud');
    skyrimtogether.off('crosshair');
    skyrimtogether.off('bike');
  }

  private onBike(json: string): void {
    let bike: HudBike | null = null;
    try {
      bike = JSON.parse(json) as HudBike | null;
    } catch {
      bike = null;
    }
    this.bike$.next(bike);
  }

  private onCrosshair(json: string): void {
    let crosshair: HudCrosshair | null = null;
    try {
      crosshair = JSON.parse(json) as HudCrosshair | null;
    } catch {
      crosshair = null;
    }
    this.crosshair$.next(crosshair);
  }

  private onHud(json: string): void {
    let state: HudState | null = null;
    try {
      state = JSON.parse(json) as HudState | null;
    } catch {
      state = null;
    }
    this.state$.next(state);
  }
}
