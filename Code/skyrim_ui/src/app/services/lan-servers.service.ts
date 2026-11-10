import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { BehaviorSubject } from 'rxjs';

/** A server on the local network, as it answered the game's search. */
export interface LanServer {
  /** "host:port" to join. */
  address: string;
  name: string;
  port: number;
  players: number;
  maxPlayers: number;
  version: string;
  /** It needs a password. */
  password: boolean;
  /** Same version as this game (joining needs exactly the same). */
  compatible: boolean;
}

/** How long a search may take before it's given up on (the game answers after 1.5 s). */
const SEARCH_TIMEOUT_MS = 5000;

/**
 * The servers on the local network, for the server browsers' LAN tab (the main menu's and the in-game one): the game
 * broadcasts a question every server there answers.
 */
@Injectable({
  providedIn: 'root',
})
export class LanServersService implements OnDestroy {
  readonly servers$ = new BehaviorSubject<LanServer[]>([]);
  readonly searching$ = new BehaviorSubject<boolean>(false);
  /** A search has finished at least once. */
  readonly searched$ = new BehaviorSubject<boolean>(false);

  private timeout?: ReturnType<typeof setTimeout>;

  constructor(private readonly zone: NgZone) {
    skyrimtogether.on('lanServers', this.onServers.bind(this));
  }

  ngOnDestroy(): void {
    skyrimtogether.off('lanServers');
  }

  /** Looks for servers again (ignored while a search runs). */
  search(): void {
    if (this.searching$.value) {
      return;
    }
    this.searching$.next(true);
    skyrimtogether.lanScan();
    this.timeout = setTimeout(() => this.zone.run(() => this.searching$.next(false)), SEARCH_TIMEOUT_MS);
  }

  private onServers(json: string): void {
    this.zone.run(() => {
      clearTimeout(this.timeout);
      this.servers$.next(JSON.parse(json) as LanServer[]);
      this.searching$.next(false);
      this.searched$.next(true);
    });
  }
}
