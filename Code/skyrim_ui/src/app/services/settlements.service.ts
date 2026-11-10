import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { BehaviorSubject } from 'rxjs';

/** A player's rights at a settlement someone else leads. */
export interface SettlementRight {
  name: string;
  online: boolean;
  /** In the local player's party now (rights stay when someone leaves it). */
  inParty: boolean;
  /** Place and move. */
  build: boolean;
  /** Scrap and store. */
  scrap: boolean;
}

/** A claimed settlement. */
export interface Settlement {
  /** The workbench, "<mod hex>:<base hex>". */
  id: string;
  name: string;
  leader: string;
  leaderOnline: boolean;
  /** A player's C.A.M.P.: its owner leads it; it's moved or picked up, not abandoned, and the host removes it. */
  camp: boolean;
  /** A C.A.M.P. its owner picked up: nothing of it stands until they set it down again. */
  campStored: boolean;
  /** The local player leads it. */
  mine: boolean;
  canBuild: boolean;
  canScrap: boolean;
  /** For the local player's own settlements: who has rights, and the rest of their party (both false). */
  rights: SettlementRight[];
}

/** Settlement claims as the game has them (WorkshopService). */
export interface SettlementsState {
  connected: boolean;
  localName: string;
  /** May release anyone's claim (the host is). */
  isHost: boolean;
  /** The settlement the local player is in build mode at, "" if none. */
  currentWorkshop: string;
  settlements: Settlement[];
}

/**
 * Settlement claims (Fallout 4): the first player to claim a settlement at its workbench leads it and gives the
 * players of their party the right to build and to scrap there. The game sends its state ("settlementsState");
 * settlements(request, payload) gives rights, abandons, and (the host) releases. Claiming happens at the workbench.
 */
@Injectable({
  providedIn: 'root',
})
export class SettlementsService implements OnDestroy {
  readonly state$ = new BehaviorSubject<SettlementsState | null>(null);

  constructor(private readonly zone: NgZone) {
    skyrimtogether.on('settlementsState', this.onState.bind(this));
  }

  ngOnDestroy(): void {
    skyrimtogether.off('settlementsState');
  }

  refresh(): void {
    this.request('get');
  }

  /** Leader: a player's rights at one of their settlements. */
  setRights(
    settlement: Settlement,
    playerName: string,
    build: boolean,
    scrap: boolean,
  ): void {
    this.request(
      'rights',
      `${settlement.id}:${build ? 1 : 0}:${scrap ? 1 : 0}:${playerName}`,
    );
  }

  /** Leader: let the settlement go, for someone else to claim. */
  abandon(settlement: Settlement): void {
    this.request('abandon', settlement.id);
  }

  /** Host: release someone else's claim. */
  release(settlement: Settlement): void {
    this.request('release', settlement.id);
  }

  /** Owner: pack their C.A.M.P. up, with everything built at it, to set it down somewhere later. */
  pickUp(settlement: Settlement): void {
    this.request('pickup', settlement.id);
  }

  private request(request: string, payload = ''): void {
    // The game's older builds don't have it
    if (typeof skyrimtogether.settlements === 'function') {
      skyrimtogether.settlements(request, payload);
    }
  }

  private onState(json: string): void {
    this.zone.run(() => this.state$.next(JSON.parse(json) as SettlementsState));
  }
}
