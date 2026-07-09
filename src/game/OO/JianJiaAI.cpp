#include "JianJiaAI.h"
#include "PlayerBots/PlayerBotMgr.h"
#include "Server/WorldSession.h"
#include "Log.h"

bool JianJiaAI::OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess)
{
    // Load the existing character from DB (same as base class)
    sess->LoginPlayer(entry->playerGUID);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "JianJiaAI: 蒹葭 (guid %u) logged in.", entry->playerGUID);
    return true;
}
