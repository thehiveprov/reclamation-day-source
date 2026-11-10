import { ChangeDetectionStrategy, Component, EventEmitter, OnDestroy, OnInit, Output } from '@angular/core';
import { Observable } from 'rxjs';
import { WorldSettingsItem, WorldSettingsService, WorldSettingsState } from '../../services/world-settings.service';

/**
 * The F2 overlay's WORLD SETTINGS: the host's weather, and NAC X's look, camera, lighting and effects (NAC's holotape
 * settings), one tab per page. Changes apply at once: the game isn't paused here.
 */
@Component({
  selector: 'app-world-settings',
  templateUrl: './world-settings.component.html',
  styleUrls: ['./world-settings.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class WorldSettingsComponent implements OnInit, OnDestroy {
  @Output() done = new EventEmitter<void>();

  readonly state$: Observable<WorldSettingsState | null>;
  /** The help of the entry under the mouse. */
  help = '';

  constructor(private readonly worldSettings: WorldSettingsService) {
    this.state$ = worldSettings.state$;
  }

  ngOnInit(): void {
    this.worldSettings.open();
  }

  ngOnDestroy(): void {
    this.worldSettings.close();
  }

  openPage(page: number): void {
    this.help = '';
    this.worldSettings.page(page);
  }

  toggle(item: WorldSettingsItem): void {
    this.worldSettings.set(item.i, item.value ? 0 : 1);
  }

  choose(item: WorldSettingsItem, option: number): void {
    if (option !== item.value) {
      this.worldSettings.set(item.i, option);
    }
  }

  slide(item: WorldSettingsItem, event: Event): void {
    this.worldSettings.set(item.i, Number((event.target as HTMLInputElement).value));
  }

  press(item: WorldSettingsItem): void {
    this.worldSettings.press(item.i);
  }

  showHelp(item: WorldSettingsItem): void {
    this.help = item.help;
  }

  trackItem(_: number, item: WorldSettingsItem): number {
    return item.i;
  }

  trackRow(index: number): number {
    return index;
  }
}
