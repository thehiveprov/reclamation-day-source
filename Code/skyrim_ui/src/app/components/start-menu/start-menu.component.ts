import { ChangeDetectionStrategy, Component, HostListener } from '@angular/core';
import { Observable } from 'rxjs';
import { ModConfigItem, ModConfigService, ModConfigState } from '../../services/mod-config.service';
import { SocialService } from '../../services/social.service';
import { StartMenuEntry, StartMenuListName, StartMenuOption, StartMenuService, StartMenuState } from '../../services/start-menu.service';
import { isModifierKey, keyName, mouseKeycode } from './key-names';

/** MCM's "MOD CONFIG" in the pause menu's list: the number MCM.swf gives the entry it adds to MainMenu.swf's list. */
const MOD_CONFIG_INDEX = 100;

/**
 * Fallout 4: the pause menu and the main menu's screens (settings, controls, help, saves) in the overlay's look, over
 * the game's own menu, which keeps running them (StartMenuBridge in the game). Each screen is the game's: its rows, its
 * selection, its "are you sure?"; a click is the game's own press, Escape its Cancel (back; at the pause menu's list,
 * resume).
 */
@Component({
  selector: 'app-start-menu',
  templateUrl: './start-menu.component.html',
  styleUrls: ['./start-menu.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class StartMenuComponent {
  readonly state$: Observable<StartMenuState | null> = this.startMenu.state$;
  /** MCM's pages, drawn in place of the pause menu while they're open. */
  readonly modConfig$: Observable<ModConfigState | null> = this.modConfig.state$;
  /** The hotkey waiting for its new key (its entry's number), null for none. */
  capturing: number | null = null;
  /** The help of the MCM entry under the mouse. */
  help = '';

  constructor(
    private readonly startMenu: StartMenuService,
    private readonly modConfig: ModConfigService,
    private readonly social: SocialService,
  ) {}

  /** The social sidebar over the pause menu: Steam friends to invite or join. */
  openSocial(): void {
    this.social.open();
  }

  @HostListener('window:keydown', ['$event'])
  onKey(event: KeyboardEvent): void {
    const modConfig = this.modConfig.state$.value;
    if (modConfig) {
      this.onModConfigKey(event, modConfig);
      return;
    }
    const state = this.startMenu.state$.value;
    if (!state) {
      return;
    }
    if (event.key === 'Escape') {
      event.preventDefault();
      this.back();
    } else if (event.key === 'Enter' && state.confirm !== undefined) {
      event.preventDefault();
      this.accept();
    } else if (event.key.toUpperCase() === 'T' && state.buttons?.some(button => button.id === 'desktop')) {
      event.preventDefault();
      this.desktop();
    } else if (event.key.toUpperCase() === 'S' && state.settingsChanged && !state.savingSettings && !state.remapping) {
      event.preventDefault();
      this.save();
    }
  }

  isConfirm(state: StartMenuState): boolean {
    return state.state.endsWith('_CONFIRM_STATE');
  }

  /** The screen's title over its panel; none for the pause menu's list (the list is all there is). */
  title(state: StartMenuState): string {
    switch (state.state) {
      case 'SETTINGS_CATEGORY_STATE':
        return 'SETTINGS';
      case 'OPTIONS_LISTS_STATE':
      case 'DEFAULT_SETTINGS_CONFIRM_STATE': {
        const category = state.categories?.entries[state.categories.selected];
        return category ? `SETTINGS · ${category.text.toUpperCase()}` : 'SETTINGS';
      }
      case 'INPUT_MAPPING_STATE':
        return 'CONTROLS';
      case 'HELP_STATE':
        return 'HELP';
      case 'SAVE_LOAD_STATE':
      case 'SAVE_LOAD_CONFIRM_STATE':
      case 'DELETE_SAVE_CONFIRM_STATE':
        return state.saving ? 'SAVE' : 'LOAD';
      case 'CHARACTER_SELECT_STATE':
        return 'CHARACTERS';
      default:
        return '';
    }
  }

  press(list: StartMenuListName, row: number, disabled = false): void {
    if (!disabled) {
      this.startMenu.press(list, row);
    }
  }

  /** Hovering a row selects it in the game (the guide text, the save's details follow). */
  hover(list: StartMenuListName, row: number, selected: number): void {
    if (row !== selected) {
      this.startMenu.select(list, row);
    }
  }

  /** A confirmation's yes (MAIN MENU when quitting), through the game's own confirm button. */
  accept(): void {
    this.startMenu.button('accept');
  }

  /** Quitting from the pause menu: to the desktop. */
  desktop(): void {
    this.startMenu.button('desktop');
  }

  /** A confirmation's answer by its button. */
  answer(id: string): void {
    if (id === 'accept') {
      this.accept();
    } else if (id === 'desktop') {
      this.desktop();
    } else {
      this.back();
    }
  }

  /** The key each of the game's answers has, as its own button bar shows it. */
  answerKey(id: string): string {
    return id === 'accept' ? 'ENTER' : id === 'desktop' ? 'T' : 'ESC';
  }

  back(): void {
    this.startMenu.event('Cancel');
  }

  defaults(): void {
    this.startMenu.event('YButton');
  }

  save(): void {
    this.startMenu.save();
  }

  deleteSave(): void {
    this.startMenu.event('XButton');
  }

  step(row: number, option: StartMenuOption, by: number): void {
    const last = Math.max(0, option.options.length - 1);
    const value = Math.max(0, Math.min(last, Math.round(option.value) + by));
    if (value !== option.value) {
      this.startMenu.setOption(row, value);
    }
  }

  toggle(row: number, option: StartMenuOption): void {
    this.startMenu.setOption(row, option.value ? 0 : 1);
  }

  slide(row: number, event: Event): void {
    this.startMenu.setOption(row, Number((event.target as HTMLInputElement).value));
  }

  sliderShare(option: StartMenuOption): number {
    const span = option.max - option.min;
    return span > 0 ? ((option.value - option.min) / span) * 100 : 0;
  }

  trackRow(index: number): number {
    return index;
  }

  // The pause menu's list: MCM's entry opens its pages here, the others are the game's

  entryText(entry: StartMenuEntry): string {
    return entry.index === MOD_CONFIG_INDEX ? 'MOD CONFIG' : entry.text;
  }

  pressMain(entry: StartMenuEntry, row: number): void {
    if (entry.index === MOD_CONFIG_INDEX) {
      this.help = '';
      this.modConfig.open();
    } else {
      this.press('main', row, entry.disabled);
    }
  }

  // MCM's pages

  /** A key for the hotkey waiting for one (Escape keeps the old key); otherwise Escape goes back to the pause menu. */
  private onModConfigKey(event: KeyboardEvent, state: ModConfigState): void {
    if (this.capturing !== null) {
      event.preventDefault();
      if (event.key === 'Escape') {
        this.capturing = null;
      } else if (!isModifierKey(event.keyCode)) {
        this.giveKey(state, event.keyCode, event);
      }
      return;
    }
    if (event.key === 'Escape') {
      event.preventDefault();
      if (state.conflict) {
        this.modConfig.cancelKey();
      } else {
        this.closeModConfig();
      }
    } else if (event.key === 'Enter' && state.conflict) {
      event.preventDefault();
      this.takeKey(state);
    }
  }

  /** The middle and side mouse buttons can be a hotkey's key too; the others leave it as it was. */
  @HostListener('window:mousedown', ['$event'])
  onMouseDown(event: MouseEvent): void {
    const state = this.modConfig.state$.value;
    if (this.capturing === null || !state) {
      return;
    }
    const keycode = mouseKeycode(event.button);
    if (keycode) {
      event.preventDefault();
      this.giveKey(state, keycode, event);
    } else {
      this.capturing = null;
    }
  }

  private giveKey(state: ModConfigState, keycode: number, event: KeyboardEvent | MouseEvent): void {
    const item = state.items.find(entry => entry.i === this.capturing);
    if (item) {
      const modifiers = item.allowModifiers ? (event.shiftKey ? 1 : 0) | (event.ctrlKey ? 2 : 0) | (event.altKey ? 4 : 0) : 0;
      this.modConfig.key(item.i, keycode, modifiers);
    }
    this.capturing = null;
  }

  closeModConfig(): void {
    this.capturing = null;
    this.help = '';
    this.modConfig.close();
  }

  openPage(mod: number, page: number): void {
    this.capturing = null;
    this.help = '';
    this.modConfig.page(mod, page);
  }

  /** The mod's name and the page's, over the page. */
  pageTitle(state: ModConfigState): string {
    const mod = state.mods[state.mod];
    if (!mod) {
      return 'MOD CONFIG';
    }
    const page = mod.pages[state.page];
    return page ? `${mod.name} · ${page}` : mod.name;
  }

  showHelp(item: ModConfigItem): void {
    this.help = item.help;
  }

  toggleItem(item: ModConfigItem): void {
    this.modConfig.set(item.i, item.value ? 0 : 1);
  }

  stepItem(item: ModConfigItem, by: number): void {
    const last = Math.max(0, item.options.length - 1);
    const value = Math.max(0, Math.min(last, Math.round(item.value) + by));
    if (value !== item.value) {
      this.modConfig.set(item.i, value);
    }
  }

  slideItem(item: ModConfigItem, event: Event): void {
    this.modConfig.set(item.i, Number((event.target as HTMLInputElement).value));
  }

  itemShare(item: ModConfigItem): number {
    const span = item.max - item.min;
    return span > 0 ? Math.max(0, Math.min(100, ((item.value - item.min) / span) * 100)) : 0;
  }

  /** A slider's value with as many decimals as its step has. */
  sliderText(item: ModConfigItem): string {
    const decimals = item.step > 0 && item.step < 1 ? Math.min(4, Math.ceil(-Math.log10(item.step) - 1e-9)) : 0;
    return item.value.toFixed(decimals);
  }

  /** A text input's value: a text for textinput, a number for textinputInt and textinputFloat. */
  enterText(item: ModConfigItem, event: Event): void {
    const text = (event.target as HTMLInputElement).value;
    if (item.type === 'textinput') {
      this.modConfig.text(item.i, text);
    } else if (text.trim() !== '' && !isNaN(Number(text))) {
      this.modConfig.set(item.i, item.type === 'textinputInt' ? Math.trunc(Number(text)) : Number(text));
    }
  }

  pressItem(item: ModConfigItem): void {
    this.modConfig.press(item.i);
  }

  captureKey(item: ModConfigItem): void {
    this.capturing = item.i;
  }

  clearKey(item: ModConfigItem): void {
    this.capturing = null;
    this.modConfig.key(item.i, 0, 0);
  }

  keyText(keycode: number, modifiers: number): string {
    return keyName(keycode, modifiers);
  }

  takeKey(state: ModConfigState): void {
    const conflict = state.conflict;
    if (conflict) {
      this.modConfig.key(conflict.item, conflict.keycode, conflict.modifiers, true);
    }
  }

  keepKey(): void {
    this.modConfig.cancelKey();
  }

  trackItem(_: number, item: ModConfigItem): number {
    return item.i;
  }
}
