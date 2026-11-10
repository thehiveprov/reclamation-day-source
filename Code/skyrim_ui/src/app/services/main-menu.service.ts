import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { BehaviorSubject, Subject } from 'rxjs';

/** A save file, as the game lists it. */
export interface MainMenuSave {
  /** File name without extension, how the game names the save. */
  file: string;
  /** Character name. */
  name: string;
  /** "Save", "Autosave", "Quicksave" or "Exitsave". */
  kind: string;
  number: number;
  level: number;
  location: string;
  /** "0d.4h.4m" */
  playTime: string;
  /** Unix time in milliseconds. */
  modified: number;
  /** data: URL of the save's screenshot, empty if it has none. */
  thumbnail: string;
}

/** A co-op world: one multiplayer playthrough, tied to its server. */
export interface MainMenuWorld {
  id: string;
  name: string;
  /** Hosted by this player (their own server), or joined on someone else's. */
  hosted: boolean;
  /** Joined worlds: the server's "host:port". */
  address: string;
  characters: number;
}

/** A co-op character: the saves of one character id in one world. */
export interface MainMenuCharacter {
  id: string;
  worldId: string;
  saves: number;
  newest: MainMenuSave;
}

/** One of the game's own main menu entries, opened in the game's menu (settings, creations, credits, ...). */
export interface MainMenuNativeEntry {
  index: number;
  text: string;
}

export interface MainMenuData {
  /** Why the player is back at the main menu (e.g. lost the server), or what is being played. */
  status: string;
  /** The game has loaded its plugins: games can be started. */
  dataLoaded: boolean;
  /** Creations (mods) are loaded: the game warns that achievements are disabled. */
  creationsLoaded: boolean;
  /** Start Me Up Redux is installed: new characters skip the prologue. */
  startMeUp: boolean;
  /** The newest single-player save (CONTINUE), null without any. */
  continueSave: MainMenuSave | null;
  /** Single-player characters (LOAD), with an empty world id. */
  singlePlayerCharacters: MainMenuCharacter[];
  nativeEntries: MainMenuNativeEntry[];
  /** A world just created or joined, to continue creating a character in. */
  selectedWorldId: string;
  recentServers: string[];
  worlds: MainMenuWorld[];
  characters: MainMenuCharacter[];
  /** Mods Reclamation Day needs that aren't loaded. */
  missingRequirements?: MainMenuRequirement[];
  /** A newer release than this build, null when up to date. */
  update?: MainMenuUpdate | null;
  /**
   * The campaign this start plays (its plugins are the loaded ones): 'commonwealth' (2287) or 'winter-of-atom' (the
   * prequel). Empty without Winter of Atom installed: nothing to switch. The saves listed are this campaign's.
   */
  campaign?: string;
}

/** A newer release of Reclamation Day: installing it restarts the game. */
export interface MainMenuUpdate {
  version: string;
  /** This build's version. */
  current: string;
  state: 'available' | 'updating' | 'failed';
  /** Progress or error. */
  message: string;
}

/** A mod Reclamation Day needs (an F4SE plugin), with where to get it. */
export interface MainMenuRequirement {
  name: string;
  url: string;
}

export interface MainMenuSaves {
  worldId: string;
  characterId: string;
  saves: MainMenuSave[];
}

export interface MainMenuStatus {
  message: string;
  /** Waiting on the game (e.g. loading a save). */
  busy: boolean;
  error: boolean;
}

/**
 * Fallout 4's main menu in the overlay (title, single-player and multiplayer screens): the game sends the characters,
 * worlds and saves, and gets the player's choices back as actions.
 */
@Injectable({
  providedIn: 'root',
})
export class MainMenuService implements OnDestroy {
  readonly open$ = new BehaviorSubject<boolean>(false);
  readonly data$ = new BehaviorSubject<MainMenuData | null>(null);
  readonly saves$ = new BehaviorSubject<MainMenuSaves | null>(null);
  readonly sourceSaves$ = new BehaviorSubject<MainMenuSave[] | null>(null);
  readonly status$ = new BehaviorSubject<MainMenuStatus | null>(null);
  /** A world the game opened (a friend's joined through Steam): its id, to go on with a character in it. */
  readonly openWorld$ = new Subject<string>();

  constructor(private readonly zone: NgZone) {
    skyrimtogether.on('mainMenuOpen', this.onOpen.bind(this));
    skyrimtogether.on('mainMenuData', this.onData.bind(this));
    skyrimtogether.on('mainMenuSaves', this.onSaves.bind(this));
    skyrimtogether.on('mainMenuSourceSaves', this.onSourceSaves.bind(this));
    skyrimtogether.on('mainMenuStatus', this.onStatus.bind(this));
    skyrimtogether.on('mainMenuClose', this.onClose.bind(this));
    skyrimtogether.on('mainMenuOpenWorld', this.onOpenWorld.bind(this));

    // The game may have opened the screens before this page finished loading (it then stayed empty over the game's
    // main menu): it opens them again if they're open
    skyrimtogether.mainMenuAction('uiReady', '{}');
  }

  ngOnDestroy(): void {
    skyrimtogether.off('mainMenuOpen');
    skyrimtogether.off('mainMenuData');
    skyrimtogether.off('mainMenuSaves');
    skyrimtogether.off('mainMenuSourceSaves');
    skyrimtogether.off('mainMenuStatus');
    skyrimtogether.off('mainMenuClose');
  }

  /** Sends one of the player's choices to the game. */
  action(action: string, payload: Record<string, string> = {}): void {
    if (action !== 'refresh' && action !== 'listSaves' && action !== 'listSourceSaves') {
      this.status$.next(null);
    }
    skyrimtogether.mainMenuAction(action, JSON.stringify(payload));
  }

  private onOpen(json: string): void {
    this.zone.run(() => {
      this.status$.next(null);
      this.saves$.next(null);
      this.sourceSaves$.next(null);
      this.data$.next(JSON.parse(json) as MainMenuData);
      this.open$.next(true);
    });
  }

  private onOpenWorld(json: string): void {
    this.zone.run(() => this.openWorld$.next(JSON.parse(json) as string));
  }

  private onData(json: string): void {
    this.zone.run(() => this.data$.next(JSON.parse(json) as MainMenuData));
  }

  private onSaves(json: string): void {
    this.zone.run(() => this.saves$.next(JSON.parse(json) as MainMenuSaves));
  }

  private onSourceSaves(json: string): void {
    this.zone.run(() =>
      this.sourceSaves$.next((JSON.parse(json) as { saves: MainMenuSave[] }).saves),
    );
  }

  private onStatus(json: string): void {
    this.zone.run(() => this.status$.next(JSON.parse(json) as MainMenuStatus));
  }

  private onClose(): void {
    this.zone.run(() => {
      this.open$.next(false);
      this.status$.next(null);
    });
  }
}
