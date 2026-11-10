import {
  ChangeDetectionStrategy,
  ChangeDetectorRef,
  Component,
  HostListener,
  OnDestroy,
} from '@angular/core';
import { TranslocoService } from '@ngneat/transloco';
import { combineLatest, Subscription } from 'rxjs';
import {
  MainMenuCharacter,
  MainMenuData,
  MainMenuNativeEntry,
  MainMenuSave,
  MainMenuService,
  MainMenuWorld,
} from '../../services/main-menu.service';
import { LanServer, LanServersService } from '../../services/lan-servers.service';
import { SocialService } from '../../services/social.service';

type Screen =
  | 'title'
  | 'singlePlayer'
  | 'characters'
  | 'saves'
  | 'worlds'
  | 'newWorld'
  | 'joinServer'
  | 'start'
  | 'sourceSaves';

/** An "are you sure?" before something the game can't take back (as the game's own menu asks). */
interface Confirmation {
  message: string;
  note?: string;
  yes: () => void;
}

/**
 * Fallout 4's main menu in the overlay. The title (CONTINUE, NEW GAME, LOAD, MULTIPLAYER, the game's other entries,
 * QUIT); single-player LOAD and MULTIPLAYER as character cards, then a character's saves as cards; creating a co-op
 * character in a world (a named hosted world, or a server joined), each world tied to its server.
 */
