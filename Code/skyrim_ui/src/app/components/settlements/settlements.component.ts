import {
  ChangeDetectionStrategy,
  ChangeDetectorRef,
  Component,
  OnDestroy,
  OnInit,
} from '@angular/core';
import {
  Settlement,
  SettlementRight,
  SettlementsService,
} from '../../services/settlements.service';
import { Sound, SoundService } from '../../services/sound.service';

/**
 * Player manager > Settlements: the claimed settlements, who leads each, and the local player's rights there. Leaders
 * give the players of their party the right to build and to scrap, and abandon; the host releases anyone's.
 */
@Component({
  selector: 'app-settlements',
  templateUrl: './settlements.component.html',
  styleUrls: ['./settlements.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class SettlementsComponent implements OnInit, OnDestroy {
  /** The settlement whose abandon (or release) button waits for a second click. */
  confirming: string | null = null;
  private confirmTimer: ReturnType<typeof setTimeout> | null = null;

  constructor(
    readonly settlementsService: SettlementsService,
    private readonly sound: SoundService,
    private readonly changeDetector: ChangeDetectorRef,
  ) {}

  ngOnInit(): void {
    this.settlementsService.refresh();
  }

  ngOnDestroy(): void {
    this.clearConfirm();
  }

  /** Leaders abandon their settlements (a C.A.M.P. is moved instead); the host releases or removes anyone else's. */
  canLetGo(settlement: Settlement, isHost: boolean): boolean {
    return settlement.mine ? !settlement.camp : isHost;
  }

  /** Abandon, release (a settlement) or remove (a C.A.M.P.), or its second click, as a translation key. */
  letGoKey(settlement: Settlement, confirming: boolean): string {
    const action = settlement.mine
      ? 'ABANDON'
      : settlement.camp
      ? 'REMOVE'
      : 'RELEASE';
    return `COMPONENT.SETTLEMENTS.${confirming ? 'CONFIRM_' : ''}${action}`;
  }

  /** The owner picks up their C.A.M.P. while it stands. */
  canPickUp(settlement: Settlement): boolean {
    return settlement.mine && settlement.camp && !settlement.campStored;
  }

  /** What the local player may do there, as a translation key. */
  rightsKey(settlement: Settlement): string {
    if (settlement.mine) {
      if (settlement.camp) {
        return settlement.campStored
          ? 'COMPONENT.SETTLEMENTS.YOUR_CAMP_STORED'
          : 'COMPONENT.SETTLEMENTS.YOUR_CAMP';
      }
      return 'COMPONENT.SETTLEMENTS.YOU_LEAD';
    }
    if (settlement.canBuild && settlement.canScrap) {
      return 'COMPONENT.SETTLEMENTS.YOU_BUILD_AND_SCRAP';
    }
    if (settlement.canBuild) {
      return 'COMPONENT.SETTLEMENTS.YOU_BUILD';
    }
    if (settlement.canScrap) {
      return 'COMPONENT.SETTLEMENTS.YOU_SCRAP';
    }
    return 'COMPONENT.SETTLEMENTS.NO_RIGHTS';
  }

  toggleBuild(settlement: Settlement, right: SettlementRight): void {
    this.settlementsService.setRights(
      settlement,
      right.name,
      !right.build,
      right.scrap,
    );
    this.sound.play(Sound.Ok);
  }

  toggleScrap(settlement: Settlement, right: SettlementRight): void {
    this.settlementsService.setRights(
      settlement,
      right.name,
      right.build,
      !right.scrap,
    );
    this.sound.play(Sound.Ok);
  }

  /** Abandon (leader) or release (host): the first click asks, the second does it. */
  letGo(settlement: Settlement): void {
    if (!this.confirmed(settlement.id)) {
      return;
    }
    if (settlement.mine) {
      this.settlementsService.abandon(settlement);
    } else {
      this.settlementsService.release(settlement);
    }
    this.sound.play(Sound.Ok);
  }

  /** Pick up the local player's C.A.M.P.: the first click asks, the second does it. */
  pickUp(settlement: Settlement): void {
    if (!this.confirmed(`pickup:${settlement.id}`)) {
      return;
    }
    this.settlementsService.pickUp(settlement);
    this.sound.play(Sound.Ok);
  }

  /** The second click on the same button within 4 s; the first one only asks. */
  private confirmed(key: string): boolean {
    if (this.confirming !== key) {
      this.clearConfirm();
      this.confirming = key;
      this.confirmTimer = setTimeout(() => {
        this.confirming = null;
        this.confirmTimer = null;
        this.changeDetector.markForCheck();
      }, 4000);
      this.sound.play(Sound.Focus);
      return false;
    }
    this.clearConfirm();
    return true;
  }

  trackSettlement(_: number, settlement: Settlement): string {
    return settlement.id;
  }

  trackRight(_: number, right: SettlementRight): string {
    return right.name;
  }

  private clearConfirm(): void {
    if (this.confirmTimer) {
      clearTimeout(this.confirmTimer);
    }
    this.confirmTimer = null;
    this.confirming = null;
  }
}
