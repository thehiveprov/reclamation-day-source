import {
  ChangeDetectionStrategy,
  ChangeDetectorRef,
  Component,
  HostListener,
} from '@angular/core';
import {
  NemesesService,
  NemesisQuestion,
  NemesisQuestionCard,
  pronouns,
} from '../../services/nemeses.service';
import { Sound, SoundService } from '../../services/sound.service';

type Menu = 'main' | 'who' | 'where' | 'when' | 'why';

interface Choice {
  key: string;
  params?: Record<string, unknown>;
  question?: NemesisQuestion;
  caps?: number;
  menu?: Menu;
  close?: boolean;
}

/** The questions under each word, in order. */
const MENUS: Record<Exclude<Menu, 'main'>, Choice[]> = {
  who: [
    { key: 'COMPONENT.NEMESIS_CARD.ASK_BOSS', question: NemesisQuestion.AskBoss },
    { key: 'COMPONENT.NEMESIS_CARD.ASK_KIN', question: NemesisQuestion.AskKin },
  ],
  where: [
    { key: 'COMPONENT.NEMESIS_CARD.ASK_CAMP', question: NemesisQuestion.AskCamp },
    { key: 'COMPONENT.NEMESIS_CARD.ASK_STASH', question: NemesisQuestion.AskStash },
  ],
  when: [{ key: 'COMPONENT.NEMESIS_CARD.ASK_HABITS', question: NemesisQuestion.AskHabits }],
  why: [{ key: 'COMPONENT.NEMESIS_CARD.ASK_WHY', question: NemesisQuestion.AskWhy }],
};

/**
 * Fallout 4 nemeses: questioning a gang man who surrendered, at gunpoint. While the player's weapon is on him, his bar
 * shows at the top (his name, gang, fear and what the player sees on him); talking to him opens the choices at the
 * bottom: ask who, where, when or why, pay him, threaten him or let him go, and finishing him on its own key (F).
 * What he gives up goes to the nemesis screen and the Pip-Boy. Keys, none of them the game's movement: the arrows
 * choose (left goes back, right takes), 1-9 take a choice at once, E or Enter takes, Backspace goes back, F finishes
 * him; the mouse too; Escape leaves (the game closes the overlay, the service closes the card). The words follow the
 * man or woman questioned (card.female).
 */
@Component({
  selector: 'app-nemesis-card',
  templateUrl: './nemesis-card.component.html',
  styleUrls: ['./nemesis-card.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class NemesisCardComponent {
  readonly Question = NemesisQuestion;
  readonly card$ = this.nemeses.card$;
  readonly gunpoint$ = this.nemeses.gunpoint$;

  menu: Menu = 'main';
  selected = 0;

  constructor(
    private readonly nemeses: NemesesService,
    private readonly sound: SoundService,
    private readonly changes: ChangeDetectorRef,
  ) {}

  /** The caps he asked for to walk ("120 caps for his life"), 0 if none. */
  offer(card: NemesisQuestionCard): number {
    const offer = card.revealed.find(item => item.kind === 'offer');
    return offer ? parseInt(offer.text, 10) || 0 : 0;
  }

  choices(card: NemesisQuestionCard): Choice[] {
    if (card.done || card.error) {
      return [{ key: 'COMPONENT.NEMESIS_CARD.CLOSE', close: true }];
    }
    if (this.menu !== 'main') {
      return [...MENUS[this.menu], { key: 'COMPONENT.NEMESIS_CARD.BACK', menu: 'main' }];
    }
    const choices: Choice[] = [
      { key: 'COMPONENT.NEMESIS_CARD.WHO', menu: 'who' },
      { key: 'COMPONENT.NEMESIS_CARD.WHERE', menu: 'where' },
      { key: 'COMPONENT.NEMESIS_CARD.WHEN', menu: 'when' },
      { key: 'COMPONENT.NEMESIS_CARD.WHY', menu: 'why' },
    ];
    const caps = this.offer(card);
    const words = pronouns(card.female);
    if (caps) {
      choices.push({ key: 'COMPONENT.NEMESIS_CARD.PAY', params: { caps, ...words }, question: NemesisQuestion.Pay, caps });
    }
    choices.push(
      { key: 'COMPONENT.NEMESIS_CARD.THREATEN', params: words, question: NemesisQuestion.Threaten },
      { key: 'COMPONENT.NEMESIS_CARD.LET_GO', params: words, question: NemesisQuestion.LetHimGo },
    );
    return choices;
  }

  /** {{he}}, {{him}}, {{his}} for the card's own words. */
  words(card: NemesisQuestionCard): Record<string, string> {
    return pronouns(card.female);
  }

  choose(card: NemesisQuestionCard, choice: Choice): void {
    if (choice.close) {
      this.close();
      return;
    }
    if (choice.menu) {
      this.menu = choice.menu;
      this.selected = 0;
      this.sound.play(Sound.Focus);
      this.changes.markForCheck();
      return;
    }
    if (choice.question !== undefined) {
      this.nemeses.ask(choice.question, choice.caps ?? 0);
      this.sound.play(Sound.Ok);
      this.menu = 'main';
      this.selected = 0;
      this.changes.markForCheck();
    }
  }

  hover(index: number): void {
    if (this.selected !== index) {
      this.selected = index;
      this.changes.markForCheck();
    }
  }

  finish(): void {
    this.nemeses.ask(NemesisQuestion.FinishHim);
    this.sound.play(Sound.Cancel);
    this.reset();
  }

  close(): void {
    this.nemeses.close();
    this.sound.play(Sound.Cancel);
    this.reset();
  }

  @HostListener('window:keydown', ['$event'])
  // @ts-ignore
  private onKey(event: KeyboardEvent): void {
    const card = this.nemeses.card$.value;
    if (!card || this.nemeses.screen$.value) {
      return;
    }
    const choices = this.choices(card);
    const key = event.key.toLowerCase();
    // 1-9: that choice at once
    if (key >= '1' && key <= '9' && key.length === 1) {
      const index = parseInt(key, 10) - 1;
      if (index < choices.length) {
        this.selected = index;
        this.choose(card, choices[index]);
      }
      event.preventDefault();
      event.stopPropagation();
      this.changes.markForCheck();
      return;
    }
    switch (key) {
      case 'arrowup':
        this.selected = (this.selected + choices.length - 1) % choices.length;
        this.sound.play(Sound.Focus);
        break;
      case 'arrowdown':
        this.selected = (this.selected + 1) % choices.length;
        this.sound.play(Sound.Focus);
        break;
      case 'enter':
      case 'e':
      case 'arrowright':
        this.choose(card, choices[Math.min(this.selected, choices.length - 1)]);
        break;
      case 'arrowleft':
        if (this.menu !== 'main') {
          this.menu = 'main';
          this.selected = 0;
          this.sound.play(Sound.Focus);
        }
        break;
      case 'backspace':
        if (this.menu !== 'main') {
          this.menu = 'main';
          this.selected = 0;
        } else {
          this.close();
        }
        break;
      case 'f':
        if (!card.done && !card.error) {
          this.finish();
        }
        break;
      default:
        return;
    }
    event.preventDefault();
    event.stopPropagation();
    this.changes.markForCheck();
  }

  private reset(): void {
    this.menu = 'main';
    this.selected = 0;
    this.changes.markForCheck();
  }
}
