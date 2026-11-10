import { ComponentPortal } from '@angular/cdk/portal';
import { ApplicationRef, Injectable } from '@angular/core';
import { TranslocoService } from '@ngneat/transloco';
import { first, takeUntil, timer } from 'rxjs';
import { filter, map } from 'rxjs/operators';
import { RootComponent } from '../components/root/root.component';
import { PlayerManagerTab } from '../models/player-manager-tab.enum';
import { View } from '../models/view.enum';
import { ClientService } from '../services/client.service';
import { DestroyService } from '../services/destroy.service';
import { PopupNotificationService } from '../services/popup-notification.service';
import { UiRepository } from '../store/ui.repository';
import { DebugHelperComponent } from './debug-helper/debug-helper.component';
import { SkyrimtogetherMock } from './skyrimtogether.mock';

@Injectable({
  providedIn: 'root',
})
export class MockClientService extends DestroyService {
  public readonly skyrimtogether: SkyrimtogetherMock =
    skyrimtogether as SkyrimtogetherMock;

  constructor(
    private readonly clientService: ClientService,
    private readonly popupNotificationService: PopupNotificationService,
    private readonly uiRepository: UiRepository,
    private readonly translocoService: TranslocoService,
    private readonly appRef: ApplicationRef,
  ) {
    super();

    this.skyrimtogether.initMock();

    timer(100)
      .pipe(
        takeUntil(this),
        map(() => this.appRef.components[0]?.instance),
        filter(overlay => !!overlay),
        first(),
      )
      .subscribe((root: RootComponent) => this.init(root));
  }

  private init(root: RootComponent) {
    const overlayRef = root.overlay.create();
    const userProfilePortal = new ComponentPortal(DebugHelperComponent);
    overlayRef.attach(userProfilePortal);

    // Preview: ?view=<settings, server_list, player_manager, ...> opens that screen, ?tab=<player_list, party_menu,
    // quests> the player manager's tab
    const params = new URLSearchParams(window.location.search);
    const view = View[params.get('view')?.toUpperCase() as keyof typeof View];
    const tab = PlayerManagerTab[params.get('tab')?.toUpperCase() as keyof typeof PlayerManagerTab];
    if (view) {
      timer(900)
        .pipe(takeUntil(this))
        .subscribe(() => {
          this.uiRepository.openView(view);
          if (tab) {
            this.uiRepository.openPlayerManagerTab(tab);
          }
        });
    }
  }
}
