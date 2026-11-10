import {
  ChangeDetectionStrategy,
  ChangeDetectorRef,
  Component,
  HostListener,
  NgZone,
  OnDestroy,
} from '@angular/core';

/** The game asking what to do with the local player's standing C.A.M.P. (JSON from the campCard event). */
interface CampCardRequest {
  /** Caps a move to where the device was used costs. */
  cost: number;
  /** Caps the player carries. */
  caps: number;
  /** Why it can't move there ("" if it can): only picking it up is offered then. */
  refusal: string;
}

/**
 * Fallout 4 C.A.M.P.s: the Makeshift CAMP device used with a C.A.M.P. standing. Move it here (for caps), pick it up
 * (kept as it is, set down again free) or not now. The game opens the overlay for it; keys 1-3, Escape is "not now".
 */
@Component({
  selector: 'app-camp-card',
  templateUrl: './camp-card.component.html',
  styleUrls: ['./camp-card.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class CampCardComponent implements OnDestroy {
  request: CampCardRequest | null = null;

  constructor(
    private readonly zone: NgZone,
    private readonly changeDetector: ChangeDetectorRef,
  ) {
    skyrimtogether.on('campCard', this.onRequest.bind(this));
  }

  ngOnDestroy(): void {
    skyrimtogether.off('campCard');
  }

  /** Why the move button is off, as a translation key and its values; null when it can move. */
  get moveBlocked(): { key: string; values?: Record<string, unknown> } | null {
    if (!this.request) {
      return null;
    }
    if (this.request.refusal) {
      return { key: 'COMPONENT.CAMP_CARD.CANT_MOVE', values: { why: this.request.refusal } };
    }
    if (this.request.caps < this.request.cost) {
      return { key: 'COMPONENT.CAMP_CARD.NOT_ENOUGH_CAPS', values: { caps: this.request.caps } };
    }
    return null;
  }

  move(): void {
    if (!this.moveBlocked) {
      this.send('campMove');
    }
  }

  pickUp(): void {
    this.send('campPickUp');
  }

  cancel(): void {
    this.send('campCancel');
  }

  @HostListener('window:keydown', ['$event'])
  onKey(event: KeyboardEvent): void {
    if (!this.request) {
      return;
    }
    const actions: Record<string, () => void> = {
      Digit1: () => this.move(),
      Numpad1: () => this.move(),
      Digit2: () => this.pickUp(),
      Numpad2: () => this.pickUp(),
      Digit3: () => this.cancel(),
      Numpad3: () => this.cancel(),
    };
    const action = actions[event.code];
    if (action) {
      event.preventDefault();
      action();
    }
  }

  private send(request: string): void {
    // The game's older builds don't have it
    if (typeof skyrimtogether.settlements === 'function') {
      skyrimtogether.settlements(request, '');
    }
    this.request = null;
    this.changeDetector.markForCheck();
  }

  private onRequest(json: string): void {
    this.zone.run(() => {
      this.request = JSON.parse(json) as CampCardRequest | null;
      this.changeDetector.markForCheck();
    });
  }
}
