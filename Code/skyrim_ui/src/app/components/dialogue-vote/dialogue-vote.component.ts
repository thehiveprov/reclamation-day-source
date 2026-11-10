import {
  ChangeDetectionStrategy,
  Component,
  HostListener,
} from '@angular/core';
import { combineLatest, interval, map, startWith } from 'rxjs';
import {
  DialogueVote,
  DialogueVoteService,
} from '../../services/dialogue-vote.service';

/** XDI's speech challenge levels: easy, medium, hard, always succeeds, then the repeatable easy, medium, hard. */
const CHALLENGES = ['', 'EASY', 'MEDIUM', 'HARD', 'SURE', 'EASY', 'MEDIUM', 'HARD'];

/**
 * Co-op dialogue (Fallout 4): the party's vote on the options of the player talking to an NPC. A voter who hasn't voted
 * sees the options with their keys (1-9, or a click; "Don't vote" lets it go) and each one's full line; then, and for
 * the talker (who votes in the game's dialogue menu), the card is the tally at the top of the screen: who voted for
 * what, the time left, and the outcome at the end. Escape closes the overlay, which lets the vote go too.
 */
@Component({
  selector: 'app-dialogue-vote',
  templateUrl: './dialogue-vote.component.html',
  styleUrls: ['./dialogue-vote.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class DialogueVoteComponent {
  /** The card with the seconds left, ticking. */
  readonly view$ = combineLatest([this.votes.card$, interval(250).pipe(startWith(0))]).pipe(
    map(([card]) => (card ? { card, seconds: Math.max(0, Math.ceil(((card.endsAt ?? Date.now()) - Date.now()) / 1000)) } : null)),
  );

  constructor(private readonly votes: DialogueVoteService) {}

  talkerName(card: DialogueVote): string {
    return card.voters.find(voter => voter.talker)?.name ?? '';
  }

  myChoice(card: DialogueVote): number {
    return card.voters.find(voter => voter.you)?.choice ?? -1;
  }

  /** Who voted for an option, by name. */
  votersFor(card: DialogueVote, index: number): string[] {
    return card.voters.filter(voter => voter.choice === index).map(voter => voter.name);
  }

  waitingFor(card: DialogueVote): number {
    return card.voters.filter(voter => voter.choice < 0).length;
  }

  challenge(level: number): string {
    return CHALLENGES[level] ? `COMPONENT.DIALOGUE_VOTE.CHALLENGE_${CHALLENGES[level]}` : '';
  }

  cast(card: DialogueVote, index: number): void {
    if (card.input && card.stage !== 'result') {
      this.votes.cast(index);
    }
  }

  letGo(): void {
    this.votes.letGo();
  }

  @HostListener('window:keydown', ['$event'])
  onKey(event: KeyboardEvent): void {
    const card = this.votes.card$.value;
    if (!card?.input) {
      return;
    }
    const match = /^(?:Digit|Numpad)([1-9])$/.exec(event.code);
    if (match) {
      event.preventDefault();
      this.cast(card, parseInt(match[1], 10) - 1);
    }
  }
}
