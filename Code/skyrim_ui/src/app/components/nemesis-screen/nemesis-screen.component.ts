import {
  ChangeDetectionStrategy,
  ChangeDetectorRef,
  Component,
  HostListener,
  OnDestroy,
} from '@angular/core';
import { Subscription } from 'rxjs';
import {
  NemesesService,
  NemesisMan,
  NemesisScreenPage,
  NemesisVictim,
  NemesisWound,
  pronouns,
} from '../../services/nemeses.service';
import { Sound, SoundService } from '../../services/sound.service';

type Tab = 'record' | 'gang' | 'world' | 'wounds';

/** Nemesis::Part, as the body outline draws them. */
const PARTS = ['head', 'torso', 'leftArm', 'rightArm', 'leftLeg', 'rightLeg'] as const;

/**
 * Fallout 4 nemeses: the full nemesis screen, opened from the Pip-Boy's OPEN FULL SCREEN entry or its key (N by
 * default). Four tabs, each a page the server writes for this player: their record, the gangs' hierarchy, the world,
 * and the wound log. A man's box opens his page over it. Keys: Q/E the tabs, the arrows the gangs (hierarchy) or the
 * hits (wound log), Backspace closes a man's page, N closes the screen (Escape closes the overlay, and the screen with
 * it). In the overlay's look, laid out after enRAGE's screens, with our own words.
 */
@Component({
  selector: 'app-nemesis-screen',
  templateUrl: './nemesis-screen.component.html',
  styleUrls: ['./nemesis-screen.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class NemesisScreenComponent implements OnDestroy {
  readonly screen$ = this.nemeses.screen$;
  readonly man$ = this.nemeses.man$;
  readonly tabs: { id: Tab; key: string }[] = [
    { id: 'record', key: 'COMPONENT.NEMESIS_SCREEN.RECORD' },
    { id: 'gang', key: 'COMPONENT.NEMESIS_SCREEN.HIERARCHY' },
    { id: 'world', key: 'COMPONENT.NEMESIS_SCREEN.WORLD' },
    { id: 'wounds', key: 'COMPONENT.NEMESIS_SCREEN.WOUNDS' },
  ];
  readonly marks = [1, 2, 3, 4, 5];
  /** Notoriety's levels: noticed, wanted, hunted, blood feud. */
  readonly levels = [10, 25, 50, 75];
  readonly parts = PARTS;

  victim = 0;
  wound = 0;

  private readonly pages: Subscription;

  constructor(
    private readonly nemeses: NemesesService,
    private readonly sound: SoundService,
    private readonly changes: ChangeDetectorRef,
  ) {
    // A wound log asked for one man opens on him
    this.pages = this.screen$.subscribe(page => {
      if (page?.page === 'wounds') {
        const index = page.selected ? (page.victims ?? []).findIndex(victim => victim.id === page.selected) : -1;
        this.victim = Math.max(0, index);
        this.wound = Math.max(0, (page.victims?.[this.victim]?.wounds.length ?? 1) - 1);
      }
    });
  }

  ngOnDestroy(): void {
    this.pages.unsubscribe();
  }

  /** A string's {{he}}, {{him}}, {{his}} for this man or woman, with its other parameters. */
  words(female?: boolean, extra: Record<string, unknown> = {}): Record<string, unknown> {
    return { ...pronouns(female), ...extra };
  }

  show(tab: Tab): void {
    this.nemeses.show(tab);
    this.sound.play(Sound.PrevNext);
  }

  showGang(id: string): void {
    this.nemeses.show(`gang:${id}`);
    this.sound.play(Sound.PrevNext);
  }

  openMan(man: { id: number } | null | undefined): void {
    if (man?.id) {
      this.nemeses.show(`man:${man.id}`);
      this.sound.play(Sound.Ok);
    }
  }

  openWounds(id: number): void {
    this.nemeses.closeMan();
    this.nemeses.show(`wounds:${id}`);
    this.sound.play(Sound.PrevNext);
  }

  closeMan(): void {
    this.nemeses.closeMan();
    this.sound.play(Sound.Cancel);
  }

  close(): void {
    this.nemeses.closeScreen();
    this.sound.play(Sound.Cancel);
  }

  pickVictim(index: number): void {
    const page = this.screen$.value;
    this.victim = index;
    this.wound = Math.max(0, (page?.victims?.[index]?.wounds.length ?? 1) - 1);
    this.sound.play(Sound.Focus);
    this.changes.markForCheck();
  }

  pickWound(victim: number, wound: number): void {
    this.victim = victim;
    this.wound = wound;
    this.sound.play(Sound.Focus);
    this.changes.markForCheck();
  }

  selectedVictim(page: NemesisScreenPage): NemesisVictim | null {
    return page.victims?.[this.victim] ?? null;
  }

  selectedWound(page: NemesisScreenPage): NemesisWound | null {
    return this.selectedVictim(page)?.wounds[this.wound] ?? null;
  }

  /** The parts of the body outline his wounds touched, for the faint marks. */
  hitParts(victim: NemesisVictim | null): Set<number> {
    return new Set((victim?.wounds ?? []).map(wound => wound.part));
  }

  /** A man's name as a box shows it: his name once known, else his seat. */
  label(man: NemesisMan): string {
    return man.known ? man.name : '';
  }

  karmaPercent(value: number): number {
    return Math.min(100, Math.max(0, (value + 1000) / 20));
  }

  /** -100..100 as the width of half a bar, from the middle. */
  relationWidth(value: number): number {
    return Math.min(50, Math.abs(value) / 2);
  }

  trackId(_: number, item: { id: number | string }): number | string {
    return item.id;
  }

  @HostListener('window:keydown', ['$event'])
  // @ts-ignore
  private onKey(event: KeyboardEvent): void {
    const page = this.screen$.value;
    if (!page) {
      return;
    }
    const key = event.key.toLowerCase();
    if (this.man$.value) {
      if (key === 'backspace' || key === 'n') {
        this.closeMan();
        event.preventDefault();
      }
      return;
    }

    const tab = this.tabs.findIndex(item => item.id === page.page);
    switch (key) {
      case 'q':
        this.show(this.tabs[(tab + this.tabs.length - 1) % this.tabs.length].id);
        break;
      case 'e':
        this.show(this.tabs[(tab + 1) % this.tabs.length].id);
        break;
      case 'n':
        this.close();
        break;
      case 'arrowleft':
      case 'arrowright':
        if (page.page === 'gang' && page.gang && page.gangs.length) {
          const gang = page.gangs.findIndex(item => item.id === page.gang!.id);
          const step = key === 'arrowleft' ? page.gangs.length - 1 : 1;
          this.showGang(page.gangs[(gang + step) % page.gangs.length].id);
        }
        break;
      case 'arrowup':
      case 'arrowdown':
        if (page.page === 'wounds' && page.victims?.length) {
          this.step(page, key === 'arrowup' ? -1 : 1);
        }
        break;
      default:
        return;
    }
    event.preventDefault();
    this.changes.markForCheck();
  }

  /** The wound log's arrows: through a man's hits, then on to the next man's. */
  private step(page: NemesisScreenPage, direction: number): void {
    const victims = page.victims ?? [];
    let victim = this.victim;
    let wound = this.wound + direction;
    if (wound < 0) {
      victim = (victim + victims.length - 1) % victims.length;
      wound = victims[victim].wounds.length - 1;
    } else if (wound >= victims[victim].wounds.length) {
      victim = (victim + 1) % victims.length;
      wound = 0;
    }
    this.pickWound(victim, Math.max(0, wound));
  }
}
