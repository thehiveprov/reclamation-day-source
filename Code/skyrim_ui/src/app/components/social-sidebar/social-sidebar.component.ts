import { ChangeDetectionStrategy, Component, OnDestroy, OnInit } from '@angular/core';
import { Subscription } from 'rxjs';
import { SocialFriend, SocialPlayer, SocialPrivacy, SocialService, SocialState } from '../../services/social.service';

interface FriendGroup {
  key: 'rd' | 'fallout4' | 'online' | 'offline';
  label: string;
  friends: SocialFriend[];
}

/**
 * The social sidebar (like Fallout 76's friends panel): Steam friends, who's hosting what, joining a
 * friend's world and inviting friends to one's own, the friends asking to join, the hosted world's privacy. Slides in
 * from the right over the main menu, the pause menu or the world.
 */
@Component({
  selector: 'app-social-sidebar',
  templateUrl: './social-sidebar.component.html',
  styleUrls: ['./social-sidebar.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class SocialSidebarComponent implements OnInit, OnDestroy {
  readonly open$ = this.social.open$;
  readonly state$ = this.social.state$;
  readonly avatars$ = this.social.avatars$;
  readonly joinAsk$ = this.social.joinAsk$;

  readonly privacies: { value: SocialPrivacy; label: string; hint: string }[] = [
    { value: 'friends', label: 'FRIENDS', hint: 'Steam friends can ask to join. You let each one in.' },
    { value: 'invite', label: 'INVITE ONLY', hint: 'Only friends you invite can join.' },
    { value: 'private', label: 'PRIVATE', hint: 'No one joins through Steam. Joining by address still works.' },
  ];

  /** Groups folded away (offline starts folded). */
  collapsed: Record<string, boolean> = { offline: true };
  /** A friend whose world is about to be joined from inside a world: asked first. */
  confirmJoin: SocialFriend | null = null;

  private stateSubscription?: Subscription;
  private readonly onKey = (event: KeyboardEvent) => {
    if (event.key !== 'Escape' || !this.social.open$.value) {
      return;
    }
    // (before the menus under it see it)
    event.stopImmediatePropagation();
    event.preventDefault();
    if (this.confirmJoin) {
      this.confirmJoin = null;
    } else {
      this.close();
    }
  };

  constructor(private readonly social: SocialService) {}

  ngOnInit(): void {
    window.addEventListener('keydown', this.onKey, true);
    // the avatars of the friends shown (online first: offline friends are folded away)
    this.stateSubscription = this.social.state$.subscribe(state => {
      if (state?.friends) {
        this.social.wantAvatars(state.friends.slice(0, 80).map(friend => friend.id));
      }
      for (const request of state?.requests ?? []) {
        this.social.wantAvatars([request.id]);
      }
    });
  }

  ngOnDestroy(): void {
    window.removeEventListener('keydown', this.onKey, true);
    this.stateSubscription?.unsubscribe();
  }

  close(): void {
    this.confirmJoin = null;
    this.social.close();
  }

  groups(state: SocialState): FriendGroup[] {
    const groups: FriendGroup[] = [
      { key: 'rd', label: 'IN RECLAMATION DAY', friends: [] },
      { key: 'fallout4', label: 'IN FALLOUT 4', friends: [] },
      { key: 'online', label: 'ONLINE', friends: [] },
      { key: 'offline', label: 'OFFLINE', friends: [] },
    ];
    for (const friend of state.friends) {
      const key = friend.game === 'rd' ? 'rd' : friend.game === 'fallout4' ? 'fallout4' : friend.state === 'offline' ? 'offline' : 'online';
      groups.find(group => group.key === key)!.friends.push(friend);
    }
    return groups.filter(group => group.friends.length);
  }

  statusText(friend: SocialFriend): string {
    const version = friend.versionOk ? '' : ' · a different version';
    if (friend.game === 'rd') {
      if (friend.status === 'hosting') {
        const world = friend.world || 'a world';
        if (friend.invitedMe) {
          return `Invited you to ${world}${version}`;
        }
        const door = friend.privacy === 'invite' ? ' · invite only' : friend.privacy === 'private' ? ' · private' : '';
        return `Hosting ${world}${door}${version}`;
      }
      if (friend.status === 'playing') {
        return `Playing in ${friend.world || 'a world'}${version}`;
      }
      return `Main menu${version}`;
    }
    if (friend.game === 'fallout4') {
      return 'Playing Fallout 4';
    }
    switch (friend.state) {
      case 'away':
        return 'Away';
      case 'busy':
        return 'Busy';
      case 'snooze':
        return 'Snooze';
      case 'offline':
        return 'Offline';
      default:
        return 'Online';
    }
  }

  partyText(player: SocialPlayer): string {
    switch (player.party) {
      case 'leader':
        return "Your party's leader";
      case 'member':
        return 'In your party';
      default:
        return 'Not in your party';
    }
  }

  privacyHint(privacy: SocialPrivacy): string {
    return this.privacies.find(entry => entry.value === privacy)?.hint ?? '';
  }

  toggle(key: string): void {
    this.collapsed = { ...this.collapsed, [key]: !this.collapsed[key] };
  }

  join(friend: SocialFriend, state: SocialState): void {
    // in a world: it's left (saved first) to join, so asked
    if (state.inWorld) {
      this.confirmJoin = friend;
      return;
    }
    this.social.join(friend.id);
  }

  confirm(): void {
    if (this.confirmJoin) {
      this.social.join(this.confirmJoin.id);
      this.confirmJoin = null;
    }
  }

  joinAsked(id: string): void {
    this.social.join(id);
  }

  dismissAsk(): void {
    this.joinAsk$.next(null);
  }

  invite(friend: SocialFriend): void {
    this.social.invite(friend.id);
  }

  answer(id: string, accept: boolean): void {
    this.social.answer(id, accept);
  }

  setPrivacy(value: SocialPrivacy): void {
    this.social.setPrivacy(value);
  }

  profile(friend: SocialFriend): void {
    this.social.profile(friend.id);
  }

  trackFriend(_: number, friend: { id: string }): string {
    return friend.id;
  }

  trackPlayer(_: number, player: SocialPlayer): number {
    return player.id;
  }

  trackGroup(_: number, group: FriendGroup): string {
    return group.key;
  }
}
