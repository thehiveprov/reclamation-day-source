import { Injectable, NgZone } from '@angular/core';
import { TranslocoService } from '@ngneat/transloco';
import { BehaviorSubject, firstValueFrom } from 'rxjs';
import { Sound, SoundService } from './sound.service';

export interface ErrorEvent {
  error:
    | 'wrong_version'
    | 'mods_mismatch'
    | 'client_mods_disallowed'
    | 'wrong_password'
    | 'server_full'
    | 'no_reason'
    | 'bad_uGridsToLoad'
    | 'non_default_install'
    | 'set_time_public_server'
    | 'use_main_menu';
  data?: Record<any, any>;
}

export interface WrongVersionErrorEvent extends ErrorEvent {
  error: 'wrong_version';
  data: { version: string; expectedVersion: string };
}

/** In a mods mismatch's list: a plugin the player has as another version than the host's (Mods::Entry::kDifferentVersion). */
const MOD_DIFFERENT_VERSION = '65535';

/** Plugins for the error popup: one per line, in a box that scrolls when the list is long (styled in ErrorComponent). */
function modList(mods: string[], numbered = false): string {
  const escape = (text: string) => text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  const tag = numbered ? 'ol' : 'ul';
  return `<${tag} class="mod-list">${mods.map(mod => `<li>${escape(mod)}</li>`).join('')}</${tag}>`;
}

export interface ModsMismatchErrorEvent extends ErrorEvent {
  error: 'mods_mismatch';
  data: { mods: [filename: string, id: string][]; hostLoadOrder?: string[] };
}

export interface ClientModsDisallowedErrorEvent extends ErrorEvent {
  error: 'client_mods_disallowed';
  data: { mods: ('SKSE' | 'MO2')[] };
}

export type ErrorEvents =
  | WrongVersionErrorEvent
  | ModsMismatchErrorEvent
  | ClientModsDisallowedErrorEvent
  | ErrorEvent;

@Injectable({
  providedIn: 'root',
})
export class ErrorService {
  private error = new BehaviorSubject<string>('');
  public error$ = this.error.asObservable();

  /** Refused for our plugins: the host's load order, which the game can switch ours to. */
  private hostLoadOrder = new BehaviorSubject<string[] | null>(null);
  public hostLoadOrder$ = this.hostLoadOrder.asObservable();

  /** Refused only for the order of our plugins (we have all of the host's and no others): a re-sort fixes it. */
  private loadOrderOnly = new BehaviorSubject<boolean>(false);
  public loadOrderOnly$ = this.loadOrderOnly.asObservable();

  constructor(
    private readonly sound: SoundService,
    private readonly translocoService: TranslocoService,
    private readonly zone: NgZone,
  ) {
    skyrimtogether.on('loadOrderResult', this.onLoadOrderResult.bind(this));
  }

  /** Switches our plugins to the host's load order: the game restarts, or answers why it can't. */
  async matchHostLoadOrder() {
    const order = this.hostLoadOrder.getValue();
    if (!order) {
      return;
    }
    this.hostLoadOrder.next(null);
    this.loadOrderOnly.next(false);
    this.error.next(
      await firstValueFrom(this.translocoService.selectTranslate('SERVICE.ERROR.LOAD_ORDER.MATCHING')),
    );
    skyrimtogether.matchLoadOrder(order.join('\n'));
  }

  private onLoadOrderResult(json: string) {
    this.zone.run(async () => {
      const result = JSON.parse(json) as { ok: boolean; reason: string; plugins: string[] };
      const message = await firstValueFrom(
        this.translocoService.selectTranslate('SERVICE.ERROR.LOAD_ORDER.' + result.reason.toUpperCase(), {
          plugins: `<strong>${result.plugins.join(', ')}</strong>`,
        }),
      );
      await this.setError(message);
    });
  }

  getError() {
    return this.error.getValue();
  }

  async setError(error: ErrorEvents | string) {
    this.sound.play(Sound.Fail);
    this.hostLoadOrder.next(
      typeof error !== 'string' && error.error === 'mods_mismatch' && error.data?.hostLoadOrder?.length
        ? error.data.hostLoadOrder
        : null,
    );
    this.loadOrderOnly.next(false);
    let message: string;
    if (typeof error === 'string') {
      message = error;
    } else {
      let data: Record<string, any> = error.data;
      switch (error.error) {
        case 'mods_mismatch':
          let mods = '';
          const install = error.data.mods
            .filter(mod => mod[1] === '0')
            .map(mod => mod[0]);
          if (install.length > 0) {
            mods += '\n\n<hr>\n';
            mods += await firstValueFrom(
              this.translocoService.selectTranslate(
                'SERVICE.ERROR.ERRORS.MODS_MISMATCH_INSTALL',
                { mods: modList(install) },
              ),
            );
          }
          const remove = error.data.mods
            .filter(mod => mod[1] !== '0' && mod[1] !== MOD_DIFFERENT_VERSION)
            .map(mod => mod[0]);
          if (remove.length > 0) {
            mods += '\n\n<hr>\n';
            mods += await firstValueFrom(
              this.translocoService.selectTranslate(
                'SERVICE.ERROR.ERRORS.MODS_MISMATCH_REMOVE',
                { mods: modList(remove) },
              ),
            );
          }
          const otherVersion = error.data.mods
            .filter(mod => mod[1] === MOD_DIFFERENT_VERSION)
            .map(mod => mod[0]);
          if (otherVersion.length > 0) {
            mods += '\n\n<hr>\n';
            mods += await firstValueFrom(
              this.translocoService.selectTranslate(
                'SERVICE.ERROR.ERRORS.MODS_MISMATCH_VERSION',
                { mods: modList(otherVersion) },
              ),
            );
            // Only other versions: the load order isn't what's wrong, matching the host's wouldn't help
            if (install.length === 0 && remove.length === 0) {
              this.hostLoadOrder.next(null);
              data = { mods };
              break;
            }
          }
          const hostLoadOrder = error.data.hostLoadOrder ?? [];
          // All of the host's mods and no others, only in another order: a prompt to re-sort ours
          if (hostLoadOrder.length > 0 && install.length === 0 && remove.length === 0) {
            this.loadOrderOnly.next(true);
            this.error.next(
              await firstValueFrom(
                this.translocoService.selectTranslate('SERVICE.ERROR.ERRORS.MODS_ORDER_ONLY', {
                  order: modList(hostLoadOrder, true),
                }),
              ),
            );
            return;
          }
          if (hostLoadOrder.length > 0) {
            if (install.length === 0 && remove.length === 0) {
              mods += '\n\n<hr>\n';
              mods += await firstValueFrom(
                this.translocoService.selectTranslate(
                  'SERVICE.ERROR.ERRORS.MODS_MISMATCH_ORDER',
                ),
              );
            }
            mods += '\n\n<hr>\n';
            mods += await firstValueFrom(
              this.translocoService.selectTranslate(
                'SERVICE.ERROR.ERRORS.MODS_MISMATCH_HOST_ORDER',
                { order: modList(hostLoadOrder, true) },
              ),
            );
          }
          data = { mods };
          break;
        case 'client_mods_disallowed':
          data = { mods: error.data.mods.join(', ') };
          break;
      }
      message = await firstValueFrom(
        this.translocoService.selectTranslate(
          'SERVICE.ERROR.ERRORS.' + error.error.toUpperCase(),
          data,
        ),
      );
    }
    this.error.next(message);
  }

  removeError() {
    this.sound.play(Sound.Ok);
    this.hostLoadOrder.next(null);
    this.loadOrderOnly.next(false);
    this.error.next('');
  }
}
