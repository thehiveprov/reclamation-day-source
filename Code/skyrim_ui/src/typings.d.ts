declare const module: NodeModule;

interface NodeModule {
  id: string;
}

/** Skyim: Together type definitions */
declare namespace SkyrimTogetherTypes {
  /** Client initialization callback */
  type InitCallback = () => void;

  /** UI activation callback */
  type ActivateCallback = () => void;

  /** UI deactivation callback */
  type DeactivateCallback = () => void;

  /** Player game entry callback */
  type EnterGameCallback = () => void;

  /** Player game exit callback */
  type ExitGameCallback = () => void;

  /** Player open/close game menu callback */
  type OpeningMenuCallback = (openingMenu: boolean) => void;

  /** Chat message reception callback */
  type MessageCallback = (
    type: number,
    content: string,
    sender: string,
  ) => void;

  /** Connection callback */
  type ConnectCallback = () => void;

  /** Disconnection callback */
  type DisconnectCallback = (isError: boolean) => void;

  /** Name change callback */
  type SetNameCallback = (name: string) => void;

  /** Version set callback */
  type SetVersionCallback = (version: string) => void;

  /**  */
  type OnDebugCallback = (isDebug: boolean) => void;

  type UpdateDebugCallback = (
    numPacketsSent: number,
    numPacketsReceived: number,
    RTT: number,
    packetLoss: number,
    sentBandwidth: number,
    receivedBandwidth: number,
  ) => void;

  type UserDataSetCallback = (password: string, username: string) => void;

  type PlayerConnectedCallback = (
    playerId: number,
    username: string,
    level: number,
    cellName: string,
  ) => void;

  type PlayerDisconnectedCallback = (
    playerId: number,
    username: string,
  ) => void;

  type SetHealthCallback = (playerId: number, health: number) => void;

  type SetLevelCallback = (playerId: number, level: number) => void;

  type SetCellCallback = (playerId: number, cellName: string) => void;

  type SetPlayer3dLoadedCallback = (playerId: number, health: number) => void;

  type SetPlayer3dUnloadedCallback = (playerId: number) => void;

  type SetLocalPlayerIdCallback = (playerId: number) => void;

  type ProtocolMismatch = () => void;

  type TriggerError = () => void;

  type DummyDataCallback = (data: Array<number>) => void;

  type PartyInfoCallback = (playerIds: Array<number>, leaderId: number) => void;

  type PartyCreatedCallback = () => void;

  type PartyLeftCallback = (inviterId: number) => void;

  type PartyInviteReceivedCallback = (inviterId: number) => void;
}

/** Global Skyrim: Together object. */
declare const skyrimtogether: SkyrimTogether;

/** Global Skyrim: Together object type. */
interface SkyrimTogether {
  /** Add listener to when the UI is first initialized. */
  on(event: 'init', callback: SkyrimTogetherTypes.InitCallback): void;

  /** Add listener to when the UI is activated. */
  on(event: 'activate', callback: SkyrimTogetherTypes.ActivateCallback): void;

  /** Add listener to when the UI is deactivated. */
  on(
    event: 'deactivate',
    callback: SkyrimTogetherTypes.DeactivateCallback,
  ): void;

  /** Add listener to when the player enters a game. */
  on(event: 'enterGame', callback: SkyrimTogetherTypes.EnterGameCallback): void;

  /** Add listener to when the player exits a game. */
  on(event: 'exitGame', callback: SkyrimTogetherTypes.ExitGameCallback): void;

  /** Add listener to when the player open/close a game menu. */
  on(
    event: 'openingMenu',
    callback: SkyrimTogetherTypes.OpeningMenuCallback,
  ): void;

  /** Add listener to when a player message is received. */
  on(event: 'message', callback: SkyrimTogetherTypes.MessageCallback): void;

  /** Add listener to when the player connects to a server. */
  on(event: 'connect', callback: SkyrimTogetherTypes.ConnectCallback): void;

