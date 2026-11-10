import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { BehaviorSubject, Subscription } from 'rxjs';
import { ClientService } from './client.service';

/** One of the talker's options, as their dialogue menu shows it. */
export interface DialogueVoteOption {
  prompt: string;
  /** What the player character says ("" when the game has no line for it). */
  line: string;
  /** A speech challenge: 0 none, 1 easy, 2 medium, 3 hard, 4 always succeeds, 5-7 the repeatable ones. */
  challenge: number;
  /** False: its conditions fail, it can't be chosen. */
  enabled: boolean;
}

export interface DialogueVoter {
  name: string;
  /** The option's index they voted for, -1 not yet. */
  choice: number;
  talker: boolean;
  you: boolean;
}

/** A vote on the talker's dialogue options (the game's dialogueVote event). */
export interface DialogueVote {
  voteId: number;
  stage: 'open' | 'tally' | 'result' | 'closed';
  /** This player is the one talking (they vote in the game's dialogue menu). */
  talker: boolean;
  npc: string;
  options: DialogueVoteOption[];
  /** The talker first. */
  voters: DialogueVoter[];
  /** When the server sent it. */
  msLeft: number;
  /** The option chosen (stage "result"), -1 nobody voted. */
  result: number;
  hostBrokeTie: boolean;
  /** The card has the mouse and keys (a voter yet to vote). */
  input: boolean;
  /** When it ends (Date.now() based), worked out on arrival. */
  endsAt?: number;
}

/** How long the outcome stays up. */
const RESULT_MS = 4000;

/**
 * Co-op dialogue (Fallout 4): the party votes on the options of the player talking to an NPC. A voter's card takes the
 * input until they vote (1-9 or a click), then stays as the tally; the talker sees the tally while they vote in the
 * game's dialogue menu. The outcome shows a moment, then the card goes. Escape (which closes the overlay) lets the vote
 * go for this player.
 */
@Injectable({
  providedIn: 'root',
})
export class DialogueVoteService implements OnDestroy {
  readonly card$ = new BehaviorSubject<DialogueVote | null>(null);

  private readonly activation: Subscription;
  private resultTimer?: ReturnType<typeof setTimeout>;

  constructor(
    private readonly zone: NgZone,
    private readonly client: ClientService,
  ) {
    skyrimtogether.on('dialogueVote', this.onVote.bind(this));
    this.activation = this.client.activationStateChange.subscribe(active => {
      const card = this.card$.value;
      if (!active && card?.input) {
        this.letGo();
      }
    });
  }

  ngOnDestroy(): void {
    skyrimtogether.off('dialogueVote');
    this.activation.unsubscribe();
    clearTimeout(this.resultTimer);
  }

  cast(index: number): void {
    const card = this.card$.value;
    if (!card || card.talker || !card.options[index]?.enabled) {
      return;
    }
    this.request('cast', String(index));
  }

  /** No vote from this player: the card stays as the tally, without the input. */
  letGo(): void {
    const card = this.card$.value;
    if (card) {
      this.card$.next({ ...card, input: false });
    }
    this.request('close');
  }

  private request(request: string, payload = ''): void {
    // The game's older builds don't have it
    if (typeof skyrimtogether.dialogueVote === 'function') {
      skyrimtogether.dialogueVote(request, payload);
    }
  }

  private onVote(json: string): void {
    this.zone.run(() => {
      const vote = JSON.parse(json) as DialogueVote | null;
      clearTimeout(this.resultTimer);
      if (!vote || vote.stage === 'closed') {
        this.card$.next(null);
        return;
      }
      vote.endsAt = Date.now() + vote.msLeft;
      this.card$.next(vote);
      if (vote.stage === 'result') {
        this.resultTimer = setTimeout(() => this.zone.run(() => this.card$.next(null)), RESULT_MS);
      }
    });
  }
}
