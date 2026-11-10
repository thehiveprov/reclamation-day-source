import { createStore } from '@ngneat/elf';
import {
  addEntities,
  deleteEntities,
  getAllEntities,
  getEntity,
  selectAllEntities,
  updateEntities,
  withEntities,
} from '@ngneat/elf-entities';
import { EventEmitter } from 'events';
import { fromEvent } from 'rxjs';
import { MessageTypes } from '../services/chat.service';
import { ErrorEvents } from '../services/error.service';
import { MockPlayer } from './mock-player';
import { nemesisSamples } from './nemesis-samples';

let nextPlayerId = 1;

const playerStore = createStore(
  { name: 'players' },
  withEntities<MockPlayer>(),
);

export class SkyrimtogetherMock extends EventEmitter implements SkyrimTogether {
  private connected = false;
  private active = false;
  private version = 'browser';
  private playerName = 'Local Player';
  private showEvents = true;
  private localPlayerId: number;
  public readonly players$ = playerStore.pipe(selectAllEntities());

  connect(host: string, port: number, password: string, username?: string): void {
    if (!this.connected) {
      let error: ErrorEvents | boolean;
      switch (host) {
        case 't-port':
        case 't-host':
          error = true;
          break;
        case 't-password':
          if (password !== 'test') {
            error = { error: 'wrong_password' };
          }
          break;
        case 't-version':
          error = {
            error: 'wrong_version',
            data: {
              version: '[current-version]',
              expectedVersion: '[expected-version]',
            },
          };
          break;
        case 't-full':
          error = { error: 'server_full' };
          break;
        case 't-order':
          error = {
            error: 'mods_mismatch',
            data: {
              mods: [],
              hostLoadOrder: ['Fallout4.esm', 'DLCRobot.esm', 'Unofficial Fallout 4 Patch.esp', 'StartMeUp.esp', 'ReclamationDay.esp'],
            },
          };
          break;
        case 't-version':
          error = {
            error: 'mods_mismatch',
            data: {
              mods: [['Misery Island.esp', '65535']],
              hostLoadOrder: ['Fallout4.esm', 'DLCRobot.esm', 'Unofficial Fallout 4 Patch.esp', 'Misery Island.esp'],
            },
          };
          break;
        case 't-client-mods':
          error = { error: 'client_mods_disallowed', data: { mods: ['SKSE'] } };
          break;
        case 't-mods':
          error = {
            error: 'mods_mismatch',
            data: {
              mods: [
                ['missing.esp', '0'],
                ['remove.esp', '12'],
                ['missing_2.esp', '0'],
                ['remove_2.esp', '12'],
              ],
            },
          };
          break;
      }
      setTimeout(() => {
        this.emit(!!error ? 'disconnect' : 'connect');
        this.connected = !error;
        if (error && typeof error !== 'boolean') {
          this.emit('triggerError', JSON.stringify(error));
        } else {
          this.emit('setLocalPlayerId', (this.localPlayerId = nextPlayerId++));
        }
        for (const player of playerStore.query(getAllEntities())) {
          this.emit(
            'playerConnected',
            player.id,
            player.name,
            player.level,
            player.cellName,
          );
          this.emit('setPlayer3dLoaded', player.id, player.health);
        }
        if (!error) {
          this.emitQuests();
          this.emitSettlements();
          setTimeout(() => this.mockQuestInvite(), 3000);
        }
      }, 250);
    }
  }

  disconnect(): void {
    if (this.connected) {
      this.emit('disconnect');
      this.connected = false;
    }
  }

  reconnect(): void {
    throw new Error('NOT YET IMPLEMENTED');
  }

  sendMessage(type: MessageTypes, message: string): void {
    if (this.connected) {
      this.emit('message', type, message, this.playerName);
    }
  }

  deactivate(): void {
    throw new Error('NOT YET IMPLEMENTED');
  }

  setTime(hours: number, minutes: number): void {
    console.log('setTime', hours, minutes);
  }

  mainMenuAction(action: string, payload: string): void {
    console.log('mainMenuAction', action, payload);
    if (action === 'close') {
      this.emit('mainMenuClose');
    } else if (action.startsWith('startMenu.')) {
      this.mockStartMenuAction(action.slice('startMenu.'.length), payload);
    } else if (action.startsWith('modConfig.')) {
      this.mockModConfigAction(action.slice('modConfig.'.length), payload);
    }
  }