  /** Add listener to when the player disconnects from a server. */
  on(
    event: 'disconnect',
    callback: SkyrimTogetherTypes.DisconnectCallback,
  ): void;

  /** Add listener to when the player's name changes. */
  on(event: 'setName', callback: SkyrimTogetherTypes.SetNameCallback): void;

  /** Add listener to when the client's version is set. */
  on(
    event: 'setVersion',
    callback: SkyrimTogetherTypes.SetVersionCallback,
  ): void;

  /** Add listener to when the player's press the F3 key */
  on(event: 'debug', callback: SkyrimTogetherTypes.OnDebugCallback): void;

  on(
    event: 'debugData',
    callback: SkyrimTogetherTypes.UpdateDebugCallback,
  ): void;

  /** Add listener to when the player's is connected with the launcher */
  on(
    event: 'userDataSet',
    callback: SkyrimTogetherTypes.UserDataSetCallback,
  ): void;

  /** Add listener to when one player connect in server. */
  on(
    event: 'playerConnected',
    callback: SkyrimTogetherTypes.PlayerConnectedCallback,
  ): void;

  /** Add listener to when one player disconnect in server. */
  on(
    event: 'playerDisconnected',
    callback: SkyrimTogetherTypes.PlayerDisconnectedCallback,
  ): void;

  on(event: 'setHealth', callback: SkyrimTogetherTypes.SetHealthCallback): void;

  /** Add listener to when one player change level in server. */
  on(event: 'setLevel', callback: SkyrimTogetherTypes.SetLevelCallback): void;

  /** Add listener to when one player change cell in server. */
  on(event: 'setCell', callback: SkyrimTogetherTypes.SetCellCallback): void;

  /** Add listener to when a player is loaded or unloaded in 3D.  */
  on(
    event: 'setPlayer3dLoaded',
    callback: SkyrimTogetherTypes.SetPlayer3dLoadedCallback,
  ): void;

  on(
    event: 'setPlayer3dUnloaded',
    callback: SkyrimTogetherTypes.SetPlayer3dUnloadedCallback,
  ): void;

  on(
    event: 'setLocalPlayerId',
    callback: SkyrimTogetherTypes.SetLocalPlayerIdCallback,
  ): void;

  on(
    event: 'protocolMismatch',
    callback: SkyrimTogetherTypes.ProtocolMismatch,
  ): void;

  on(event: 'triggerError', callback: SkyrimTogetherTypes.TriggerError): void;

  on(event: 'dummyData', callback: SkyrimTogetherTypes.DummyDataCallback): void;

  on(event: 'partyInfo', callback: SkyrimTogetherTypes.PartyInfoCallback): void;

  on(
    event: 'partyCreated',
    callback: SkyrimTogetherTypes.PartyCreatedCallback,
  ): void;

  on(event: 'partyLeft', callback: SkyrimTogetherTypes.PartyLeftCallback): void;

  on(
    event: 'partyInviteReceived',
    callback: SkyrimTogetherTypes.PartyInviteReceivedCallback,
  ): void;

  /** Add listener to when the local player starts (seconds > 0) or stops (0) waiting to respawn. */
  on(event: 'respawnCountdown', callback: (seconds: number) => void): void;

  /** Fallout 4 main menu multiplayer screens: opened with their data (JSON). */
  on(event: 'mainMenuOpen', callback: (json: string) => void): void;
  /** Worlds and characters changed (JSON). */
  on(event: 'mainMenuData', callback: (json: string) => void): void;
  /** One character's saves (JSON). */
  on(event: 'mainMenuSaves', callback: (json: string) => void): void;
  /** Single-player saves a co-op character can start from (JSON). */
  on(event: 'mainMenuSourceSaves', callback: (json: string) => void): void;
  /** Progress or an error (JSON). */
  on(event: 'mainMenuStatus', callback: (json: string) => void): void;
  /** The screens closed (a save is loading, or back to the main menu). */
  on(event: 'mainMenuClose', callback: () => void): void;

