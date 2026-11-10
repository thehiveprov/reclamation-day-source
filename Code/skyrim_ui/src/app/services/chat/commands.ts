import { ChatService } from '../chat.service';
import { EMOTE_COMMANDS } from '../emotes.service';

export interface Command {
  readonly name: string;
  readonly executor: (args: string[]) => Promise<void>;
}

export class CommandHandler {
  private Help: Command = {
    name: 'help',
    executor: async () => {
      const cmds = [...this.commands.keys()].join(', ');
      this.chatService.pushSystemMessage(
        'SERVICE.COMMANDS.AVAILABLE_COMMANDS',
        { cmds },
      );
    },
  }
  
  private SetTime: Command = {
    name: 'settime', 
    executor: async (args) => {
      const cmds = [...this.commands.keys()].join(', ');
      if (args.length != 2) {
        this.chatService.pushSystemMessage(
          'COMPONENT.CHAT.SET_TIME_ARGUMENT_COUNT', 
          { cmds },
        );
        return;
      }
      const hours = parseInt(args[0]);
      const minutes = parseInt(args[1]);
      if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59 || Number.isNaN(hours) || Number.isNaN(minutes)) {
        this.chatService.pushSystemMessage(
          'COMPONENT.CHAT.SET_TIME_INVALID_ARGUMENTS',
          { cmds },
        );
        return;
      }
      skyrimtogether.setTime(hours, minutes);
      // TODO (Toe Knee): Ideally send a localizable response string here,
      // currently relies on user making it themselves with serverside scripting
    },
  }

  private readonly commands = new Map<string, Command>();

  public constructor(private readonly chatService: ChatService) {
    this.register(this.Help);
    this.register(this.SetTime);
    // Fallout 4 emotes: /salute, /sit..., and /emotes for the list; the game plays them (EmoteService)
    for (const name of [...EMOTE_COMMANDS, 'emotes']) {
      this.register({
        name,
        executor: async () => {
          if (typeof skyrimtogether.emotes === 'function') {
            skyrimtogether.emotes('command', name);
          }
        },
      });
    }
  }

  public readonly COMMAND_PREFIX = '/';

  public register(cmd: Command) {
    if (!this.commands.has(cmd.name)) {
      this.commands.set(cmd.name, cmd);
    }
  }

  public async tryExecute(input: string) {
    const inputWithoutPrefix = input.slice(this.COMMAND_PREFIX.length);
    const [commandName, ...args] = inputWithoutPrefix.split(' ');
    const command = this.commands.get(commandName);
    if (command) {
      command.executor(args);
    } else {
      this.chatService.pushSystemMessage('SERVICE.COMMANDS.COMMAND_NOT_FOUND', {
        cmd: commandName,
      });
    }
  }
}