  // MCM's pages as ModConfigBridge sends them: a few mods like the installed ones, their values kept here
  private modConfigOpen = false;
  private modConfigMod = 0;
  private modConfigPage = 0;
  private modConfigConflict: any = null;
  private mcmItem(type: string, text: string, extra: any = {}): any {
    return {
      type,
      text,
      help: '',
      centered: false,
      big: false,
      value: 0,
      valueString: '',
      options: [],
      min: 0,
      max: 1,
      step: 1,
      keycode: 0,
      modifiers: 0,
      allowModifiers: true,
      ...extra,
    };
  }
  private readonly modConfigMods: { name: string; error: string; pages: { name: string; items: any[] }[] }[] = [
    {
      name: 'Reclamation Day',
      error: '',
      pages: [
        {
          name: 'HUD',
          items: [
            this.mcmItem('text', "The compass, health and radiation, AP, the enemy's bar, ammo and grenades in Reclamation Day's own look."),
            this.mcmItem('switcher', 'Reclamation Day HUD', { value: 1, help: "Off: the game's own HUD pieces come back as they were." }),
          ],
        },
        {
          name: 'Actor Bars',
          items: [
            this.mcmItem('section', 'Bars'),
            this.mcmItem('switcher', 'Show bars over actors', { value: 1, groupControl: 1, help: 'Health bars over the people and creatures near you.' }),
            this.mcmItem('slider', 'Distance', { value: 2500, min: 500, max: 6000, step: 100, groupCondition: 1, help: 'How far away a bar still shows.' }),
            this.mcmItem('slider', 'Opacity', { value: 0.85, min: 0, max: 1, step: 0.05, groupCondition: 1 }),
            this.mcmItem('stepper', 'Show for', { value: 1, options: ['Enemies', 'Everyone hostile or hurt', 'Everyone'], groupCondition: 1 }),
          ],
        },
      ],
    },
    {
      name: 'Fallout 2287: Nuclear Winter',
      error: '',
      pages: [
        {
          name: '',
          items: [
            this.mcmItem('text', 'Fallout 2287: Nuclear Winter\nv1.0.0', { centered: true, big: true }),
            this.mcmItem('text', 'by D1v1ne122', { centered: true }),
          ],
        },
        {
          name: 'General Settings',
          items: [
            this.mcmItem('text', 'General metric options, system toggles and mod activation.'),
            this.mcmItem('section', 'General Settings'),
            this.mcmItem('switcher', 'Mod Activation (def: ON)', { value: 1, help: 'Toggle to turn the mod On or Off' }),
            this.mcmItem('dropdown', 'Insulation Settings', { value: 0, options: ['Realistic', 'Universal'], help: 'Changes the temperature metric displayed on the HUD.' }),
            this.mcmItem('spacer', ''),
            this.mcmItem('hotkey', 'Insulation Toggle Hotkey', { keycode: 73, help: 'Display the Insulation Widget with a rundown of your equipped armor.' }),
            this.mcmItem('button', '[ Reset Helper Messages ]', { help: 'Resets all helper messages so they will re-display.' }),
            this.mcmItem('textinputFloat', 'Max Insulation', { value: 120.5 }),
          ],
        },
      ],
    },
    { name: 'KARMA', error: 'It needs KARMA.esm.', pages: [] },
    {
      name: "VATS 76'",
      error: '',
      pages: [
        {
          name: '',
          items: [
            this.mcmItem('text', 'VATS without leaving first or third person.'),
            this.mcmItem('section', 'Keys'),
            this.mcmItem('hotkey', 'Realtime VATS on/off', { keycode: 86, help: 'Locks onto the enemy closest to your crosshair. Press again to leave.' }),
            this.mcmItem('hotkey', 'Next target', { keycode: 0 }),
          ],
        },
        { name: 'Targets', items: [this.mcmItem('slider', 'Lock range', { value: 4000, min: 1000, max: 8000, step: 250 })] },
      ],
    },
  ];

  /** The debug helper's MCM button: the pause menu with MCM's pages open, or closed. */
  mockModConfig(open: boolean): void {
    if (open) {
      this.mockStartMenu('pause');
    }
    this.modConfigOpen = open;
    this.emitModConfig();
  }

  private emitModConfig(): void {
    if (!this.modConfigOpen) {
      this.emit('modConfig', 'null');
      return;
    }
    const mod = this.modConfigMods[this.modConfigMod];
    const page = mod?.pages[this.modConfigPage];
    // the groups: bit 0 always, each switcher with a groupControl turns its bit on
    let flags = 1;
    page?.items.forEach(item => {
      if (item.groupControl !== undefined) {
        flags = item.value ? flags | (1 << item.groupControl) : flags & ~(1 << item.groupControl);
      }
    });
    const items = (mod?.error ? [] : page?.items ?? [])
      .map((item, i) => ({ ...item, i }))
      .filter(item => item.groupCondition === undefined || (flags & (1 << item.groupCondition)) !== 0)
      .map(({ groupControl, groupCondition, ...item }) => item);
    const state = {
      mods: this.modConfigMods.map(entry => ({ name: entry.name, pages: entry.pages.map(p => p.name), error: entry.error })),
      mod: this.modConfigMod,
      page: this.modConfigPage,
      items,
      ...(this.modConfigConflict ? { conflict: this.modConfigConflict } : {}),
    };
    this.emit('modConfig', JSON.stringify(state));
  }

  private mockModConfigAction(action: string, payload: string): void {
    const [first, second, third] = payload.split(':');
    const item = this.modConfigMods[this.modConfigMod]?.pages[this.modConfigPage]?.items[Number(first)];
    if (action === 'open' || action === 'close') {
      this.modConfigOpen = action === 'open';
      this.modConfigConflict = null;
    } else if (action === 'page') {
      this.modConfigMod = Number(first);
      this.modConfigPage = Number(second) < (this.modConfigMods[this.modConfigMod]?.pages.length ?? 0) ? Number(second) : 0;
    } else if (action === 'set' && item) {
      item.value = Number(second);
    } else if (action === 'text' && item) {
      item.valueString = payload.slice(first.length + 1);
    } else if ((action === 'key' || action === 'forceKey') && item) {
      // X is Enhanced Movement's dodge here
      if (action === 'key' && Number(second) === 88) {
        this.modConfigConflict = { item: Number(first), keycode: 88, modifiers: Number(third), with: 'Dodge (EnhancedMovement)' };
      } else {
        this.modConfigConflict = null;
        item.keycode = Number(second);
        item.modifiers = Number(third);
      }
    } else if (action === 'cancelKey') {
      this.modConfigConflict = null;
    }
    this.emitModConfig();
  }