  /** Fallout 4: matching the host's load order failed, nothing was changed (JSON: ok, reason, plugins). */
  on(event: 'loadOrderResult', callback: (json: string) => void): void;

  /** Fallout 4 server browsers, LAN tab: the servers that answered (JSON array, see LanServer). */
  on(event: 'lanServers', callback: (json: string) => void): void;

  /** Fallout 4 build tools: their switches and keys (JSON, see BuildToolsState). */
  on(event: 'buildToolsState', callback: (json: string) => void): void;

  /** Fallout 4 quest team-ups: the local player's quests, teams and invites (JSON, see QuestsState). */
  on(event: 'questsState', callback: (json: string) => void): void;
  /** Fallout 4 quest team-ups: another player invites us to do a quest together (JSON, see QuestInvite). */
  on(event: 'questTeamInvite', callback: (json: string) => void): void;
  /** Fallout 4 quest team-ups: a new quest in the Pip-Boy, offered to do together (JSON: questId, questName, party). */
  on(event: 'questStarted', callback: (json: string) => void): void;

  /** Fallout 4 settlement claims: who leads which settlement and the rights there (JSON, see SettlementsState). */
  on(event: 'settlementsState', callback: (json: string) => void): void;
  /** Fallout 4 C.A.M.P.s: the device used with one standing (JSON: cost, caps, refusal), "null" when the game closed it. */
  on(event: 'campCard', callback: (json: string) => void): void;

  /** Fallout 4 adopted dogs: name the one just adopted, or Dogmeat (JSON: id, the dog's record in hex, and name). */
  on(event: 'companionName', callback: (json: string) => void): void;
  /** Fallout 4 nemeses: a surrendered man's questioning card, opened by talking to him (JSON, see NemesisQuestionCard). */
  on(event: 'nemesisCard', callback: (json: string) => void): void;
  /** Fallout 4 nemeses: a page of the nemesis screen (JSON, see NemesisScreenPage), "null" when the game closed it. */
  on(event: 'nemesisScreen', callback: (json: string) => void): void;
  /** Fallout 4 nemeses: the bar over a surrendered man held at gunpoint (JSON, see NemesisBar), "null" when it goes. */
  on(event: 'nemesisGunpoint', callback: (json: string) => void): void;
  /** Fallout 4: the HUD drawn by the overlay (JSON, see HudState), "null" while it's off or there's no game. */
  on(event: 'hud', callback: (json: string) => void): void;
  /** Fallout 4: the game's HUD colour (Settings, Display: HUD Color), "[r,g,b]" 0 to 255: the whole overlay's colour (ThemeService). */
  on(event: 'hudColor', callback: (json: string) => void): void;
  /** Fallout 4 dual wielding: the crosshair drawn by the overlay with two guns out (JSON, see HudCrosshair). */
  on(event: 'crosshair', callback: (json: string) => void): void;
  /** Fallout 4: the motorcycle's gauges (JSON, see HudBike), "null" off it. */
  on(event: 'bike', callback: (json: string) => void): void;
  /** Fallout 4 emotes: the wheel while its key is held (JSON, see EmoteWheel), "null" when it's let go. */
  on(event: 'emoteWheel', callback: (json: string) => void): void;
  /** Fallout 4 co-op dialogue: a vote on the talker's options (JSON, see DialogueVote), "null" when it's gone. */
  on(event: 'dialogueVote', callback: (json: string) => void): void;
  /** Fallout 4 main menu: a world the game opened (a friend's joined through Steam), its id (JSON string). */
  on(event: 'mainMenuOpenWorld', callback: (json: string) => void): void;
  /** Fallout 4 Steam friends: the social sidebar's state (JSON, see SocialState). */
  on(event: 'social', callback: (json: string) => void): void;
  /** Fallout 4 Steam friends: a friend's avatar (JSON: id, url as a data URL). */
  on(event: 'socialAvatar', callback: (json: string) => void): void;
  /** Fallout 4 Steam friends: a line to show (an invite, someone asking to join; JSON string). */
  on(event: 'socialToast', callback: (json: string) => void): void;
  /** Fallout 4 Steam friends: the sidebar opened ("true") or closed ("false") by its key in the world. */
  on(event: 'socialSidebar', callback: (open: string) => void): void;
  /** Fallout 4 Steam friends: Steam asked to join a friend while in a world (JSON, see SocialJoinAsk). */
  on(event: 'socialJoinAsk', callback: (json: string) => void): void;
  /** Fallout 4: the game's pause / main menu screen the overlay draws (JSON, see StartMenuState), "null" when it's gone. */
  on(event: 'startMenu', callback: (json: string) => void): void;
  /** Fallout 4: MCM's mods and the open page over the pause menu (JSON, see ModConfigState), "null" when it closes. */
  on(event: 'modConfig', callback: (json: string) => void): void;
  /** Fallout 4: the F2 overlay's WORLD SETTINGS, its open page (JSON, see WorldSettingsState), "null" when it closes. */
  on(event: 'worldSettings', callback: (json: string) => void): void;

