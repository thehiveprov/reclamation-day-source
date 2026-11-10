import { ChangeDetectionStrategy, Component, OnInit } from '@angular/core';
import { combineLatest, map } from 'rxjs';
import { PlayerListService } from '../../services/player-list.service';
import {
  Quest,
  QuestInvite,
  QuestTeamService,
} from '../../services/quest-team.service';
import { Sound, SoundService } from '../../services/sound.service';

/**
 * Player manager > Quests: do quests together. Lists the local player's quests with who they're doing each one with,
 * invites other players to one, and the invites received.
 */
@Component({
  selector: 'app-quest-team',
  templateUrl: './quest-team.component.html',
  styleUrls: ['./quest-team.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class QuestTeamComponent implements OnInit {
  /** The quest whose invite list is open. */
  inviting: string | null = null;

  readonly view$ = combineLatest([
    this.questTeamService.state$,
    this.playerListService.playerList,
  ]).pipe(
    map(([state, playerList]) => {
      const localId = state?.localPlayerId ?? -1;
      const others = (playerList?.players ?? []).filter(
        player => player.id !== localId,
      );
      return { state, others };
    }),
  );

  constructor(
    readonly questTeamService: QuestTeamService,
    private readonly playerListService: PlayerListService,
    private readonly sound: SoundService,
  ) {}

  ngOnInit(): void {
    this.questTeamService.refresh();
  }

  /** The other players on the quest's team, by name. */
  teammates(quest: Quest, localId: number, helpers: boolean): string {
    const ids = (helpers ? quest.team?.helpers : quest.team?.participants) ?? [];
    return ids
      .filter(id => id !== localId)
      .map(id => this.questTeamService.playerName(id))
      .join(', ');
  }

  /** Players not on the quest's team yet. */
  invitable<T extends { id: number }>(quest: Quest, others: T[]): T[] {
    const team = quest.team;
    return others.filter(
      player =>
        !team ||
        (!team.participants.includes(player.id) &&
          !team.helpers.includes(player.id)),
    );
  }

  toggleInvite(quest: Quest): void {
    this.inviting = this.inviting === quest.id ? null : quest.id;
    this.sound.play(Sound.Focus);
  }

  invite(quest: Quest, playerId: number): void {
    this.questTeamService.invite(quest.id, playerId);
    this.inviting = null;
    this.sound.play(Sound.Ok);
  }

  leave(quest: Quest): void {
    this.questTeamService.leave(quest.id);
    this.sound.play(Sound.Ok);
  }

  accept(invite: QuestInvite): void {
    this.questTeamService.accept(invite);
    this.sound.play(Sound.Ok);
  }

  decline(invite: QuestInvite): void {
    this.questTeamService.decline(invite);
    this.sound.play(Sound.Ok);
  }

  trackQuest(_: number, quest: Quest): string {
    return quest.id;
  }
}
