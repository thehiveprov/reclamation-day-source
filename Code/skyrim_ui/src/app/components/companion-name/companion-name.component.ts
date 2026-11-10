import {
  ChangeDetectionStrategy,
  ChangeDetectorRef,
  Component,
  ElementRef,
  NgZone,
  OnDestroy,
  ViewChild,
} from '@angular/core';

/** The game asking for a dog's name: one just adopted, or Dogmeat joining (JSON from the companionName event). */
interface CompanionNameRequest {
  /** The dog's record, form id in hex. */
  id: string;
  /** Its name now. */
  name: string;
}

/** Fallout 4: names the dog the local player just adopted (or Dogmeat). The game opens the overlay for it. */
@Component({
  selector: 'app-companion-name',
  templateUrl: './companion-name.component.html',
  styleUrls: ['./companion-name.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class CompanionNameComponent implements OnDestroy {
  readonly maxLength = 24;

  request: CompanionNameRequest | null = null;
  name = '';

  @ViewChild('nameInput')
  set nameInput(input: ElementRef<HTMLInputElement> | undefined) {
    // Typing goes straight in
    setTimeout(() => input?.nativeElement.focus());
  }

  constructor(
    private readonly zone: NgZone,
    private readonly changeDetector: ChangeDetectorRef,
  ) {
    skyrimtogether.on('companionName', this.onRequest.bind(this));
  }

  ngOnDestroy(): void {
    skyrimtogether.off('companionName');
  }

  confirm(): void {
    if (!this.request) {
      return;
    }
    const name = this.name.trim();
    this.send(name ? 'name' : 'keep', name ? `${this.request.id}:${name}` : this.request.id);
  }

  keep(): void {
    if (this.request) {
      this.send('keep', this.request.id);
    }
  }

  private send(request: string, payload: string): void {
    // The game's older builds don't have it
    if (typeof skyrimtogether.companions === 'function') {
      skyrimtogether.companions(request, payload);
    }
    this.request = null;
    this.changeDetector.markForCheck();
  }

  private onRequest(json: string): void {
    this.zone.run(() => {
      this.request = JSON.parse(json) as CompanionNameRequest;
      this.name = '';
      this.changeDetector.markForCheck();
    });
  }
}
