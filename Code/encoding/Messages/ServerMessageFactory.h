#pragma once

#include <TiltedCore/Stl.hpp>
#include <Messages/Message.h>
#include <MetaMessage.h>

#include <Messages/AuthenticationResponse.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Messages/ServerTimeSettings.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/NotifyFactionsChanges.h>
#include <Messages/NotifyRemoveCharacter.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/NotifyPlayerList.h>
#include <Messages/NotifyPartyInfo.h>
#include <Messages/NotifyPartyInvite.h>
#include <Messages/NotifyPartyJoined.h>
#include <Messages/NotifyPartyLeft.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/NotifyActivate.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/NotifyDeathStateChange.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/NotifySpellCast.h>
#include <Messages/NotifyInterruptCast.h>
#include <Messages/NotifyAddTarget.h>
#include <Messages/NotifyProjectileLaunch.h>
#include <Messages/NotifyScriptAnimation.h>
#include <Messages/NotifyDrawWeapon.h>
#include <Messages/NotifyMount.h>
#include <Messages/NotifyNewPackage.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/NotifySyncExperience.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/TeleportCommandResponse.h>
#include <Messages/StringCacheUpdate.h>
#include <Messages/NotifyPlayerRespawn.h>
#include <Messages/NotifyDialogue.h>
#include <Messages/NotifySubtitle.h>
#include <Messages/NotifyPlayerDialogue.h>
#include <Messages/NotifyActorTeleport.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/NotifyTeleport.h>
#include <Messages/NotifyPlayerHealthUpdate.h>
#include <Messages/NotifySettingsChange.h>
#include <Messages/NotifyWeatherChange.h>
#include <Messages/NotifySetWaypoint.h>
#include <Messages/NotifyRemoveWaypoint.h>
#include <Messages/NotifySetTimeResult.h>
#include <Messages/NotifyQuestItemTaken.h>
#include <Messages/NotifyActorTrade.h>
#include <Messages/NotifyTerminalItemRun.h>
#include <Messages/NotifyDestructionStage.h>
#include <Messages/NotifyPipboyLight.h>
#include <Messages/NotifySettlementRatings.h>
#include <Messages/NotifySettlementProduction.h>
#include <Messages/NotifyFactionStanding.h>
#include <Messages/NotifySharedQuest.h>
#include <Messages/NotifyDialogueLock.h>
#include <Messages/NotifyPowerArmor.h>
#include <Messages/NotifyItemDropped.h>
#include <Messages/NotifyItemPickedUp.h>
#include <Messages/NotifyWorkshopChange.h>
#include <Messages/NotifyQuestTeam.h>
#include <Messages/NotifyMovementAction.h>
#include <Messages/NotifySettlementClaims.h>
#include <Messages/NotifyCamps.h>
#include <Messages/NotifyNemesis.h>
#include <Messages/NotifyDialogueVote.h>

using TiltedPhoques::UniquePtr;

struct ServerMessageFactory
{
    UniquePtr<ServerMessage> Extract(TiltedPhoques::Buffer::Reader& aReader) const noexcept;

    template <class T> static auto Visit(T&& func)
    {
        auto s_visitor = CreateMessageVisitor<
            AuthenticationResponse, AssignCharacterResponse, ServerReferencesMoveRequest, ServerTimeSettings, CharacterSpawnRequest, NotifyInventoryChanges, StringCacheUpdate, NotifyFactionsChanges, NotifyRemoveCharacter, NotifyQuestUpdate, NotifyPlayerList, NotifyPartyInfo, NotifyPartyInvite,
            NotifyActorValueChanges, NotifyPartyJoined, NotifyPartyLeft, NotifyActorMaxValueChanges, NotifyHealthChangeBroadcast, NotifyActivate, NotifyLockChange, AssignObjectsResponse, NotifyDeathStateChange, NotifyOwnershipTransfer, NotifyObjectInventoryChanges, NotifySpellCast,
            NotifyProjectileLaunch, NotifyInterruptCast, NotifyAddTarget, NotifyScriptAnimation, NotifyDrawWeapon, NotifyMount, NotifyNewPackage, NotifyRespawn, NotifySyncExperience, NotifyEquipmentChanges, NotifyChatMessageBroadcast, TeleportCommandResponse, NotifyPlayerRespawn, NotifyDialogue,
            NotifySubtitle, NotifyPlayerDialogue, NotifyActorTeleport, NotifyPlayerLeft, NotifyPlayerJoined, NotifyDialogue, NotifySubtitle, NotifyPlayerDialogue, NotifyPlayerLevel, NotifyPlayerCellChanged, NotifyTeleport, NotifyPlayerHealthUpdate, NotifySettingsChange,
            NotifyWeatherChange, NotifySetWaypoint, NotifyRemoveWaypoint, NotifySetTimeResult, NotifyQuestItemTaken, NotifyDialogueLock, NotifyPowerArmor, NotifyItemDropped, NotifyItemPickedUp, NotifyWorkshopChange, NotifyQuestTeam, NotifyMovementAction, NotifySettlementClaims, NotifyCamps, NotifyNemesis, NotifyDialogueVote, NotifyActorTrade, NotifyTerminalItemRun, NotifyDestructionStage, NotifyPipboyLight, NotifySettlementRatings, NotifySettlementProduction, NotifyFactionStanding, NotifySharedQuest>;

        return s_visitor(std::forward<T>(func));
    }
};