  // The pause menu as the game's own movie would run it: a few screens, the rows it shows on them
  private startMenuState: any = null;
  private readonly startMenuOptions = [
    { text: 'Difficulty', type: 1, value: 2, id: 0, options: ['Very Easy', 'Easy', 'Normal', 'Hard', 'Very Hard', 'Survival'], min: 0, max: 1, step: 0.05 },
    { text: 'Invert Y', type: 2, value: 0, id: 1, options: [], min: 0, max: 1, step: 0.05 },
    { text: 'Look Sensitivity', type: 0, value: 0.45, id: 2, options: [], min: 0, max: 1, step: 0.05 },
    { text: 'Vibration', type: 2, value: 1, id: 3, options: [], min: 0, max: 1, step: 0.05 },
    { text: 'Save on Rest', type: 2, value: 1, id: 5, options: [], min: 0, max: 1, step: 0.05 },
    { text: 'Autosave Every', type: 1, value: 2, id: 9, options: ['5 Mins', '10 Mins', '15 Mins', '30 Mins', '45 Mins', '60 Mins', 'Disabled'], min: 0, max: 1, step: 0.05 },
    { text: 'Companion Enabled', type: 2, value: 1, id: 10, options: [], min: 0, max: 1, step: 0.05 },
  ];
  private readonly startMenuScreens: Record<string, () => any> = {
    pause: () => ({
      menu: 'pause',
      state: 'MAIN_STATE',
      main: {
        entries: [
          { text: 'QUICKSAVE', index: 2 },
          { text: 'SAVE', index: 3 },
          { text: 'LOAD', index: 4 },
          { text: 'HELP', index: 8 },
          { text: 'MOD_CONFIG', index: 100 },
          { text: 'SETTINGS', index: 9 },
          { text: 'QUIT', index: 10 },
        ],
        selected: 0,
      },
    }),
    categories: () => ({
      menu: 'pause',
      state: 'SETTINGS_CATEGORY_STATE',
      categories: { entries: [{ text: 'Gameplay' }, { text: 'Display' }, { text: 'Interface' }, { text: 'Audio' }, { text: 'Controls' }], selected: 0 },
    }),
    options: () => ({
      ...this.startMenuScreens['categories'](),
      state: 'OPTIONS_LISTS_STATE',
      options: { entries: this.startMenuOptions, selected: this.startMenuState?.options?.selected ?? 0 },
      guide: 'How hard the Commonwealth fights back. Survival changes much more than the damage.',
    }),
    controls: () => ({
      ...this.startMenuScreens['categories'](),
      state: 'INPUT_MAPPING_STATE',
      categories: { entries: [{ text: 'Gameplay' }, { text: 'Display' }, { text: 'Interface' }, { text: 'Audio' }, { text: 'Controls' }], selected: 4 },
      controls: {
        entries: [
          { text: 'Forward', key: 'W', button: 'LS' },
          { text: 'Back', key: 'S', button: 'LS' },
          { text: 'Activate', key: 'E', button: 'A' },
          { text: 'Jump', key: 'Space', button: 'X' },
          { text: 'Sprint', key: 'Alt', button: 'L3' },
          { text: 'Pip-Boy', key: 'Tab', button: 'B' },
          { text: 'VATS', key: 'Q', button: 'RB' },
        ],
        selected: 2,
      },
      remapping: false,
    }),
    help: () => ({
      menu: 'pause',
      state: 'HELP_STATE',
      help: { entries: [{ text: 'Survival Mode' }, { text: 'Settlements' }, { text: 'V.A.T.S.' }, { text: 'Power Armor' }], selected: 2 },
      helpTitle: 'V.A.T.S.',
      helpText: 'The Vault-Tec Assisted Targeting System slows time and lets you target specific body parts.\n\nEach shot costs Action Points.',
    }),
    saves: () => ({
      menu: 'pause',
      state: 'SAVE_LOAD_STATE',
      saves: {
        entries: [
          { text: 'Sanctuary Hills', playTime: '12.04.33' },
          { text: 'Red Rocket Truck Stop', playTime: '11.51.07' },
          { text: 'Concord', playTime: '10.22.48' },
          { text: 'Vault 111', playTime: '0.18.02' },
        ],
        selected: 0,
      },
      saving: false,
      saveInfo: { player: 'Nate', time: '10/23/2287 18:41', location: 'Sanctuary Hills', level: 'Level 14' },
    }),
    quit: () => ({
      ...this.startMenuScreens['pause'](),
      state: 'QUIT_CONFIRM_STATE',
      confirm: 'Quit to main menu?  Any unsaved progress will be lost.',
      buttons: [
        { id: 'accept', text: 'MAIN MENU' },
        { id: 'desktop', text: 'DESKTOP' },
        { id: 'cancel', text: 'CANCEL' },
      ],
    }),
  };

  /** The debug helper's start menu buttons: a screen, or none. */
  mockStartMenu(screen: string): void {
    this.startMenuState = screen === 'clear' ? null : this.startMenuScreens[screen]?.() ?? null;
    this.emit('startMenu', JSON.stringify(this.startMenuState));
  }