  /** Remove listener from when the application is first initialized. */
  off(event: 'init', callback?: SkyrimTogetherTypes.InitCallback): void;

  /** Remove listener from when the UI is activated. */
  off(event: 'activate', callback?: SkyrimTogetherTypes.ActivateCallback): void;

  /** Remove listener from when the UI is deactivated. */
  off(
    event: 'deactivate',
    callback?: SkyrimTogetherTypes.DeactivateCallback,
  ): void;

  /** Remove listener from when the player enters a game. */
  off(
    event: 'enterGame',
    callback?: SkyrimTogetherTypes.EnterGameCallback,
  ): void;

  /** Remove listener from when the player exits a game. */
  off(event: 'exitGame', callback?: SkyrimTogetherTypes.ExitGameCallback): void;

  /** Add listener to when the player open/close a game menu. */
  off(
    event: 'openingMenu',
    callback?: SkyrimTogetherTypes.OpeningMenuCallback,
  ): void;

  /** Remove listener from when a player message is received. */
  off(event: 'message', callback?: SkyrimTogetherTypes.MessageCallback): void;

  /** Remove listener from when the player connects to a server. */
  off(event: 'connect', callback?: SkyrimTogetherTypes.ConnectCallback): void;

  /** Remove listener from when the player disconnects from a server. */
  off(
    event: 'disconnect',
    callback?: SkyrimTogetherTypes.DisconnectCallback,
  ): void;

  /** Remove listener from when the player's name changes. */
  off(event: 'setName', callback?: SkyrimTogetherTypes.SetNameCallback): void;

  /** Remove listener from when the client's version is set. */
  off(
    event: 'setVersion',
    callback?: SkyrimTogetherTypes.SetVersionCallback,
  ): void;

  /** Remove listener to when the player's press the F3 key */
  off(event: 'debug', callback?: SkyrimTogetherTypes.OnDebugCallback): void;

  off(
    event: 'debugData',
    callback?: SkyrimTogetherTypes.UpdateDebugCallback,
  ): void;

  /** Add listener to when one player connect in server. */
  off(
    event: 'playerConnected',
    callback?: SkyrimTogetherTypes.PlayerConnectedCallback,
  ): void;

  /** Add listener to when one player disconnect in server. */
  off(
    event: 'playerDisconnected',
    callback?: SkyrimTogetherTypes.PlayerDisconnectedCallback,
  ): void;

  off(
    event: 'userDataSet',
    callback?: SkyrimTogetherTypes.UserDataSetCallback,
  ): void;

  off(
    event: 'setHealth',
    callback?: SkyrimTogetherTypes.SetHealthCallback,
  ): void;

  off(event: 'setLevel', callback?: SkyrimTogetherTypes.SetLevelCallback): void;

  off(event: 'setCell', callback?: SkyrimTogetherTypes.SetCellCallback): void;

  /** Add listener to when a player is loaded or unloaded in 3D.  */
  off(
    event: 'setPlayer3dLoaded',
    callback?: SkyrimTogetherTypes.SetPlayer3dLoadedCallback,
  ): void;

