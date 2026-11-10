import { ChangeDetectionStrategy, ChangeDetectorRef, Component, ElementRef, OnDestroy, OnInit } from '@angular/core';
import { Subscription } from 'rxjs';
import { HudBike, HudCrosshair, HudMarker, HudService, HudState } from '../../services/hud.service';
import { HUD_KIND_ICONS, HUD_MAP_ICONS, HudMarkerIcon } from './hud-marker-icons';

/** Degrees either side of the middle the compass shows. */
const SPAN = 90;
/** Where a marker off the strip is held: inside the strip's faded ends. */
const EDGE = SPAN * 0.9;
/** Tick spacing on the compass, degrees. */
const TICK = 15;
const DIRECTIONS: Record<number, string> = { 0: 'N', 45: 'NE', 90: 'E', 135: 'SE', 180: 'S', 225: 'SW', 270: 'W', 315: 'NW' };

/** The speedometer's dial: miles an hour, as a car of the time has it, from -130 to +130 degrees. */
const SPEED_TOP = 100;
const SPEED_SWEEP = 130;
/** The fuel gauge's dial: E to F, from -60 to +60 degrees. */
const FUEL_SWEEP = 60;
const MPH_PER_KMH = 0.621371;

export interface GaugeTick {
  x1: number;
  y1: number;
  x2: number;
  y2: number;
  major: boolean;
  label: string;
  lx: number;
  ly: number;
}

/** A tick around a dial's centre at 50,50 (0 degrees is straight up, clockwise). */
function gaugeTick(degrees: number, major: boolean, label: string): GaugeTick {
  const r = (degrees * Math.PI) / 180;
  const s = Math.sin(r);
  const c = Math.cos(r);
  const inner = major ? 38 : 41;
  return { x1: 50 + s * 45, y1: 50 - c * 45, x2: 50 + s * inner, y2: 50 - c * inner, major, label, lx: 50 + s * 30, ly: 50 - c * 30 };
}

const SPEED_TICKS: GaugeTick[] = [];
for (let mph = 0; mph <= SPEED_TOP; mph += 5) {
  const major = mph % 20 === 0;
  SPEED_TICKS.push(gaugeTick((mph / SPEED_TOP) * 2 * SPEED_SWEEP - SPEED_SWEEP, major, major ? String(mph) : ''));
}
const FUEL_TICKS: GaugeTick[] = [];
for (let eighth = 0; eighth <= 8; ++eighth) {
  const major = eighth % 4 === 0;
  const label = eighth === 0 ? 'E' : eighth === 8 ? 'F' : '';
  FUEL_TICKS.push(gaugeTick((eighth / 8) * 2 * FUEL_SWEEP - FUEL_SWEEP, major, label));
}

export interface CompassTick {
  angle: number;
  /** 0..100 along the strip. */
  x: number;
  /** A direction's letters (N, NE...), else empty. */
  label: string;
  major: boolean;
}

export type MarkerShape = 'quest' | 'door' | 'own' | 'enemy' | 'location' | 'recon';

export interface PlacedMarker {
  key: string;
  x: number;
  shape: MarkerShape;
  /** The game's own icon remade (hud-marker-icons.ts): its path, in scan lines or an undiscovered place's outline. */
  path: string;
  /** Locations sit on a chip with a ring. */
  chip: boolean;
  /** Targeted (an enemy picked in VATS, a location chosen on the map). */
  targeted: boolean;
  /** Locations not found yet. */
  undiscovered: boolean;
  /** Quest kinds: where it is against the player. */
  height: 'above' | 'below' | '';
  /** Off the strip, held at its edge (quest kinds). */
  edge: boolean;
  /** Drawn later is on top. */
  order: number;
}

/** -180..180 */
function wrap(degrees: number): number {
  return ((((degrees + 180) % 360) + 360) % 360) - 180;
}

/**
 * Fallout 4: the HUD in the overlay's style (HudService in the game): the compass along the top with the game's own
 * markers, the enemy's bar under it, health with what radiation took at the bottom left, AP, ammo and grenades at the
 * bottom right. Each piece is there only while the game would show its own.
 */