  private mockStartMenuAction(action: string, payload: string): void {
    const state = this.startMenuState;
    if (!state) {
      return;
    }
    const [first, second] = payload.split(':');
    let next: string | null = null;
    if (action === 'event' && payload === 'Cancel') {
      next = { MAIN_STATE: 'clear', SETTINGS_CATEGORY_STATE: 'pause', OPTIONS_LISTS_STATE: 'categories', INPUT_MAPPING_STATE: 'categories', HELP_STATE: 'pause', SAVE_LOAD_STATE: 'pause', QUIT_CONFIRM_STATE: 'pause' }[state.state as string] ?? 'pause';
    } else if (action === 'press' && first === 'main') {
      next = ({ 3: 'saves', 4: 'saves', 8: 'help', 9: 'categories', 10: 'quit' } as Record<number, string>)[state.main.entries[Number(second)].index] ?? null;
    } else if (action === 'press' && first === 'category') {
      next = Number(second) === 4 ? 'controls' : 'options';
    } else if (action === 'select' && first === 'option') {
      state.options.selected = Number(second);
      this.emit('startMenu', JSON.stringify(state));
    } else if (action === 'set') {
      this.startMenuOptions[Number(first)].value = Number(second);
      next = 'options';
    } else if (action === 'button' && state.state === 'QUIT_CONFIRM_STATE') {
      next = 'clear';
    }
    if (next) {
      this.mockStartMenu(next);
    }
  }

  // A plugin the host has isn't installed here
  matchLoadOrder(order: string): void {
    console.log('matchLoadOrder', order);
    setTimeout(
      () => this.emit('loadOrderResult', JSON.stringify({ ok: false, reason: 'missing', plugins: ['SomeHostMod.esp'] })),
      500,
    );
  }

  playSound(sound: string): void {
    console.log('playSound', sound);
  }

  // Two servers on the network: one we can join, one on another version
  lanScan(): void {
    setTimeout(
      () =>
        this.emit(
          'lanServers',
          JSON.stringify([
            {
              address: '192.168.1.14:10578',
              compatible: true,
              name: 'Commonwealth Crew',
              port: 10578,
              players: 2,
              maxPlayers: 8,
              version: 'v1.6.7-121-ga620cde7',
              password: false,
            },
            {
              address: '192.168.1.26:10578',
              compatible: false,
              name: 'Sanctuary Hills',
              port: 10578,
              players: 1,
              maxPlayers: 8,
              version: 'v1.6.7-119-g70c9c2af',
              password: true,
            },
          ]),
        ),
      800,
    );
  }

  // Build tools as the client keeps them (BuildToolsService), with its rules for keys in short
  private buildToolsState = {
    placeAnywhere: true,
    scrapAnything: true,
    keys: [
      ['placeAnywhere', 0x70], ['scrapAnything', 0x73], ['rotationAxis', 0x2d], ['rotateLeft', 0xbc], ['rotateRight', 0xbe],
      ['rotateStepDown', 0x6d], ['rotateStepUp', 0x6b], ['level', 0x60], ['moveUp', 0x68], ['moveDown', 0x62],
      ['moveLeft', 0x64], ['moveRight', 0x66], ['moveCloser', 0x67], ['moveAway', 0x69], ['moveStepDown', 0x6f],
      ['moveStepUp', 0x6a], ['worldAxes', 0x24], ['scaleDown', 0x61], ['scaleUp', 0x63], ['scaleStepDown', 0x22],
      ['scaleStepUp', 0x21], ['scaleReset', 0x6e], ['redraw', 0x65], ['undo', 0x08], ['remember', 0xbb],
      ['pastePositionX', 0xdb], ['pastePositionY', 0xdd], ['pastePositionZ', 0xdc], ['pasteAngles', 0xba],
      ['pasteScale', 0xde], ['pasteAll', 0xbf],
    ].map(([action, key]) => ({ action: action as string, key: key as number, defaultKey: key as number })),
  };

  buildTools(request: string, payload: string): void {
    const [name, value] = payload.split(':');
    let error = '';
    if (request === 'bind') {
      const key = Number(value);
      const reserved =
        (key >= 0x30 && key <= 0x39) || (key >= 0x41 && key <= 0x5a) || [0x09, 0x0d, 0x10, 0x11, 0x12, 0x14, 0x1b, 0x20, 0x25, 0x26, 0x27, 0x28, 0x71, 0x72, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7b, 0xc0].includes(key);
      const used = this.buildToolsState.keys.find(entry => entry.key === key && entry.action !== name);
      if (reserved) {
        error = 'reserved';
      } else if (used) {
        error = `used:${used.action}`;
      } else {
        this.buildToolsState.keys.find(entry => entry.action === name)!.key = key;
      }
    } else if (request === 'set') {
      this.buildToolsState[name as 'placeAnywhere' | 'scrapAnything'] = value === '1';
    } else if (request === 'reset') {
      this.buildToolsState.keys.forEach(entry => (entry.key = entry.defaultKey));
    }
    setTimeout(() => this.emit('buildToolsState', JSON.stringify({ ...this.buildToolsState, error })), 0);
  }

  // Quest team-ups as the client keeps them (QuestService). Invited players accept after a second; the first other
  // player invites us to a quest a few seconds after connecting.
  private questList: { id: string; name: string; active: boolean; team: { participants: number[]; helpers: number[] } | null; role: string }[] = [
    { id: '000229E8', name: 'Unlikely Valentine', active: true, team: null, role: '' },
    { id: '0001ED89', name: 'The Molecular Level', active: false, team: null, role: '' },
    { id: '00059C13', name: 'Pickman Gallery', active: false, team: null, role: '' },
  ];
  private questInvites: { questId: string; questName: string; inviterId: number }[] = [];
  private mainQuestSync = false;

