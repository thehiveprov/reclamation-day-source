import { OverlayModule } from '@angular/cdk/overlay';
import { ScrollingModule } from '@angular/cdk/scrolling';
import { HttpClientModule } from '@angular/common/http';
import { NgModule } from '@angular/core';
import { FormsModule, ReactiveFormsModule } from '@angular/forms';
import { BrowserModule } from '@angular/platform-browser';
import { BrowserAnimationsModule } from '@angular/platform-browser/animations';
import { FontAwesomeModule } from '@fortawesome/angular-fontawesome';
import { environment } from '../environments/environment';
import { ActionButtonsComponent } from './components/action-buttons/action-buttons.component';
import { ChatComponent } from './components/chat/chat.component';
import { ConnectComponent } from './components/connect/connect.component';
import { ConnectPasswordComponent } from './components/connect-password/connect-password.component';
import { DebugComponent } from './components/debug/debug.component';
import { DisconnectComponent } from './components/disconnect/disconnect.component';
import { DropdownOptionComponent } from './components/dropdown/dropdown-option.component';
import { DropdownComponent } from './components/dropdown/dropdown.component';
import { ErrorComponent } from './components/error/error.component';
import { GroupComponent } from './components/group/group.component';
import { LoadingComponent } from './components/loading/loading.component';
import { NotificationPopupContainerComponent } from './components/notification-popup-container/notification-popup-container.component';
import { NotificationPopupComponent } from './components/notification-popup/notification-popup.component';
import { NotificationsComponent } from './components/notifications/notifications.component';
import { OrderComponent } from './components/order/order.component';
import { PartyMenuComponent } from './components/party-menu/party-menu.component';
import { QuestTeamComponent } from './components/quest-team/quest-team.component';
import { SettlementsComponent } from './components/settlements/settlements.component';
import { PlayerListComponent } from './components/player-list/player-list.component';
import { RespawnTimerComponent } from './components/respawn-timer/respawn-timer.component';
import { CompanionNameComponent } from './components/companion-name/companion-name.component';
import { CampCardComponent } from './components/camp-card/camp-card.component';
import { EmoteWheelComponent } from './components/emote-wheel/emote-wheel.component';
import { DialogueVoteComponent } from './components/dialogue-vote/dialogue-vote.component';
import { NemesisCardComponent } from './components/nemesis-card/nemesis-card.component';
import { NemesisBarComponent } from './components/nemesis-bar/nemesis-bar.component';
import { NemesisScreenComponent } from './components/nemesis-screen/nemesis-screen.component';
import { SocialSidebarComponent } from './components/social-sidebar/social-sidebar.component';
import { HudComponent } from './components/hud/hud.component';
import { StartMenuComponent } from './components/start-menu/start-menu.component';
import { MultiplayerMenuComponent } from './components/multiplayer-menu/multiplayer-menu.component';
import { AutofocusDirective } from './directives/autofocus.directive';
import { PlayerManagerComponent } from './components/player-manager/player-manager.component';
import { PopupComponent } from './components/popup/popup.component';
import { RootComponent } from './components/root/root.component';
import { ServerListComponent } from './components/server-list/server-list.component';
import { SettingsComponent } from './components/settings/settings.component';
import { WorldSettingsComponent } from './components/world-settings/world-settings.component';
import { ToggleComponent } from './components/toggle/toggle.component';
import { WindowComponent } from './components/window/window.component';
import { HealthDirective } from './directives/health.directive';
import { RadioDirective } from './directives/radio.directive';
import { SliderDirective } from './directives/slider.directive';
import { CheckboxDirective } from './directives/checkbox.directive';
import { AssetService } from './services/asset.service';
import { ClientService } from './services/client.service';
import { ChatService } from './services/chat.service';
import { ErrorService } from './services/error.service';
import { GroupService } from './services/group.service';
import { PlayerListService } from './services/player-list.service';
import { SettingService } from './services/setting.service';
import { SoundService } from './services/sound.service';
import { StoreService } from './services/store.service';
import { TranslocoRootModule } from './transloco-root.module';

@NgModule({
  declarations: [
    ChatComponent,
    ConnectComponent,
    ConnectPasswordComponent,
    DebugComponent,
    DisconnectComponent,
    DropdownComponent,
    DropdownOptionComponent,
    ErrorComponent,
    GroupComponent,
    LoadingComponent,
    NotificationPopupComponent,
    NotificationPopupContainerComponent,
    NotificationsComponent,
    OrderComponent,
    PartyMenuComponent,
    QuestTeamComponent,
    SettlementsComponent,
    PlayerListComponent,
    RespawnTimerComponent,
    CompanionNameComponent,
    CampCardComponent,
    EmoteWheelComponent,
    DialogueVoteComponent,
    NemesisCardComponent,
    NemesisBarComponent,
    NemesisScreenComponent,
    SocialSidebarComponent,
    HudComponent,
    StartMenuComponent,
    MultiplayerMenuComponent,
    PlayerManagerComponent,
    ActionButtonsComponent,
    PopupComponent,
    RootComponent,
    ServerListComponent,
    SettingsComponent,
    WorldSettingsComponent,
    ToggleComponent,
    WindowComponent,

    AutofocusDirective,
    CheckboxDirective,
    HealthDirective,
    RadioDirective,
    SliderDirective,
  ],
  imports: [
    BrowserAnimationsModule,
    BrowserModule,
    FontAwesomeModule,
    FormsModule,
    HttpClientModule,
    ReactiveFormsModule,
    ScrollingModule,
    TranslocoRootModule,
    OverlayModule,
  ],
  providers: [
    ClientService,
    ChatService,
    AssetService,
    SoundService,
    ErrorService,
    GroupService,
    PlayerListService,
    StoreService,
    SettingService,

    ...environment.providers,
  ],
  bootstrap: [RootComponent],
})
export class AppModule {}