@Component({
  selector: 'app-hud',
  templateUrl: './hud.component.html',
  styleUrls: ['./hud.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class HudComponent implements OnInit, OnDestroy {
  state: HudState | null = null;
  ticks: CompassTick[] = [];
  markers: PlacedMarker[] = [];
  crosshair: HudCrosshair | null = null;
  /** Which of the crosshair's two kick animations runs (switching between them restarts it), -1 before the first. */
  kick = -1;
  bike: HudBike | null = null;
  readonly speedTicks = SPEED_TICKS;
  readonly fuelTicks = FUEL_TICKS;

  private subscription?: Subscription;
  private crosshairSubscription?: Subscription;
  private bikeSubscription?: Subscription;
  /** The shots the crosshair has kicked for, -1 while it's hidden (it comes back without a kick). */
  private shots = -1;
  /** The colour the HUD is drawn in now, "" for the overlay's own green. */
  private color = '';

  constructor(
    private readonly hud: HudService,
    private readonly changeDetector: ChangeDetectorRef,
    private readonly element: ElementRef<HTMLElement>,
  ) {}

  ngOnInit(): void {
    // Up to every frame, outside Angular's zone: only this view is checked
    this.subscription = this.hud.state$.subscribe(state => {
      this.state = state;
      this.applyColor(state?.color);
      this.layoutCompass();
      this.changeDetector.detectChanges();
    });
    this.crosshairSubscription = this.hud.crosshair$.subscribe(crosshair => {
      const shots = crosshair?.on ? crosshair.shots ?? 0 : -1;
      if (shots >= 0 && this.shots >= 0 && shots > this.shots) {
        this.kick = this.kick === 0 ? 1 : 0;
      }
      this.shots = shots;
      this.crosshair = crosshair;
      this.changeDetector.detectChanges();
    });
    this.bikeSubscription = this.hud.bike$.subscribe(bike => {
      this.bike = bike;
      this.changeDetector.detectChanges();
    });
  }

  /** The speedometer's needle, from its stop at 0 and past its top a little before the pin holds it. */
  speedNeedle(kmh: number): number {
    const mph = Math.min(kmh * MPH_PER_KMH, SPEED_TOP * 1.04);
    return (mph / SPEED_TOP) * 2 * SPEED_SWEEP - SPEED_SWEEP;
  }

  speedMph(kmh: number): number {
    return Math.round(kmh * MPH_PER_KMH);
  }

  fuelNeedle(fuel: number): number {
    return Math.min(Math.max(fuel, 0), 1) * 2 * FUEL_SWEEP - FUEL_SWEEP;
  }

  /**
   * The game's HUD colour, as the game's own HUD would take it: the overlay's colour tokens set again on the HUD (the
   * root's --pip-color and --line were worked out from the root's --pip, so all three).
   */
  private applyColor(color: [number, number, number] | undefined): void {
    const rgb = color ? color.join(', ') : '';
    if (rgb === this.color) {
      return;
    }
    this.color = rgb;
    const style = this.element.nativeElement.style;
    if (rgb) {
      style.setProperty('--pip', rgb);
      style.setProperty('--pip-color', `rgb(${rgb})`);
      style.setProperty('--line', `rgba(${rgb}, 0.35)`);
    } else {
      style.removeProperty('--pip');
      style.removeProperty('--pip-color');
      style.removeProperty('--line');
    }
  }

  ngOnDestroy(): void {
    this.subscription?.unsubscribe();
    this.crosshairSubscription?.unsubscribe();
    this.bikeSubscription?.unsubscribe();
  }

  trackTick(_: number, tick: CompassTick): number {
    return tick.angle;
  }

  trackMarker(_: number, marker: PlacedMarker): string {
    return marker.key;
  }

  percent(value: number | undefined): number {
    return Math.max(0, Math.min(1, value ?? 0)) * 100;
  }

  private layoutCompass(): void {
    const compass = this.state?.compass;
    if (!compass) {
      this.ticks = [];
      this.markers = [];
      return;
    }

    const toDegrees = compass.half > 0 ? 180 / compass.half : 1;
    const heading = compass.heading * toDegrees;

    const ticks: CompassTick[] = [];
    for (let angle = 0; angle < 360; angle += TICK) {
      const offset = wrap(angle - heading);
      if (Math.abs(offset) <= SPAN) {
        ticks.push({ angle, x: 50 + (offset / SPAN) * 50, label: DIRECTIONS[angle] ?? '', major: angle % 90 === 0 });
      }
    }
    this.ticks = ticks;

    const markers: PlacedMarker[] = [];
    compass.markers.forEach((marker, index) => {
      const placed = this.place(marker, index, heading, toDegrees);
      if (placed) {
        markers.push(placed);
      }
    });
    markers.sort((a, b) => a.order - b.order);
    this.markers = markers;
  }

  private place(marker: HudMarker, index: number, heading: number, toDegrees: number): PlacedMarker | null {
    const [kind, markerHeading, , height, state, icon] = marker;
    let shape: MarkerShape;
    let order: number;
    switch (kind) {
      case 1:
        shape = 'quest';
        order = 4;
        break;
      case 2:
        shape = 'door';
        order = 3;
        break;
      case 3:
        shape = 'own';
        order = 3;
        break;
      case 4:
      case 6:
        shape = 'enemy';
        order = 2;
        break;
      case 10:
        shape = 'recon';
        order = 2;
        break;
      default:
        shape = 'location';
        order = 1;
    }

    let offset = wrap(markerHeading * toDegrees - heading);
    let edge = false;
    if (Math.abs(offset) > EDGE) {
      // Where to go stays in sight; the rest is gone past the strip's ends
      if (order < 3) {
        if (Math.abs(offset) > SPAN) {
          return null;
        }
      } else {
        offset = Math.sign(offset) * EDGE;
        edge = true;
      }
    }

    const undiscovered = shape === 'location' && state === 1;
    const glyph = this.icon(kind, icon);
    return {
      key: `${kind}:${icon}:${index}`,
      x: 50 + (offset / SPAN) * 50,
      shape,
      path: undiscovered && glyph.outline ? glyph.outline : glyph.lines,
      chip: shape === 'location',
      targeted: kind === 6 || (shape === 'location' && state === 3),
      undiscovered,
      height: shape === 'quest' || shape === 'door' ? (height === 3 ? 'above' : height === 1 ? 'below' : '') : '',
      edge,
      order,
    };
  }

  /** The kind's icon, a location's by its map icon (a dot for one the game's HUD has none of). */
  private icon(kind: number, icon: number): HudMarkerIcon {
    switch (kind) {
      case 1:
        return HUD_KIND_ICONS.quest;
      case 2:
        return HUD_KIND_ICONS.questDoor;
      case 3:
        return HUD_KIND_ICONS.playerSet;
      case 4:
        return HUD_KIND_ICONS.enemy;
      case 6:
        return HUD_KIND_ICONS.enemyTargeted;
      case 10:
        return HUD_KIND_ICONS.recon;
      default:
        return HUD_MAP_ICONS[icon] ?? HUD_MAP_ICONS[77] ?? HUD_KIND_ICONS.enemy;
    }
  }
}