  // Settlement claims as the client keeps them (WorkshopService): one we lead, one where we can build, one whose
  // leader is away, our C.A.M.P. and another player's. The mock is the host, so it can release (or remove) anyone's.
  private settlementList: {
    id: string;
    name: string;
    leader: string;
    leaderOnline: boolean;
    camp?: boolean;
    campStored?: boolean;
    mine: boolean;
    canBuild: boolean;
    canScrap: boolean;
    rights: { name: string; online: boolean; inParty: boolean; build: boolean; scrap: boolean }[];
  }[] = [
    {
      id: '0:1F0C2',
      name: 'Sanctuary Hills',
      leader: 'Me',
      leaderOnline: true,
      mine: true,
      canBuild: true,
      canScrap: true,
      rights: [
        { name: 'Alex', online: true, inParty: true, build: true, scrap: false },
        { name: 'Robin', online: false, inParty: false, build: true, scrap: true },
      ],
    },
    { id: '0:2A3C5', name: 'Red Rocket Truck Stop', leader: 'Alex', leaderOnline: true, mine: false, canBuild: true, canScrap: false, rights: [] },
    { id: '0:6BD45', name: 'Starlight Drive In', leader: 'Robin', leaderOnline: false, mine: false, canBuild: false, canScrap: false, rights: [] },
    {
      id: 'FFFFFFFE:1',
      name: "Me's C.A.M.P.",
      leader: 'Me',
      leaderOnline: true,
      camp: true,
      mine: true,
      canBuild: true,
      canScrap: true,
      rights: [{ name: 'Alex', online: true, inParty: true, build: false, scrap: false }],
    },
    { id: 'FFFFFFFE:2', name: "Alex's C.A.M.P.", leader: 'Alex', leaderOnline: true, camp: true, mine: false, canBuild: false, canScrap: false, rights: [] },
  ];

  companions(request: string, payload: string): void {
    console.log('companions', request, payload);
  }

  /** Nemeses: the server's own sample pages (nemesis-samples.ts), as the game would pass them on. */
  nemeses(request: string, payload: string): void {
    const send = (event: 'nemesisScreen' | 'nemesisCard', page: unknown) => setTimeout(() => this.emit(event, JSON.stringify(page)), 50);
    if (request === 'screen') {
      const [page, arg] = payload.split(':');
      if (page === 'gang') {
        send('nemesisScreen', nemesisSamples[`gang_${arg ?? 'gunners'}`] ?? nemesisSamples['gang_gunners']);
      } else if (page === 'man') {
        const man = nemesisSamples[`man_${arg}`] ?? Object.entries(nemesisSamples).find(([key]) => key.startsWith('man_'))?.[1];
        send('nemesisScreen', man);
      } else if (page === 'wounds') {
        send('nemesisScreen', { ...nemesisSamples['wounds'], selected: Number(arg) || 0 });
      } else {
        send('nemesisScreen', nemesisSamples[page] ?? nemesisSamples['record']);
      }
    } else if (request === 'question') {
      const lines = ['Nobody. I answer to nobody.', 'Out past the old interchange. Go see for yourself.', 'Stash? What stash?', 'My brother. Leave him out of it.',
        'They drink at the Third Rail most nights.', 'I got caps. Hundred and twenty. Take them and let me walk.', 'Okay! Okay!', 'Caps talk. Ask.', "You won't see me again."];
      const question = Number(payload.split(':')[0]);
      const card = nemesisSamples['card'];
      send('nemesisCard', {
        ...card,
        line: lines[question] ?? '',
        done: question === 8,
        revealed: question === 5 ? [{ kind: 'offer', text: '120 caps for his life', location: '' }] : card.revealed,
      });
    }
  }

  /** The debug helper's nemesis buttons. */
  mockNemesis(what: 'screen' | 'gunpoint' | 'card' | 'clear'): void {
    if (what === 'screen') {
      this.nemeses('screen', 'record');
    } else if (what === 'gunpoint') {
      this.emit('nemesisGunpoint', JSON.stringify(nemesisSamples['gunpoint']));
    } else if (what === 'card') {
      this.emit('nemesisCard', JSON.stringify(nemesisSamples['card']));
    } else {
      this.emit('nemesisGunpoint', 'null');
      this.emit('nemesisScreen', 'null');
    }
  }

  /** The debug helper's C.A.M.P. buttons: the card as the device opens it (here, or where it can't move), none. */
  mockCamp(what: 'card' | 'blocked' | 'clear'): void {
    const card = what === 'clear' ? null : { cost: 12, caps: 340, refusal: what === 'blocked' ? 'Too close to a marked location.' : '' };
    this.emit('campCard', JSON.stringify(card));
  }

  social(request: string, payload: string): void {
    console.log('social', request, payload);
  }

  emotes(request: string, payload: string): void {
    console.log('emotes', request, payload);
    if (request === 'play' || request === 'close') {
      this.emit('emoteWheel', 'null');
    }
  }

  /** The debug helper's emote button: the wheel as its key opens it (the game's default eight), none. */
  mockEmoteWheel(what: 'open' | 'clear'): void {
    const names = ['point', 'yes', 'no', 'clap', 'cheer', 'salute', 'sit', 'think'];
    const labels = ['Point', 'Yes', 'No', 'Clap', 'Cheer', 'Salute', 'Sit', 'Think'];
    const wheel = what === 'clear' ? null : { key: 'G', emotes: names.map((name, i) => ({ name, label: labels[i] })) };
    this.emit('emoteWheel', JSON.stringify(wheel));
  }

  private mockVote?: any;

  dialogueVote(request: string, payload: string): void {
    console.log('dialogueVote', request, payload);
    if (request === 'cast' && this.mockVote) {
      this.mockVote.voters[1].choice = Number(payload);
      this.mockVote.stage = 'tally';
      this.mockVote.input = false;
      this.emit('dialogueVote', JSON.stringify(this.mockVote));
    }
  }

