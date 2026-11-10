import { Injectable, OnDestroy } from '@angular/core';

/**
 * The game's HUD colour (Settings, Display: HUD Color R/G/B) as the whole overlay's colour: the HUD, the menus, the
 * sidebar, the windows, popups and wheels. The game sends it as it changes, and again every few seconds for a page that
 * has just loaded ('hudColor', "[r,g,b]"). It goes on the root: --pip, which everything's colour is made from
 * (styles.scss), and the panels' dark tints, the colour darkened as far as the overlay's own green was for them.
 */
@Injectable({
  providedIn: 'root',
})
export class ThemeService implements OnDestroy {
  /** What's on the root now, "" for the overlay's own green. */
  private current = '';

  constructor() {
    skyrimtogether.on('hudColor', this.onHudColor.bind(this));
  }

  ngOnDestroy(): void {
    skyrimtogether.off('hudColor');
  }

  private onHudColor(json: string): void {
    let color: unknown;
    try {
      color = JSON.parse(json);
    } catch {
      return;
    }
    if (
      !Array.isArray(color) ||
      color.length !== 3 ||
      !color.every(c => typeof c === 'number' && Number.isFinite(c))
    ) {
      return;
    }
    const [r, g, b] = color.map(c => Math.min(Math.max(Math.round(c), 0), 255));
    this.apply(r, g, b);
  }

  private apply(r: number, g: number, b: number): void {
    const key = `${r}, ${g}, ${b}`;
    if (key === this.current) {
      return;
    }
    this.current = key;
    // Each a share of the colour plus a floor, chosen so the overlay's own green (26, 255, 128) gives the tints
    // styles.scss had for it (11,26,15 / 4,18,10 / 3,13,7 / 2,9,5): a red HUD gets dark red panels, a white one grey
    const tint = (share: number, floor: number) =>
      [r, g, b].map(c => Math.round(c * share + floor)).join(', ');
    const style = document.documentElement.style;
    style.setProperty('--pip', key);
    style.setProperty('--tint-light', tint(0.07, 8));
    style.setProperty('--tint', tint(0.07, 2));
    style.setProperty('--tint-mid', tint(0.05, 1.5));
    style.setProperty('--tint-deep', tint(0.033, 1));
  }
}