@Component({
  selector: 'app-multiplayer-menu',
  templateUrl: './multiplayer-menu.component.html',
  styleUrls: ['./multiplayer-menu.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class MultiplayerMenuComponent implements OnDestroy {
  screen: Screen = 'title';

  /** The character whose saves are shown (an empty world id for single-player ones). */
  character: MainMenuCharacter | null = null;

  /** The world a new co-op character is created in. */
  worldId = '';

  worldName = '';
  serverAddress = '';
  serverName = '';

  /** Joining a server: the ones on the local network, or public ones (coming). */
  joinTab: 'lan' | 'public' = 'lan';

  confirmation: Confirmation | null = null;

  readonly open$ = this.menu.open$;
  readonly data$ = this.menu.data$;
  readonly saves$ = this.menu.saves$;
  readonly sourceSaves$ = this.menu.sourceSaves$;
  readonly status$ = this.menu.status$;
  readonly lanServers$ = this.lan.servers$;
  readonly lanSearching$ = this.lan.searching$;
  readonly lanSearched$ = this.lan.searched$;

  private readonly subscriptions = new Subscription();

  constructor(
    private readonly menu: MainMenuService,
    private readonly social: SocialService,
    private readonly lan: LanServersService,
    private readonly transloco: TranslocoService,
    private readonly changeDetector: ChangeDetectorRef,
  ) {
    this.subscriptions.add(
      this.menu.open$.subscribe(open => {
        if (open) {
          this.screen = 'title';
          this.character = null;
          this.confirmation = null;
          this.changeDetector.markForCheck();
        }
      }),
    );

    // A friend's world joined through Steam (the social sidebar, an invite): go on with the character in it
    this.subscriptions.add(
      this.menu.openWorld$.subscribe(worldId => {
        this.worldId = worldId;
        this.screen = 'start';
        this.changeDetector.markForCheck();
      }),
    );

    // A world just created or joined: go on with the character in it
    this.subscriptions.add(
      this.menu.data$.subscribe(data => {
        if (
          data?.selectedWorldId &&
          (this.screen === 'newWorld' || this.screen === 'joinServer')
        ) {
          this.worldId = data.selectedWorldId;
          this.screen = 'start';
        }
        this.changeDetector.markForCheck();
      }),
    );

    this.subscriptions.add(
      combineLatest([this.saves$, this.sourceSaves$, this.status$]).subscribe(() =>
        this.changeDetector.markForCheck(),
      ),
    );
  }

  ngOnDestroy(): void {
    this.subscriptions.unsubscribe();
  }

  @HostListener('document:keydown.escape')
  onEscape(): void {
    if (!this.menu.open$.getValue() || this.menu.status$.getValue()?.busy) {
      return;
    }
    if (this.confirmation) {
      this.confirmation = null;
    } else if (this.screen !== 'title') {
      this.back();
    }
    this.changeDetector.markForCheck();
  }

  @HostListener('document:keydown.enter', ['$event'])
  onEnter(event: Event): void {
    if (this.confirmation) {
      // Not also a click on whatever is focused behind the prompt (the card that asked)
      event.preventDefault();
      this.confirm();
    }
  }

  /** The "are you sure?" was accepted. */
  confirm(): void {
    const yes = this.confirmation?.yes;
    this.confirmation = null;
    yes?.();
    this.changeDetector.markForCheck();
  }

  cancelConfirmation(): void {
    this.confirmation = null;
  }

  private ask(message: string, yes: () => void, note?: string): void {
    this.confirmation = { message, note, yes };
    this.changeDetector.markForCheck();
  }

  private text(key: string, params?: Record<string, unknown>): string {
    return this.transloco.translate(`COMPONENT.MULTIPLAYER_MENU.${key}`, params);
  }

  /** The game's note when creations are loaded, on its new game prompt. */
  private creationsNote(data: MainMenuData): string | undefined {
    return data.creationsLoaded ? this.text('CREATIONS_NOTE') : undefined;
  }

  world(data: MainMenuData, id: string): MainMenuWorld | undefined {
    return data.worlds.find(world => world.id === id);
  }

  // Title

  continueGame(): void {
    this.ask(this.text('CONFIRM_CONTINUE'), () => this.menu.action('continue'));
  }

  newGame(data: MainMenuData): void {
    this.ask(
      this.text('CONFIRM_NEW'),
      () => this.menu.action('newGame'),
      this.creationsNote(data),
    );
  }

  openSinglePlayer(): void {
    this.screen = 'singlePlayer';
  }

  openMultiplayer(): void {
    this.screen = 'characters';
  }

  /** The social sidebar: Steam friends to join or invite. */
  openSocial(): void {
    this.social.open();
  }

  openNative(entry: MainMenuNativeEntry): void {
    this.menu.action('native', { index: String(entry.index) });
  }

  quit(): void {
    this.ask(this.text('CONFIRM_QUIT'), () => this.menu.action('quit'));
  }

  /** Out of the prequel: 2287's plugins load at the next start, so the game restarts. */
  backTo2287(): void {
    this.ask(
      this.text('CONFIRM_CAMPAIGN_2287'),
      () => this.menu.action('switchCampaign', { campaign: 'commonwealth' }),
      this.text('CAMPAIGN_NOTE'),
    );
  }

  // Characters and saves

  openCharacter(character: MainMenuCharacter): void {
    this.character = character;
    this.menu.saves$.next(null);
    this.menu.action('listSaves', {
      worldId: character.worldId,
      characterId: character.id,
    });
    this.screen = 'saves';
  }

  play(save: MainMenuSave): void {
    const character = this.character;
    if (character) {
      this.ask(this.text('CONFIRM_LOAD'), () =>
        this.menu.action('play', { worldId: character.worldId, save: save.file }),
      );
    }
  }

  /** The save to the Recycle Bin; the game sends the character's saves again. */
  deleteSave(save: MainMenuSave): void {
    const character = this.character;
    if (character) {
      this.ask(
        this.text('CONFIRM_DELETE'),
        () =>
          this.menu.action('deleteSave', {
            worldId: character.worldId,
            characterId: character.id,
            save: save.file,
          }),
        this.text('DELETE_NOTE'),
      );
    }
  }

  // Creating a co-op character

  createCharacter(): void {
    this.screen = 'worlds';
  }

  chooseWorld(world: MainMenuWorld): void {
    this.worldId = world.id;
    this.screen = 'start';
  }

  newWorld(): void {
    this.worldName = '';
    this.screen = 'newWorld';
  }

  submitWorld(): void {
    const name = this.worldName.trim();
    if (name) {
      this.menu.action('createWorld', { name });
    }
  }

  joinServer(recentServers: string[]): void {
    this.serverAddress = recentServers[0] ?? '';
    this.serverName = '';
    this.screen = 'joinServer';
    this.showLan();
  }

  showLan(): void {
    this.joinTab = 'lan';
    this.searchLan();
  }

  searchLan(): void {
    this.lan.search();
  }

  /** One of the servers on the network: joined like one typed in, under its own name. */
  joinLan(server: LanServer): void {
    this.serverAddress = server.address;
    this.serverName = server.name;
    this.submitServer();
  }

  submitServer(): void {
    const address = this.serverAddress.trim();
    if (address) {
      this.menu.action('addServer', { address, name: this.serverName.trim() });
    }
  }

  startNewGame(data: MainMenuData): void {
    const worldId = this.worldId;
    this.ask(
      this.text('CONFIRM_NEW_COOP', {
        world: this.world(data, worldId)?.name ?? '',
      }),
      () => this.menu.action('newCharacter', { worldId }),
      this.creationsNote(data),
    );
  }

  startFromSave(): void {
    this.menu.sourceSaves$.next(null);
    this.menu.action('listSourceSaves');
    this.screen = 'sourceSaves';
  }

  pickSourceSave(save: MainMenuSave): void {
    const worldId = this.worldId;
    this.ask(this.text('CONFIRM_FROM_SAVE'), () =>
      this.menu.action('newCharacterFrom', { worldId, save: save.file }),
    );
  }

  exportCrashReports(): void {
    this.menu.action('exportCrashReports');
  }

  /** Downloads the newer release, then the game closes, installs it and starts again. */
  installUpdate(): void {
    this.menu.action('update');
  }

  /** A missing mod's download page, in the player's browser. */
  openUrl(url: string): void {
    this.menu.action('openUrl', { url });
  }

  back(): void {
    switch (this.screen) {
      case 'singlePlayer':
      case 'characters':
        this.screen = 'title';
        break;
      case 'saves':
        this.screen =
          this.character?.worldId === '' ? 'singlePlayer' : 'characters';
        break;
      case 'worlds':
        this.screen = 'characters';
        break;
      case 'newWorld':
      case 'joinServer':
      case 'start':
        this.screen = 'worlds';
        break;
      case 'sourceSaves':
        this.screen = 'start';
        break;
    }
    this.menu.status$.next(null);
  }

  /** "Save 3", "Autosave", ... as the label of a save card. */
  kindKey(save: MainMenuSave): string {
    switch (save.kind.toLowerCase()) {
      case 'autosave':
        return 'KIND_AUTOSAVE';
      case 'quicksave':
        return 'KIND_QUICKSAVE';
      case 'exitsave':
        return 'KIND_EXITSAVE';
      default:
        return 'KIND_SAVE';
    }
  }

  /** "0d.4h.4m" -> "4h 4m" (with days when there are some). */
  playTime(value: string): string {
    const match = /^(\d+)d\.(\d+)h\.(\d+)m/.exec(value);
    if (!match) {
      return value;
    }
    const [, days, hours, minutes] = match;
    return (
      (days !== '0' ? `${days}d ` : '') +
      (days !== '0' || hours !== '0' ? `${hours}h ` : '') +
      `${minutes}m`
    );
  }

  date(milliseconds: number): string {
    return new Date(milliseconds).toLocaleString(undefined, {
      month: 'short',
      day: 'numeric',
      hour: 'numeric',
      minute: '2-digit',
    });
  }

  trackById(_: number, item: { id: string }): string {
    return item.id;
  }

  trackByCharacter(_: number, character: MainMenuCharacter): string {
    return character.worldId + '/' + character.id;
  }

  trackByFile(_: number, save: MainMenuSave): string {
    return save.file;
  }
}