  /** The debug helper's dialogue vote buttons: a voter's card, the talker's tally, the outcome, none. */
  mockDialogueVote(what: 'voter' | 'talker' | 'result' | 'clear'): void {
    if (what === 'clear') {
      this.mockVote = undefined;
      this.emit('dialogueVote', 'null');
      return;
    }
    const talker = what === 'talker';
    this.mockVote = {
      voteId: 7,
      stage: what === 'result' ? 'result' : 'open',
      talker,
      npc: 'Piper',
      options: [
        { prompt: 'Sure', line: "Sure, I'll help you out.", challenge: 0, enabled: true },
        { prompt: 'Sarcastic', line: "Oh, I'd love nothing more than to run your errands.", challenge: 0, enabled: true },
        { prompt: 'More caps', line: "That's not enough for this kind of job.", challenge: 2, enabled: true },
        { prompt: 'Who are you?', line: '', challenge: 0, enabled: true },
      ],
      voters: [
        { name: talker ? 'Me' : 'Alex', choice: talker ? -1 : 2, talker: true, you: talker },
        { name: talker ? 'Robin' : 'Me', choice: what === 'result' ? 2 : -1, talker: false, you: !talker },
        { name: 'Sam', choice: what === 'result' ? 0 : -1, talker: false, you: false },
      ],
      msLeft: 30000,
      result: what === 'result' ? 2 : -1,
      hostBrokeTie: what === 'result',
      input: what === 'voter',
    };
    this.emit('dialogueVote', JSON.stringify(this.mockVote));
  }

  private hudSpin?: ReturnType<typeof setInterval>;

  /** The debug helper's HUD buttons: a sample HUD, the same turning on the spot, none. */
  mockHud(what: 'show' | 'spin' | 'clear'): void {
    if (this.hudSpin) {
      clearInterval(this.hudSpin);
      this.hudSpin = undefined;
    }
    if (what === 'clear') {
      this.emit('hud', 'null');
      this.emit('bike', 'null');
      return;
    }
    this.emit('bike', JSON.stringify({ kmh: 72, fuel: 0.1, on: true, lamp: true, gear: 3 }));
    // Radians, clockwise from north, as the game sent them (its probe, 10-23): the quest below the player, the Vault
    // found, Sanctuary and an encampment not yet; with a teammate, a targeted settlement, a quest door, two enemies (one
    // targeted) and the player's own marker behind
    const markers = [
      [1, 0.25, 72, 1, 0, 90, 0],
      [5, -0.62, 20, 3, 2, 15, 0],
      [5, 0.75, 71, 1, 1, 12, 0],
      [5, 1.35, 187, 1, 1, 3, 0],
      [5, -1.2, 30, 2, 0, 89, 0],
      [7, -0.95, 45, 2, 3, 13, 0],
      [2, 0.55, 12, 3, 0, 81, 0],
      [4, -0.2, 8, 2, 0, 0, 1],
      [6, 0.05, 9, 2, 0, 0, 1],
      [3, 3.0, 0, 0, 0, 85, 0],
    ];
    let heading = 0;
    const send = () =>
      this.emit(
        'hud',
        JSON.stringify({
          compass: { heading, half: Math.PI, markers },
          health: { value: 0.62, rads: 0.18 },
          radiation: true,
          ap: { value: 0.8 },
          enemy: { name: 'Raider Psycho', value: 0.45, optional: 0.3, hostile: true, legendary: true, skull: false },
          weapon: 'Suppressed 10mm Pistol',
          ammo: { first: 7, second: 112 },
          explosives: { first: 3, second: 0 },
        }),
      );
    send();
    if (what === 'spin') {
      this.hudSpin = setInterval(() => {
        heading = (heading + 0.02) % (2 * Math.PI);
        send();
      }, 16);
    }
  }

  settlements(request: string, payload: string): void {
    if (request === 'rights') {
      const [mod, base, build, scrap, ...name] = payload.split(':');
      const settlement = this.settlementList.find(entry => entry.id === `${mod}:${base}`);
      const right = settlement?.rights.find(entry => entry.name === name.join(':'));
      if (right) {
        right.build = build === '1';
        right.scrap = scrap === '1';
      }
    } else if (request === 'abandon' || request === 'release') {
      this.settlementList = this.settlementList.filter(entry => entry.id !== payload);
    } else if (request === 'pickup' || request === 'campPickUp') {
      const mine = this.settlementList.find(entry => entry.camp && entry.mine);
      if (mine) {
        mine.campStored = true;
      }
    } else if (request === 'campMove' || request === 'campCancel') {
      console.log('settlements', request);
    }
    this.emitSettlements();
  }

  private emitSettlements(): void {
    setTimeout(
      () =>
        this.emit(
          'settlementsState',
          JSON.stringify({
            connected: this.connected,
            localName: 'Me',
            isHost: true,
            currentWorkshop: '0:1F0C2',
            settlements: this.settlementList,
          }),
        ),
      0,
    );
  }

  quests(request: string, payload: string): void {
    const [questId, id] = payload.split(':');
    const quest = this.questList.find(entry => entry.id === questId);
    if (request === 'mainQuestSync') {
      this.mainQuestSync = payload === '1';
    } else if (request === 'inviteAll' && quest) {
      setTimeout(() => {
        quest.team ??= { participants: [this.localPlayerId], helpers: [] };
        for (const player of playerStore.query(getAllEntities())) {
          if (!quest.team.participants.includes(player.id)) {
            quest.team.participants.push(player.id);
          }
        }
        quest.role = 'participant';
        this.emitQuests();
      }, 1000);
    } else if (request === 'invite' && quest) {
      setTimeout(() => {
        quest.team ??= { participants: [this.localPlayerId], helpers: [] };
        quest.team.participants.push(Number(id));
        quest.role = 'participant';
        this.emitQuests();
      }, 1000);
    } else if (request === 'accept' || request === 'decline') {
      const invite = this.questInvites.find(entry => entry.questId === questId);
      this.questInvites = this.questInvites.filter(entry => entry !== invite);
      if (request === 'accept' && invite) {
        this.questList.push({
          id: invite.questId,
          name: invite.questName,
          active: false,
          team: { participants: [invite.inviterId, this.localPlayerId], helpers: [] },
          role: 'participant',
        });
      }
    } else if (request === 'leave' && quest) {
      quest.team = null;
      quest.role = '';
    }
    this.emitQuests();
  }

