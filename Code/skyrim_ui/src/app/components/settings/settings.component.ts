import { Component, EventEmitter, HostListener, Output } from '@angular/core';
import { TranslocoService } from '@ngneat/transloco';
import { lastValueFrom, map, Observable } from 'rxjs';
import { Tag } from 'src/app/models/tag';
import {
  autoHideTimerLengths,
  FontSize,
  PartyAnchor,
  SettingService,
} from 'src/app/services/setting.service';
import { Sound, SoundService } from '../../services/sound.service';
import { environment } from 'src/environments/environment';
import { HttpClient } from '@angular/common/http';
import { ClientService } from 'src/app/services/client.service';
import {
  BuildToolsService,
  BuildToolsState,
  keyName,
} from 'src/app/services/build-tools.service';

@Component({
  selector: 'app-settings',
  templateUrl: './settings.component.html',
  styleUrls: ['./settings.component.scss'],
})
export class SettingsComponent {
  readonly availableLanguages = this.translocoService.getAvailableLangs();
  readonly availableFontSizes: { id: FontSize; label: string }[] = [
    { id: FontSize.XS, label: 'COMPONENT.SETTINGS.FONT_SIZES.XS' },
    { id: FontSize.S, label: 'COMPONENT.SETTINGS.FONT_SIZES.S' },
    { id: FontSize.M, label: 'COMPONENT.SETTINGS.FONT_SIZES.M' },
    { id: FontSize.L, label: 'COMPONENT.SETTINGS.FONT_SIZES.L' },
    { id: FontSize.XL, label: 'COMPONENT.SETTINGS.FONT_SIZES.XL' },
  ];
  readonly availablePartyAnchors: { id: PartyAnchor; label: string }[] = [
    {
      id: PartyAnchor.TOP_LEFT,
      label: 'COMPONENT.SETTINGS.PARTY_ANCHOR_POSITION.TOP_LEFT',
    },
    {
      id: PartyAnchor.TOP_RIGHT,
      label: 'COMPONENT.SETTINGS.PARTY_ANCHOR_POSITION.TOP_RIGHT',
    },
    {
      id: PartyAnchor.BOTTOM_RIGHT,
      label: 'COMPONENT.SETTINGS.PARTY_ANCHOR_POSITION.BOTTOM_LEFT',
    },
    {
      id: PartyAnchor.BOTTOM_LEFT,
      label: 'COMPONENT.SETTINGS.PARTY_ANCHOR_POSITION.BOTTOM_RIGHT',
    },
  ];
  readonly availableAutoHideTimes = autoHideTimerLengths;

  public settings = this.settingService.settings;
  public autoHideTime: number;
  public partyAnchor: PartyAnchor;
  public partyAnchorOffsetX: number;
  public partyAnchorOffsetY: number;
  public fontSize: FontSize;
  public maxFontSize = Object.values(FontSize).length - 1;
  public minFontSize = 0;

  clientVersion$: Observable<string>;
  isVersionOutdated: Promise<boolean>;

  // Build tools: the game's state, and the action waiting for a key press
  readonly buildTools$ = this.buildTools.state$;
  readonly keyName = keyName;
  capturingAction: string | null = null;
  private lastKey: number | null = null;

  @Output() public done = new EventEmitter<void>();
  @Output() public settingsUpdated = new EventEmitter<void>();

  constructor(
    private readonly settingService: SettingService,
    private readonly sound: SoundService,
    private readonly translocoService: TranslocoService,
    private readonly http: HttpClient,
    private readonly client: ClientService,
    private readonly buildTools: BuildToolsService,
  ) {
    this.clientVersion$ = this.client.versionSet.pipe(map(version => version.split('-')[0]));
  }

  changeKey(action: string): void {
    this.capturingAction = this.capturingAction === action ? null : action;
    this.sound.play(Sound.Focus);
  }

  setBuildTool(setting: 'placeAnywhere' | 'scrapAnything', on: boolean): void {
    this.buildTools.set(setting, on);
  }

  resetBuildKeys(): void {
    this.capturingAction = null;
    this.buildTools.resetKeys();
    this.sound.play(Sound.Ok);
  }

  /** The message for a key the game refused, if the last change was refused. */
  buildToolsError(
    state: BuildToolsState,
  ): { key: string; params: Record<string, string> } | null {
    if (!state.error || this.lastKey === null) {
      return null;
    }
    const key = keyName(this.lastKey);
    if (state.error === 'reserved') {
      return { key: 'COMPONENT.SETTINGS.BUILD_TOOLS.ERROR_RESERVED', params: { key } };
    }
    if (state.error.startsWith('used:')) {
      const action = this.translocoService.translate(
        `COMPONENT.SETTINGS.BUILD_TOOLS.ACTIONS.${state.error.substring(5)}`,
      );
      return { key: 'COMPONENT.SETTINGS.BUILD_TOOLS.ERROR_USED', params: { key, action } };
    }
    return null;
  }

  @HostListener('window:keydown', ['$event'])
  // @ts-ignore
  private captureKey(event: KeyboardEvent): void {
    // Escape cancels (see below)
    if (!this.capturingAction || event.key === 'Escape') {
      return;
    }
    event.stopPropagation();
    event.preventDefault();
    this.lastKey = event.keyCode;
    this.buildTools.bind(this.capturingAction, event.keyCode);
    this.capturingAction = null;
  }

  ngOnInit(): void {
    this.isVersionOutdated = this.isGameVersionOutdated();
  }

  close() {
    this.done.next();
    this.sound.play(Sound.Ok);
  }

  private getVersionTagList(): Promise<Tag[]> {
    return lastValueFrom(
      this.http
        .get<Tag[]>(`${ environment.githubUrl }`));
  }

  async isGameVersionOutdated(): Promise<boolean> {
    let usedVersion = this.client.getVersion();

    const tags = await this.getVersionTagList();
    const usedVersionIndex = tags.findIndex(tag => tag.name === usedVersion);

    return usedVersionIndex > 0 || usedVersionIndex === -1;
  }

  @HostListener('window:keydown.escape', ['$event'])
  // @ts-ignore
  private activate(event: KeyboardEvent): void {
    event.stopPropagation();
    event.preventDefault();
    // Waiting for a key: Escape cancels that, not the settings
    if (this.capturingAction) {
      this.capturingAction = null;
      return;
    }
    this.close();
  }
}