  off(
    event: 'setPlayer3dUnloaded',
    callback?: SkyrimTogetherTypes.SetPlayer3dUnloadedCallback,
  ): void;

  off(
    event: 'setLocalPlayerId',
    callback?: SkyrimTogetherTypes.SetLocalPlayerIdCallback,
  ): void;

  off(
    event: 'protocolMismatch',
    callback?: SkyrimTogetherTypes.ProtocolMismatch,
  ): void;

  off(event: 'triggerError', callback?: SkyrimTogetherTypes.TriggerError): void;

  off(
    event: 'dummyData',
    callback?: SkyrimTogetherTypes.DummyDataCallback,
  ): void;

  off(
    event: 'partyInfo',
    callback?: SkyrimTogetherTypes.PartyInfoCallback,
  ): void;

  off(
    event: 'partyCreated',
    callback?: SkyrimTogetherTypes.PartyCreatedCallback,
  ): void;

  off(
    event: 'partyLeft',
    callback?: SkyrimTogetherTypes.PartyLeftCallback,
  ): void;

  off(
    event: 'partyInviteReceived',
    callback?: SkyrimTogetherTypes.PartyInviteReceivedCallback,
  ): void;

  off(event: 'respawnCountdown', callback?: (seconds: number) => void): void;

  off(
    event:
      | 'mainMenuOpen'
      | 'mainMenuData'
      | 'mainMenuSaves'
      | 'mainMenuSourceSaves'
      | 'mainMenuStatus'
      | 'mainMenuClose'
      | 'mainMenuOpenWorld'
      | 'social'
      | 'socialAvatar'
      | 'socialToast'
      | 'socialSidebar'
      | 'socialJoinAsk'
      | 'lanServers'
      | 'loadOrderResult'
      | 'buildToolsState'
      | 'questsState'
      | 'questTeamInvite'
      | 'questStarted'
      | 'settlementsState'
      | 'campCard'
      | 'companionName'
      | 'nemesisCard'
      | 'nemesisScreen'
      | 'nemesisGunpoint'
      | 'hud'
      | 'hudColor'
      | 'crosshair'
      | 'bike'
      | 'emoteWheel'
      | 'dialogueVote'
      | 'startMenu'
      | 'modConfig'
      | 'worldSettings',
    callback?: (...args: any[]) => void,
  ): void;

  /**
   * Fallout 4 main menu multiplayer screens: one of the player's choices.
   *
   * @param action What to do, e.g. "play", "createWorld".
   * @param payload Its arguments as a flat JSON object of strings.
   */
  mainMenuAction(action: string, payload: string): void;

  /**
   * Fallout 4, refused for our plugins: switches plugins.txt to the host's load order and restarts the game, or answers
   * with the loadOrderResult event when it can't.
   *
   * @param order The host's plugins in load order, one per line.
   */
  matchLoadOrder(order: string): void;

  /** Fallout 4 overlay buttons: one of the game's menu sounds (UIMenuOK, UIMenuFocus...), played by the game. */
  playSound(sound: string): void;

  /** Fallout 4 server browsers, LAN tab: looks for servers on the local network, answered with the lanServers event. */
  lanScan(): void;

  /**
   * Fallout 4 build tools settings.
   *
   * @param request "get", "bind" ("action:virtualKey"), "set" ("placeAnywhere:1"), or "reset" (default keys).
   * @param payload See request.
   */
  buildTools(request: string, payload: string): void;

  /**
   * Fallout 4 quest team-ups.
   *
   * @param request "get", "invite" ("questId:playerId"), "inviteAll" ("questId": the party, or everyone without one),
   * "accept" / "decline" ("questId:inviterId"), "leave" ("questId"), "mainQuestSync" ("1" / "0", party leader).
   * @param payload See request (questId: the quest's form id in hex).
   */
  quests(request: string, payload: string): void;