  private emitQuests(): void {
    setTimeout(
      () =>
        this.emit(
          'questsState',
          JSON.stringify({
            localPlayerId: this.localPlayerId ?? 0,
            connected: this.connected,
            mainQuestSync: this.mainQuestSync,
            quests: this.questList,
            invites: this.questInvites,
          }),
        ),
      0,
    );
  }

  private mockQuestInvite(): void {
    const inviter = playerStore.query(getAllEntities())[0];
    if (!this.connected || !inviter) {
      return;
    }
    const invite = { questId: '0002A9E0', questName: 'Hunter/Hunted', inviterId: inviter.id };
    this.questInvites.push(invite);
    this.emit('questTeamInvite', JSON.stringify(invite));
    this.emitQuests();

    // A quest of our own showing up in the Pip-Boy a moment later
    setTimeout(() => {
      this.questList.push({ id: '0004C0A5', name: 'Hole in the Wall', active: false, team: null, role: '' });
      this.emit('questStarted', JSON.stringify({ questId: '0004C0A5', questName: 'Hole in the Wall', party: true }));
      this.emitQuests();
    }, 1500);
  }

  teleportToPlayer(playerId: number): void {
    if (this.connected) {
      const player = playerStore.query(getEntity(playerId));
      if (player) {
        console.log(
          `%cTELEPORT`,
          'background: #F09688; color: #fff; padding: 3px; font-size: 9px;',
          'Teleport to player',
          JSON.stringify(player.name),
          `(${player.id})`,
          'in',
          JSON.stringify(player.cellName),
        );
      }
    }
  }

  launchParty(): void {
    if (this.connected) {
      this.emit('partyCreated');
    }
  }

  createPartyInvite(playerId: number): void {
    playerStore.update(updateEntities(playerId, { invited: true }));
  }

  acceptPartyInvite(inviterId: number): void {
    this.emit('partyCreated');
    this.emit(
      'partyInfo',
      [
        ...playerStore
          .query(getAllEntities())
          .filter(p => p.isInGroup)
          .map(p => p.id),
      ],
      inviterId,
    );
  }

  kickPartyMember(playerId: number): void {
    playerStore.update(updateEntities(playerId, { isInGroup: false }));
    this.emit(
      'partyInfo',
      playerStore
        .query(getAllEntities())
        .filter(p => p.isInGroup)
        .map(p => p.id),
      this.localPlayerId,
    );
  }

  leaveParty(): void {
    if (this.connected) {
      this.emit('partyInfo', [], -1);
      this.emit('partyLeft');
    }
  }

  changePartyLeader(playerId: number): void {
    playerStore.update(updateEntities(playerId, { hasOwnParty: true }));
    this.emit(
      'partyInfo',
      [
        ...playerStore
          .query(getAllEntities())
          .filter(p => p.isInGroup)
          .map(p => p.id),
      ],
      playerId,
    );
  }

  initMock() {
    Object.keys((this as any)._events).forEach(e => {
      this.on(e, (...params) => {
        if (this.showEvents) {
          const eventName = e
            .replace(/(\G(?!^)|\b[a-zA-Z][a-z]*)([A-Z][a-z]*|\d+)/gm, `$1_$2`)
            .toUpperCase();
          console.log(
            `%cEVENT`,
            'background: #009688; color: #fff; padding: 3px; font-size: 9px;',
            `[${eventName}]`,
            ...params.map(v => JSON.stringify(v)),
          );
        }
      });
    });
    this.emit('init');
    this.emit('enterGame');
    this.emit('setVersion', this.version);
    this.emit('setName', this.playerName);

    // Previews, for screenshots without key presses: ?overlay opens the in-game overlay right away (as F2 does);
    // ?connected joins a mock server with two others in your party and some chat; ?dead starts the respawn timer;
    // ?menu opens the main menu. ?view=<settings, server_list, ...> opens that screen (MockClientService).
    const preview = new URLSearchParams(window.location.search);
    if (preview.has('overlay')) {
      setTimeout(() => {
        this.active = true;
        this.emit('activate');
      }, 500);
    }
    if (preview.has('connected')) {
      const others = [this.addMockPlayer(), this.addMockPlayer()];
      this.connect('localhost', 10578, '');
      setTimeout(() => {
        this.launchParty();
        for (const other of others) {
          this.accteptMockPlayerInvite(other.id);
        }
        this.emit('message', MessageTypes.GLOBAL_CHAT, 'Anyone near Diamond City?', others[0].name);
        this.emit('message', MessageTypes.PARTY_CHAT, 'Meet at Red Rocket', others[1].name);
        this.emit('message', MessageTypes.LOCAL_CHAT, 'Behind you!', others[0].name);
        this.emit('message', MessageTypes.PLAYER_DIALOGUE, 'Welcome to the Commonwealth.', others[1].name);
      }, 600);
    }
    // ?connect=<host>: joins that mock server (t-mods, t-order, t-version, ... show the server's refusals)
    if (preview.has('connect')) {
      setTimeout(() => this.connect(preview.get('connect') ?? 'localhost', 10578, ''), 600);
    }
    if (preview.has('dead')) {
      setTimeout(() => this.emit('respawnCountdown', 30), 600);
    }
    // ?menu: the main menu's title screen, with one missing requirement
    if (preview.has('menu')) {
      setTimeout(() => {
        this.emit(
          'mainMenuOpen',
          JSON.stringify({
            status: '',
            dataLoaded: true,
            creationsLoaded: true,
            startMeUp: true,
            continueSave: null,
            singlePlayerCharacters: [],
            nativeEntries: [],
            selectedWorldId: '',
            recentServers: [],
            worlds: [{ id: 'w1', name: 'Commonwealth Crew', hosted: true, address: '', characters: 1 }],
            characters: [],
            missingRequirements: [
              { name: 'Garden of Eden Papyrus Script Extender', url: 'https://www.nexusmods.com/fallout4/mods/74160' },
              { name: 'Start Me Up Redux', url: 'https://www.nexusmods.com/fallout4/mods/56984' },
            ],
            update: { version: 'v1.6.7-130-g0123abc', current: 'v1.6.7-121-ga620cde7', state: 'available', message: '' },
          }),
        );
      }, 600);
    }
    // Headless screenshots fast-forward time, which can leave Angular's animations stuck at their start: finish them
    if (['overlay', 'connected', 'dead', 'menu'].some(name => preview.has(name))) {
      setInterval(() => {
        for (const animation of document.getAnimations()) {
          if (animation.effect?.getTiming().iterations !== Infinity) {
            animation.finish();
          }
        }
      }, 500);
    }

    fromEvent(window, 'keydown').subscribe((event: KeyboardEvent) => {
      if (event.ctrlKey && event.location === 2) {
        this.active = !this.active;
        this.emit(this.active ? 'activate' : 'deactivate');
        event.preventDefault();
        return;
      }
      switch (event.key) {
        case 'F2': {
          this.active = !this.active;
          this.emit(this.active ? 'activate' : 'deactivate');
          event.preventDefault();
          break;
        }
      }
    });
  }

