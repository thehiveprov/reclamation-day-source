import { ChangeDetectionStrategy, Component } from '@angular/core';
import { Observable, of, switchMap, takeWhile, timer } from 'rxjs';
import { map } from 'rxjs/operators';
import { ClientService } from '../../services/client.service';

/** Counts down the seconds until the local player respawns, while they are in bleedout. */
@Component({
  selector: 'app-respawn-timer',
  templateUrl: './respawn-timer.component.html',
  styleUrls: ['./respawn-timer.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class RespawnTimerComponent {
  /** Whole seconds left, 0 when hidden. */
  secondsLeft$: Observable<number>;

  constructor(private readonly clientService: ClientService) {
    this.secondsLeft$ = this.clientService.respawnCountdownChange.pipe(
      switchMap(seconds => {
        if (seconds <= 0) {
          return of(0);
        }
        const end = Date.now() + seconds * 1000;
        return timer(0, 100).pipe(
          map(() => Math.max(0, Math.ceil((end - Date.now()) / 1000))),
          takeWhile(left => left > 0, true),
        );
      }),
    );
  }
}
