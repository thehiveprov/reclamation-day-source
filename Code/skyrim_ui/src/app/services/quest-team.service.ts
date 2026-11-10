import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { faPeopleGroup } from '@fortawesome/free-solid-svg-icons';
import { BehaviorSubject } from 'rxjs';
import { PlayerListService } from './player-list.service';
import { PopupNotificationService } from './popup-notification.service';

/** Who's doing a quest together. */
export interface QuestTeam {
  /** Share the quest's progress. */
  participants: number[];
  /** Already did it (or got further on another path): their own progress stays as it is. */
  helpers: number[];
}

/** One of the local player's quests (what the Pip-Boy lists as running) or a quest they're on a team for. */
export interface Quest {
  /** Form id, hex. */
  id: string;
  name: string;
  /** The one tracked in the Pip-Boy. */
  active: boolean;
  team: QuestTeam | null;
  role: '' | 'participant' | 'helper';
}

export interface QuestInvite {
  questId: string;
  questName: string;
  inviterId: number;
}

/** Quests as the game has them (QuestService). */
export interface QuestsState {
  localPlayerId: number;
  connected: boolean;
  /** The party shares every main quest (the party leader's switch). */
  mainQuestSync: boolean;
  quests: Quest[];
  invites: QuestInvite[];
}

/** A quest that just showed up in the Pip-Boy. */
interface QuestStarted {
  questId: string;
  questName: string;
  /** Invites go to the party (otherwise to everyone). */
  party: boolean;
}

/**
 * Quest team-ups (Fallout 4, like Fallout 76): each player's quests are their own until they invite each other to do
 * one together. The game sends its state ("questsState"), invites ("questTeamInvite") and each new quest
 * ("questStarted", offered as a pop-up to invite the party); quests(request, payload) invites, accepts, declines,
 * leaves, and switches the party's main quest sync.
 */
@Injectable({
  providedIn: 'root',
})
export class QuestTeamService implements OnDestroy {
  readonly state$ = new BehaviorSubject<QuestsState | null>(null);

  constructor(
    private readonly zone: NgZone,
    private readonly playerListService: PlayerListService,
    private readonly popupNotificationService: PopupNotificationService,
  ) {
    skyrimtogether.on('questsState', this.onState.bind(this));
    skyrimtogether.on('questTeamInvite', this.onInvite.bind(this));
    skyrimtogether.on('questStarted', this.onQuestStarted.bind(this));
  }

  ngOnDestroy(): void {
    skyrimtogether.off('questsState');
    skyrimtogether.off('questTeamInvite');
    skyrimtogether.off('questStarted');
  }

  refresh(): void {
    this.request('get');
  }

  invite(questId: string, playerId: number): void {
    this.request('invite', `${questId}:${playerId}`);
  }

  /** Invites the party, or everyone without one. */
  inviteAll(questId: string): void {
    this.request('inviteAll', questId);
  }

  /** Party leader: share every main quest within the party, or stop. */
  setMainQuestSync(on: boolean): void {
    this.request('mainQuestSync', on ? '1' : '0');
  }

  accept(invite: QuestInvite): void {
    this.request('accept', `${invite.questId}:${invite.inviterId}`);
  }

  decline(invite: QuestInvite): void {
    this.request('decline', `${invite.questId}:${invite.inviterId}`);
  }

  leave(questId: string): void {
    this.request('leave', questId);
  }

  /** A player's name, or "Player <id>" when the list doesn't have them. */
  playerName(playerId: number): string {
    const player = this.playerListService.playerList
      .getValue()
      ?.players.find(entry => entry.id === playerId);
    return player?.name ?? `Player ${playerId}`;
  }

  private request(request: string, payload = ''): void {
    // The game's older builds don't have it
    if (typeof skyrimtogether.quests === 'function') {
      skyrimtogether.quests(request, payload);
    }
  }

  private onState(json: string): void {
    this.zone.run(() => this.state$.next(JSON.parse(json) as QuestsState));
  }

  private onQuestStarted(json: string): void {
    const quest = JSON.parse(json) as QuestStarted;
    this.zone.run(() =>
      this.popupNotificationService.addMessage({
        messageKey: 'SERVICE.QUEST_TEAM.NEW_QUEST',
        messageParams: { quest: quest.questName },
        icon: faPeopleGroup,
        duration: 45000,
        actions: [
          {
            nameKey: quest.party
              ? 'SERVICE.QUEST_TEAM.INVITE_PARTY'
              : 'SERVICE.QUEST_TEAM.INVITE_EVERYONE',
            callback: () => this.inviteAll(quest.questId),
          },
          // Clicking the pop-up closes it
          { nameKey: 'SERVICE.QUEST_TEAM.NOT_NOW', callback: () => {} },
        ],
      }),
    );
  }

  private onInvite(json: string): void {
    const invite = JSON.parse(json) as QuestInvite;
    this.zone.run(() =>
      this.popupNotificationService.addMessage({
        messageKey: 'SERVICE.QUEST_TEAM.INVITE',
        messageParams: {
          from: this.playerName(invite.inviterId),
          quest: invite.questName,
        },
        icon: faPeopleGroup,
        duration: 60000,
        actions: [
          {
            nameKey: 'COMPONENT.NOTIFICATIONS.ACCEPT',
            callback: () => this.accept(invite),
          },
          {
            nameKey: 'COMPONENT.NOTIFICATIONS.DECLINE',
            callback: () => this.decline(invite),
          },
        ],
      }),
    );
  }
}
