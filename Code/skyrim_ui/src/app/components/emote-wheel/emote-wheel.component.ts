import {
  ChangeDetectionStrategy,
  ChangeDetectorRef,
  Component,
  HostListener,
  OnDestroy,
} from '@angular/core';
import { Subscription } from 'rxjs';
import { EmoteWheel, EmoteWheelEntry, EmotesService } from '../../services/emotes.service';

/** The wheel's size in its own units (the SVG's viewBox is -200..200): the hub, the slices' inner and outer edges. */
const HUB = 48;
const INNER = 58;
const OUTER = 192;
const LABEL = 128;
/** Half the gap between two slices, in degrees. */
const GAP = 1.5;
/** How far the pointer moves from the middle before a slice is picked, in screen pixels. */
const DEAD_ZONE = 36;

interface Slice {
  path: string;
  x: number;
  y: number;
}

/** A point on the wheel, at an angle (degrees, 0 at the top, clockwise) and a distance from the middle. */
function at(angle: number, radius: number): [number, number] {
  const radians = (angle * Math.PI) / 180;
  return [radius * Math.sin(radians), -radius * Math.cos(radians)];
}

/**
 * Fallout 4 emotes: the wheel of eight while the wheel key is held. The game puts the pointer in the middle of the
 * screen; moving it picks the slice that way (or hovering over one), and letting the key go plays it (the game asks
 * the pointer's emote of this page as it moves: EmotesService.hover). A click plays one at once.
 */
@Component({
  selector: 'app-emote-wheel',
  templateUrl: './emote-wheel.component.html',
  styleUrls: ['./emote-wheel.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class EmoteWheelComponent implements OnDestroy {
  readonly HUB = HUB;
  wheel: EmoteWheel | null = null;
  slices: Slice[] = [];
  hovered = -1;

  private readonly subscription: Subscription;

  constructor(
    private readonly emotes: EmotesService,
    private readonly changes: ChangeDetectorRef,
  ) {
    this.subscription = this.emotes.wheel$.subscribe(wheel => {
      this.wheel = wheel;
      this.hovered = -1;
      this.slices = wheel ? this.layout(wheel.emotes.length) : [];
      this.changes.markForCheck();
    });
  }

  ngOnDestroy(): void {
    this.subscription.unsubscribe();
  }

  @HostListener('window:mousemove', ['$event'])
  onMove(event: MouseEvent): void {
    if (!this.wheel || !this.slices.length) {
      return;
    }
    const dx = event.clientX - window.innerWidth / 2;
    const dy = event.clientY - window.innerHeight / 2;
    let index = -1;
    if (Math.hypot(dx, dy) >= DEAD_ZONE) {
      const step = 360 / this.slices.length;
      const angle = ((Math.atan2(dx, -dy) * 180) / Math.PI + 360) % 360;
      index = Math.round(angle / step) % this.slices.length;
    }
    this.hover(index);
  }

  hover(index: number): void {
    if (!this.wheel || index === this.hovered) {
      return;
    }
    this.hovered = index;
    this.emotes.hover(index >= 0 ? this.wheel.emotes[index].name : '');
    this.changes.markForCheck();
  }

  play(emote: EmoteWheelEntry): void {
    this.emotes.play(emote.name);
  }

  private layout(count: number): Slice[] {
    const step = 360 / count;
    const slices: Slice[] = [];
    for (let i = 0; i < count; i++) {
      const from = i * step - step / 2 + GAP;
      const to = i * step + step / 2 - GAP;
      const [ox0, oy0] = at(from, OUTER);
      const [ox1, oy1] = at(to, OUTER);
      const [ix1, iy1] = at(to, INNER);
      const [ix0, iy0] = at(from, INNER);
      const [x, y] = at(i * step, LABEL);
      slices.push({
        path: `M ${ox0} ${oy0} A ${OUTER} ${OUTER} 0 0 1 ${ox1} ${oy1} L ${ix1} ${iy1} A ${INNER} ${INNER} 0 0 0 ${ix0} ${iy0} Z`,
        x,
        y,
      });
    }
    return slices;
  }
}
