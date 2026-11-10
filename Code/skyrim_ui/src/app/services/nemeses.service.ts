import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { BehaviorSubject, Subscription } from 'rxjs';
import { ClientService } from './client.service';

/** How a man stands with the player, as the screens colour him. */
export type NemesisTone = 'murderous' | 'vengeful' | 'resentful' | 'none';

/** The words for a man or a woman, as the strings take them ({{he}}, {{He}}, {{him}}, {{his}}, {{His}}). */
export function pronouns(female?: boolean): Record<string, string> {
  return female
    ? { he: 'she', He: 'She', him: 'her', his: 'her', His: 'Her' }
    : { he: 'he', He: 'He', him: 'him', his: 'his', His: 'His' };
}

/** The bar over a surrendered man held at gunpoint, and the top of his questioning card. */
export interface NemesisBar {
  id: number;
  actor: number;
  /** "Unknown" until the player knows him. */
  name: string;
  /** "the Gunners", "rides alone". */
  gang: string;
  rank: number;
  fear: number;
  fearWord: string;
  hate: number;
  tone: NemesisTone;
  /** His temper once known, kin, at gunpoint, how often he was just hit... */
  chips: { text: string; tone: 'normal' | 'aim' | 'blood' | 'gold' | 'good' }[];
}

/** A surrendered man's questioning card. */
export interface NemesisQuestionCard {
  id: number;
  name: string;
  /** "Captain of the Gunners", "Rides alone". */
  standing: string;
  temper: string;
  fear: number;
  fearWord: string;
  hate: number;
  /** What he just said. */
  line: string;
  revealed: { kind: string; text: string; location: string }[];
  /** He's gone (let go, finished): only closing is left. */
  done: boolean;
  /** A woman: the card's words follow. */
  female?: boolean;
  bar?: NemesisBar | null;
  error?: string;
}

/** RequestNemesis::Question */
export enum NemesisQuestion {
  AskBoss = 0,
  AskCamp,
  AskStash,
  AskKin,
  AskHabits,
  AskWhy,
  Threaten,
  Pay,
  LetHimGo,
  FinishHim,
  Close,
}

/** A man as a box on the hierarchy or a line in a list. */
export interface NemesisMan {
  id: number;
  /** His name is known to the player: else only his seat is shown. */
  known: boolean;
  name: string;
  nickname: string;
  /** "Captain", the tier's title, "" outside the hierarchy. */
  seat: string;
  gang: string;
  rank: number;
  status: 'alive' | 'dead' | 'gone';
  tone: NemesisTone;
  /** "Resentful", "Vengeful", "Murderous" towards the player, "" for no grudge. */
  standing: string;
  hate: number;
  temper: string;
  limp: boolean;
  scarred: boolean;
  liar: boolean;
  informant: boolean;
  /** New, or came for the player in the last day. */
  fresh: boolean;
}

export interface NemesisGangTab {
  id: string;
  name: string;
  destroyed: boolean;
  notoriety: number;
}

export interface NemesisTier {
  label: string;
  single: string;
  men: NemesisMan[];
  vacant: number;
}

export interface NemesisGang {
  id: string;
  name: string;
  short: string;
  leaderTitle: string;
  destroyed: boolean;
  loners: boolean;
  notoriety: { value: number; level: number; word: string };
  aim: string;
  strength: { word: string; men: number; ground: number; manpower: number; caps: number; supplies: number };
  wars: { against: string; over: string }[];
  leader: NemesisMan | null;
  leaderNote: string;
  tiers: NemesisTier[];
  men?: NemesisMan[];
  unnamed: number;
  fallen: NemesisMan[];
  fallenCount: number;
}

export interface NemesisManPage {
  id: number;
  known: boolean;
  name: string;
  nickname: string;
  seat: string;
  gang: string;
  rank: number;
  kind: string;
  age: number;
  trade: string;
  status: 'alive' | 'dead' | 'gone';
  statusWord: string;
  fate: string;
  tone: NemesisTone;
  temper?: string;
  temperText?: string;
  wits?: string;
  home?: string;
  faith?: string;
  grudge: {
    hate: number;
    hateWord: string;
    fear: number;
    fearWord: string;
    intent: string;
    origin: string;
    since: string;
    downedYou: number;
    mercy: boolean;
    informant: boolean;
    owes: boolean;
  } | null;
  scars: { text: string; place: string }[];
  injuries: string;
  ties: { kind: string; text: string; id: number }[];
  history: { at: string; tone: number; text: string }[];
  wounds: number;
  female?: boolean;
}

export interface NemesisWound {
  /** Nemesis::Part: head, torso, left arm, right arm, left leg, right leg, unknown. */
  part: number;
  where: string;
  cause: string;
  weapon: string;
  ammo: string;
  /** Metres. */
  range: number;
  damage: number;
  hits: number;
  severity: 'light' | 'serious' | 'severe' | 'crippling' | 'lethal';
  crippled: boolean;
  lethal: boolean;
  at: string;
  place: string;
}

export interface NemesisVictim {
  id: number;
  name: string;
  gang: string;
  nemesis: boolean;
  outcome: 'survived' | 'killed' | 'dead' | 'left for dead' | 'gone';
  wounds: NemesisWound[];
  female?: boolean;
}