  /**
   * Fallout 4 adopted dogs, the name card.
   *
   * @param request "name" ("id:name") or "keep" ("id": keeps the name it has).
   * @param payload See request (id: the dog's record, form id in hex).
   */
  companions(request: string, payload: string): void;

  /**
   * Fallout 4 settlement claims (Player manager > Settlements). Claiming happens at the workbench.
   *
   * @param request "get", "rights" ("id:build:scrap:name", leader; build and scrap 1 or 0, the name last), "abandon"
   * ("id", leader) or "release" ("id", host).
   * @param payload See request (id: the workbench, "<mod hex>:<base hex>").
   */
  settlements(request: string, payload: string): void;

  /**
   * Fallout 4 nemeses: questioning a man who surrendered, and the nemesis screen.
   *
   * @param request "question" (payload: "<RequestNemesis::Question>" or "<question>:<caps>"), "close" (the card),
   * "screen" (payload: the page, "record", "gang:<id>", "man:<id>", "world" or "wounds") or "closeScreen".
   * @param payload See request.
   */
  nemeses(request: string, payload: string): void;

  /**
   * Fallout 4 Steam friends: the social sidebar (main menu, pause menu, its key in the world) and the Pip-Boy's Social tab.
   *
   * @param request "refresh", "avatars" (payload {"ids": "id,id,..."}), "join", "invite", "profile" (payload {"id": "<SteamID>"}),
   * "answer" (payload {"id", "accept": "1" or "0"}: someone asking to join the host's world), "privacy" (payload
   * {"value": "friends", "invite" or "private"}) or "close" (the sidebar opened with its key: the game's cursor back).
   * @param payload JSON, see request; SteamIDs as text.
   */
  social(request: string, payload: string): void;

  /**
   * Fallout 4 emotes: the wheel and the chat's emote commands.
   *
   * @param request "hover" (payload: the emote's name under the pointer, "" none), "play" (payload: its name; the wheel
   * closes), "close" (the wheel, playing nothing) or "command" (payload: what followed the "/": an emote, or "emotes").
   * @param payload See request.
   */
  emotes(request: string, payload: string): void;

  /**
   * Fallout 4 co-op dialogue: the vote card.
   *
   * @param request "cast" (payload: the option's index) or "close" (the card lets go of the input without a vote).
   * @param payload See request.
   */
  dialogueVote(request: string, payload: string): void;

  /**
   * Connect to server at given address and port.
   *
   * @param host IP address or hostname.
   * @param port Port.
   * @param password Server password.
   * @param username Name shown to other players, empty for the character's name.
   */
  connect(host: string, port: number, password: string, username?: string): void;

  /**
   * Disconnect from server or cancel connection.
   */
  disconnect(): void;

  /**
   * Send message to server.
   */
  sendMessage(type: number, message: string): void;

  /**
   * Send a request to the server for changing the in-game time.
   */
  setTime(hours: number, minutes: number): void;

  /**
   * Deactivate UI and release control.
   */
  deactivate(): void;

  /**
   * Teleport to given player
   *
   * @param playerId Id of the player to which the requester should be teleported to
   */
  teleportToPlayer(playerId: number): void;

  /**
   * Reconnect the client.
   */
  reconnect(): void;

  /**
   * Launch a party.
   */
  launchParty(): void;

  /**
   * Send a party invite to player with player id.
   *
   * @param playerId Id of the player to which the invite should be sent.
   */
  createPartyInvite(playerId: number): void;

  /**
   * Accept a party invite.
   *
   * @param inviterId Id of the player who sent the invite.
   */
  acceptPartyInvite(inviterId: number): void;

  /**
   * As a party leader, kick a member from the party.
   *
   * @param playerId Id of the player who gets kicked.
   */
  kickPartyMember(playerId: number): void;

  /**
   * Leave the currently joined party.
   */
  leaveParty(): void;

  /**
   * As a party leader, make someone else the leader.
   *
   * @param playerId Id of the new leader.
   */
  changePartyLeader(playerId: number): void;
}