  setMockVersion(version: string): void {
    this.version = version;
  }

  setMockPlayerName(name: string): void {
    this.playerName = name;
  }

  setShowEvents(show: boolean): void {
    this.showEvents = show;
  }

  addMockPlayer() {
    const newPlayerId = nextPlayerId++;
    const cities = [
      'Sanctuary Hills',
      'Diamond City',
      'Goodneighbor',
      'Concord',
      'Red Rocket Truck Stop',
      'The Castle',
      'Bunker Hill',
      'Vault 81',
    ];
    const newPlayer: MockPlayer = {
      name: 'Player ' + newPlayerId,
      id: newPlayerId,
      level: Math.floor(Math.random() * 100),
      health: Math.floor(Math.random() * 100),
      cellName: cities[Math.floor(Math.random() * cities.length)],
      hasOwnParty: false,
      isInGroup: false,
      invited: false,
      invitedLocalPlayer: false,
    };
    playerStore.update(addEntities(newPlayer));
    if (this.connected) {
      this.emit(
        'playerConnected',
        newPlayer.id,
        newPlayer.name,
        newPlayer.level,
        newPlayer.cellName,
      );
      this.emit('setPlayer3dLoaded', newPlayer.id, newPlayer.health);
    }
    // this.emit('setHealth', newPlayer.id, newPlayer.health);
    return newPlayer;
  }

  disconnectMockPlayer(playerId: number) {
    const mockPlayer = playerStore.query(getEntity(playerId));
    if (mockPlayer) {
      if (this.connected) {
        this.emit('playerDisconnected', mockPlayer.id, mockPlayer.name);
      }
      playerStore.update(deleteEntities(mockPlayer.id));
    }
  }

  accteptMockPlayerInvite(playerId: number) {
    playerStore.update(
      updateEntities(playerId, { invited: false, isInGroup: true }),
    );
    this.emit(
      'partyInfo',
      playerStore
        .query(getAllEntities())
        .filter(p => p.isInGroup)
        .map(p => p.id),
      this.localPlayerId,
    );
  }

  inviteToPlayerMockParty(playerId: number) {
    playerStore.update(updateEntities(playerId, { invitedLocalPlayer: true }));
    this.emit('partyInviteReceived', playerId);
  }

  startPlayerMockParty(playerId: number) {
    playerStore.update(
      updateEntities(playerId, { hasOwnParty: true, isInGroup: true }),
    );
  }

  mockPlayerLeaveParty(playerId: number) {
    const player = playerStore.query(getEntity(playerId));

    playerStore.update(
      updateEntities(playerId, {
        hasOwnParty: false,
        invitedLocalPlayer: false,
        isInGroup: false,
      }),
    );
    if (this.connected) {
      if (player.isInGroup) {
        this.emit(
          'partyInfo',
          playerStore
            .query(getAllEntities())
            .filter(p => p.isInGroup)
            .map(p => p.id),
          this.localPlayerId,
        );
      } else if (player.hasOwnParty) {
        this.emit('partyInfo', [], -1);
      }
    }
  }

  updateMockDebugData() {
    const debugData = [
      Math.floor(Math.random() * 100),
      Math.floor(Math.random() * 100),
      Math.floor(Math.random() * 100),
      Math.floor((Math.random() / 4) * 100),
      Math.random() * 100,
      Math.random() * 100,
    ];
    this.emit('debugData', ...debugData);
    return debugData;
  }
}

export function mockSkyrimTogether() {
  (globalThis as any).skyrimtogether = new SkyrimtogetherMock();
}