export interface NemesisWar {
  a: string;
  b: string;
  over: string;
  since: string;
  battles: number;
  deadA: number;
  deadB: number;
  heat: number;
  ended: boolean;
  result: string;
}

/** A page of the nemesis screen, as the server writes it (Nemesis::Engine::ScreenJson). */
export interface NemesisScreenPage {
  page: 'record' | 'gang' | 'man' | 'world' | 'wounds';
  date: string;
  player: string;
  gangs: NemesisGangTab[];
  enabled: boolean;
  // record
  karma?: { value: number; alignment: number; word: string };
  tallies?: { label: string; value: number }[];
  notoriety?: { gang: string; name: string; value: number; level: number; word: string }[];
  nemeses?: NemesisMan[];
  story?: { at: string; tag: string; text: string }[];
  storyMore?: number;
  dead?: { id: number; name: string }[];
  deadMore?: number;
  party?: { player: string; nemeses: { id: number; name: string; standing: string }[] }[];
  // gang
  gang?: NemesisGang | null;
  // man
  man?: NemesisManPage | null;
  // world
  wars?: NemesisWar[];
  relations?: { a: string; b: string; value: number; word: string; why: string }[];
  chronicle?: { at: string; tag: string; gang: string; title: string; text: string; place: string }[];
  funerals?: { who: string; gang: string; place: string; at: string }[];
  // wounds
  victims?: NemesisVictim[];
  men?: number;
  total?: number;
  hits?: number;
  selected?: number;
}

/**
 * Nemeses (Fallout 4): the nemesis screen (the Pip-Boy's OPEN FULL SCREEN, or its key), questioning a gang man who
 * surrendered at gunpoint, and the bar over him while the player's weapon is on him. The game passes requests on
 * (nemeses(...)) and the server's answers back (the nemesisScreen, nemesisCard and nemesisGunpoint events). Escape
 * closes the overlay without the key reaching the page: what's open closes with it.
 */
@Injectable({
  providedIn: 'root',
})
export class NemesesService implements OnDestroy {
  readonly card$ = new BehaviorSubject<NemesisQuestionCard | null>(null);
  readonly screen$ = new BehaviorSubject<NemesisScreenPage | null>(null);
  /** A man's page over the screen (a box clicked), null for none. */
  readonly man$ = new BehaviorSubject<NemesisManPage | null>(null);
  readonly gunpoint$ = new BehaviorSubject<NemesisBar | null>(null);

  private readonly activation: Subscription;

  constructor(
    private readonly zone: NgZone,
    private readonly client: ClientService,
  ) {
    skyrimtogether.on('nemesisCard', this.onCard.bind(this));
    skyrimtogether.on('nemesisScreen', this.onScreen.bind(this));
    skyrimtogether.on('nemesisGunpoint', this.onGunpoint.bind(this));
    this.activation = this.client.activationStateChange.subscribe(active => {
      if (active) {
        return;
      }
      if (this.screen$.value) {
        this.closeScreen();
      }
      if (this.card$.value) {
        this.close();
      }
    });
  }

  ngOnDestroy(): void {
    skyrimtogether.off('nemesisCard');
    skyrimtogether.off('nemesisScreen');
    skyrimtogether.off('nemesisGunpoint');
    this.activation.unsubscribe();
  }

  /** A question, a threat, caps, letting him go or finishing him. */
  ask(question: NemesisQuestion, caps = 0): void {
    this.request('question', caps > 0 ? `${question}:${caps}` : String(question));
    if (question === NemesisQuestion.FinishHim) {
      this.card$.next(null);
    }
  }

  /** The card goes, the game's cursor with it. */
  close(): void {
    this.card$.next(null);
    this.request('close');
  }

  /** A page of the screen: "record", "gang:<id>", "man:<id>", "world", "wounds". */
  show(page: string): void {
    this.request('screen', page);
  }

  closeMan(): void {
    this.man$.next(null);
  }

  closeScreen(): void {
    this.screen$.next(null);
    this.man$.next(null);
    this.request('closeScreen');
  }

  private request(request: string, payload = ''): void {
    // The game's older builds don't have it
    if (typeof skyrimtogether.nemeses === 'function') {
      skyrimtogether.nemeses(request, payload);
    }
  }

  private parse<T>(json: string): T | null {
    try {
      return JSON.parse(json) as T;
    } catch {
      return null;
    }
  }

  private onCard(json: string): void {
    this.zone.run(() => {
      this.gunpoint$.next(null);
      this.card$.next(this.parse<NemesisQuestionCard>(json));
    });
  }

  private onScreen(json: string): void {
    this.zone.run(() => {
      const page = this.parse<NemesisScreenPage>(json);
      if (!page) {
        this.screen$.next(null);
        this.man$.next(null);
      } else if (page.page === 'man') {
        // A man's page opens over the screen
        this.man$.next(page.man ?? null);
      } else {
        this.screen$.next(page);
      }
    });
  }

  private onGunpoint(json: string): void {
    this.zone.run(() => this.gunpoint$.next(this.card$.value ? null : this.parse<NemesisBar>(json)));
  }
}
