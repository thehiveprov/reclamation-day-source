import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { faUserGroup } from '@fortawesome/free-solid-svg-icons';
import { BehaviorSubject, Subscription } from 'rxjs';
import { ClientService } from './client.service';
import { PopupNotificationService } from './popup-notification.service';

/** A world's door for Steam friends: open to friends (the host asked first), invited friends only, or shut. */
export type SocialPrivacy = 'friends' | 'invite' | 'private';

/** A Steam friend as the game sees them. SteamIDs are text: 64 bits don't fit a number. */
export interface SocialFriend {
  id: string;
  name: string;
  state: 'online' | 'away' | 'busy' | 'snooze' | 'offline';
  /** "rd": in Reclamation Day, "fallout4": in Fallout 4 without it, "": neither. */
  game: 'rd' | 'fallout4' | '';
  /** Their Reclamation Day: "hosting", "playing", "menu". */
  status: string;
  world: string;
  privacy: string;
  /** They invited this player to the world they host. */
  invitedMe: boolean;
  /** Their build is this one (joining needs the same). */
  versionOk: boolean;
  canJoin: boolean;
  /** This player (the host) invited them. */
  invited: boolean;
}

/** A player in this world (the server's list), with their part in this player's party. */
export interface SocialPlayer {
  id: number;
  name: string;
  party: 'leader' | 'member' | '';
}

export interface SocialState {
  /** Steam is there for the game (a copy without Steam: none of this). */
  available: boolean;
  peerToPeer?: boolean;
  me?: { id: string; name: string };
  hosting?: { world: string; privacy: SocialPrivacy } | null;
  playing?: { world: string; host: string } | null;
  /** In a world (joining leaves it first). */
  inWorld?: boolean;
  /** The sidebar's key. */
  key?: string;
  friends: SocialFriend[];
  /** Friends asking to join the host's world. */
  requests: { id: string; name: string }[];
  /** Friends in the host's world through Steam. */
  joined: string[];
  /** The players in this world, the party first (none off a server). */
  players?: SocialPlayer[];
}

/** Steam asked to join a friend (an accepted invite, the friends list's Join Game) while in a world. */
export interface SocialJoinAsk {
  id: string;
  name: string;
  world: string;
}

/**
 * Steam friends (Fallout 4, docs/STEAM_SOCIAL.md): the social sidebar's state from the game (the social, socialAvatar,
 * socialToast, socialSidebar and socialJoinAsk events) and what's chosen in it back (social(...)). The sidebar opens
 * from the main menu's and the pause menu's SOCIAL, and with its key in the world (the game opens it then, the cursor
 * with it, and closing gives the cursor back).
 */
@Injectable({
  providedIn: 'root',
})
export class SocialService implements OnDestroy {
  readonly state$ = new BehaviorSubject<SocialState | null>(null);
  readonly open$ = new BehaviorSubject<boolean>(false);
  readonly avatars$ = new BehaviorSubject<Record<string, string>>({});
  readonly joinAsk$ = new BehaviorSubject<SocialJoinAsk | null>(null);

  /** Opened by the game (its key in the world): closing tells the game, which takes the cursor back. */
  private openedByGame = false;
  private readonly avatarsAsked = new Set<string>();
  private readonly activation: Subscription;

  constructor(
    private readonly zone: NgZone,
    private readonly client: ClientService,
    private readonly popups: PopupNotificationService,
  ) {
    skyrimtogether.on('social', this.onState.bind(this));
    skyrimtogether.on('socialAvatar', this.onAvatar.bind(this));
    skyrimtogether.on('socialToast', this.onToast.bind(this));
    skyrimtogether.on('socialSidebar', this.onSidebar.bind(this));
    skyrimtogether.on('socialJoinAsk', this.onJoinAsk.bind(this));
    // Escape closes the overlay in the world: the sidebar with it
    this.activation = this.client.activationStateChange.subscribe(active => {
      if (!active && this.open$.value && this.openedByGame) {
        this.close();
      }
    });
  }

  ngOnDestroy(): void {
    skyrimtogether.off('social');
    skyrimtogether.off('socialAvatar');
    skyrimtogether.off('socialToast');
    skyrimtogether.off('socialSidebar');
    skyrimtogether.off('socialJoinAsk');
    this.activation.unsubscribe();
  }

  /** From the main menu's or the pause menu's SOCIAL. */
  open(): void {
    this.openedByGame = false;
    this.open$.next(true);
    this.request('refresh');
  }

  close(): void {
    this.open$.next(false);
    this.joinAsk$.next(null);
    if (this.openedByGame) {
      this.openedByGame = false;
      this.request('close');
    }
  }

  /** Their world joined: the sidebar goes (the main menu's world page opens, or this world is left first). */
  join(id: string): void {
    this.close();
    this.request('join', JSON.stringify({ id }));
  }

  invite(id: string): void {
    this.request('invite', JSON.stringify({ id }));
  }

  answer(id: string, accept: boolean): void {
    this.request('answer', JSON.stringify({ id, accept: accept ? '1' : '0' }));
  }

  setPrivacy(value: SocialPrivacy): void {
    this.request('privacy', JSON.stringify({ value }));
  }

  /** Steam's overlay on their profile. */
  profile(id: string): void {
    this.request('profile', JSON.stringify({ id }));
  }

  /** The avatars of these friends, asked of the game once each. */
  wantAvatars(ids: string[]): void {
    const missing = ids.filter(id => !this.avatarsAsked.has(id));
    if (!missing.length) {
      return;
    }
    missing.forEach(id => this.avatarsAsked.add(id));
    this.request('avatars', JSON.stringify({ ids: missing.join(',') }));
  }

  private request(request: string, payload = ''): void {
    // The game's older builds don't have it
    if (typeof skyrimtogether.social === 'function') {
      skyrimtogether.social(request, payload);
    }
  }

  private parse<T>(json: string): T | null {
    try {
      return JSON.parse(json) as T;
    } catch {
      return null;
    }
  }

  private onState(json: string): void {
    this.zone.run(() => this.state$.next(this.parse<SocialState>(json)));
  }

  private onAvatar(json: string): void {
    const avatar = this.parse<{ id: string; url: string }>(json);
    if (!avatar) {
      return;
    }
    this.zone.run(() => this.avatars$.next({ ...this.avatars$.value, [avatar.id]: avatar.url }));
  }

  private onToast(json: string): void {
    const text = this.parse<string>(json);
    if (!text) {
      return;
    }
    this.zone.run(() =>
      this.popups.addMessage({
        messageKey: 'SERVICE.SOCIAL.TOAST',
        messageParams: { text },
        icon: faUserGroup,
        duration: 10000,
      }),
    );
  }

  private onSidebar(open: string): void {
    this.zone.run(() => {
      if (open === 'true') {
        this.openedByGame = true;
        this.open$.next(true);
      } else {
        this.openedByGame = false;
        this.open$.next(false);
        this.joinAsk$.next(null);
      }
    });
  }

  private onJoinAsk(json: string): void {
    this.zone.run(() => this.joinAsk$.next(this.parse<SocialJoinAsk>(json)));
  }
}
