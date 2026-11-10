/**
 * MCM's keycodes: a keyboard key is its Windows virtual-key code (what Chromium gives as KeyboardEvent.keyCode), a
 * mouse button 256 + the game's button number (2 the middle one, 3 and 4 the side ones), a gamepad button 266 and up.
 * Modifiers: Shift 1, Ctrl 2, Alt 4.
 */
const NAMES: Record<number, string> = {
  8: 'Backspace',
  9: 'Tab',
  13: 'Enter',
  19: 'Pause',
  20: 'Caps Lock',
  32: 'Space',
  33: 'Page Up',
  34: 'Page Down',
  35: 'End',
  36: 'Home',
  37: 'Left',
  38: 'Up',
  39: 'Right',
  40: 'Down',
  44: 'Print Screen',
  45: 'Insert',
  46: 'Delete',
  106: 'Num *',
  107: 'Num +',
  109: 'Num -',
  110: 'Num .',
  111: 'Num /',
  144: 'Num Lock',
  145: 'Scroll Lock',
  186: ';',
  187: '=',
  188: ',',
  189: '-',
  190: '.',
  191: '/',
  192: '`',
  219: '[',
  220: '\\',
  221: ']',
  222: "'",
};

const MOUSE_OFFSET = 256;
const GAMEPAD_OFFSET = 266;

/** Shift, Ctrl and Alt: modifiers, never a hotkey's key themselves. */
export function isModifierKey(keyCode: number): boolean {
  return keyCode === 16 || keyCode === 17 || keyCode === 18 || (keyCode >= 160 && keyCode <= 165);
}

/** A mouse button as MCM numbers it (only the middle and side ones can be keys), 0 for the others. */
export function mouseKeycode(button: number): number {
  switch (button) {
    case 1:
      return MOUSE_OFFSET + 2;
    case 3:
      return MOUSE_OFFSET + 3;
    case 4:
      return MOUSE_OFFSET + 4;
    default:
      return 0;
  }
}

export function keyName(keycode: number, modifiers = 0): string {
  if (!keycode) {
    return '';
  }
  let name: string;
  if (keycode >= GAMEPAD_OFFSET) {
    name = `Pad ${keycode - GAMEPAD_OFFSET + 1}`;
  } else if (keycode >= MOUSE_OFFSET) {
    name = `Mouse ${keycode - MOUSE_OFFSET + 1}`;
  } else if ((keycode >= 48 && keycode <= 57) || (keycode >= 65 && keycode <= 90)) {
    name = String.fromCharCode(keycode);
  } else if (keycode >= 96 && keycode <= 105) {
    name = `Num ${keycode - 96}`;
  } else if (keycode >= 112 && keycode <= 135) {
    name = `F${keycode - 111}`;
  } else {
    name = NAMES[keycode] ?? `Key ${keycode}`;
  }
  const held = [modifiers & 2 ? 'Ctrl' : '', modifiers & 1 ? 'Shift' : '', modifiers & 4 ? 'Alt' : ''].filter(part => part);
  return [...held, name].join(' + ');
}
