#include "Common.h"
#include "Policies/SingletonImp.h"
#include "PlayerBotMgr.h"
#include "OO/OOMgr.h"
#include "OO/WebChatMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "World.h"
#include "WorldSession.h"
#include "AccountMgr.h"
#include "Config/Config.h"
#include "Chat.h"
#include "Player.h"
#include "Group.h"
#include "PlayerBotAI.h"
#include "PartyBotAI.h"
#include "BattleBotAI.h"
#include "BattleBotWaypoints.h"
#include "BattleGroundMgr.h"
#include "MapManager.h"
#include "Language.h"
#include "Utilities/Random.h"
#include "Spell.h"

INSTANTIATE_SINGLETON_1(PlayerBotMgr);


PlayerBotMgr::PlayerBotMgr()
{
    m_totalChance = 0;
    m_maxAccountId = 0;

    // Config
    m_confMinRandomBots         = 3;
    m_confMaxRandomBots         = 10;
    m_confRandomBotsRefresh     = 60000;
    m_confUpdateDiff            = 10000;
    m_confEnableRandomBots      = false;
    m_confDebug                 = false;
    m_confBattleBotAutoJoin_1   = false;
    m_confBattleBotAutoJoin_2   = false;
    m_confBattleBotAutoJoin_3   = false;
    m_confBattleBotUpdateMs     = 1000;
    m_confBattleBotUpdateMsWSG  = 700;
    m_confBattleBotUpdateMsAB   = 700;
    m_confBattleBotUpdateMsBR   = 400;

    // Time
    m_elapsedTime = 0;
    m_lastBotsRefresh = 0;
    m_lastUpdate = 0;
    m_lastBattleBotQueueUpdate = 0;
}

PlayerBotMgr::~PlayerBotMgr()
{

}

void PlayerBotMgr::LoadConfig()
{
    m_confEnableRandomBots = sConfig.GetBoolDefault("RandomBot.Enable", false);
    m_confMinRandomBots = sConfig.GetIntDefault("RandomBot.MinBots", 3);
    m_confMaxRandomBots = sConfig.GetIntDefault("RandomBot.MaxBots", 10);
    m_confRandomBotsRefresh = sConfig.GetIntDefault("RandomBot.Refresh", 60000);
    m_confAllowSaving = sConfig.GetBoolDefault("PlayerBot.AllowSaving", false);
    m_confDebug = sConfig.GetBoolDefault("PlayerBot.Debug", false);
    m_confUpdateDiff = sConfig.GetIntDefault("PlayerBot.UpdateMs", 10000);
    m_confBattleBotAutoJoin_1 = sConfig.GetBoolDefault("BattleBot.AutoJoin.AV", false);
    m_confBattleBotAutoJoin_2 = sConfig.GetBoolDefault("BattleBot.AutoJoin.WSG", false);
    m_confBattleBotAutoJoin_3 = sConfig.GetBoolDefault("BattleBot.AutoJoin.AB", false);
    m_confBattleBotUpdateMs    = sConfig.GetIntDefault("BattleBot.UpdateMs",     1000);
    m_confBattleBotUpdateMsWSG = sConfig.GetIntDefault("BattleBot.UpdateMs.WSG",  700);
    m_confBattleBotUpdateMsAB  = sConfig.GetIntDefault("BattleBot.UpdateMs.AB",   700);
    m_confBattleBotUpdateMsBR  = sConfig.GetIntDefault("BattleBot.UpdateMs.BR",   400);

    if (!sWorld.getConfig(CONFIG_BOOL_FORCE_LOGOUT_DELAY))
        m_tempBots.clear();
}

void PlayerBotMgr::Load()
{
    // 1- Clean
    DeleteAll();
    m_bots.clear();
    m_tempBots.clear();
    m_totalChance = 0;

    // 2- Configuration
    LoadConfig();

    // Override autojoin flags from saved_variables if a runtime toggle was previously issued.
    // This ensures the state survives server crashes and restarts.
    // If no DB entry exists (fresh start), the config-file value is kept as-is.
    {
        bool exists = false;
        uint32 val = sObjectMgr.GetSavedVariable(VAR_BATTLEBOT_AUTOJOIN_AV, 0, &exists);
        if (exists) m_confBattleBotAutoJoin_1 = val != 0;
        val = sObjectMgr.GetSavedVariable(VAR_BATTLEBOT_AUTOJOIN_WSG, 0, &exists);
        if (exists) m_confBattleBotAutoJoin_2 = val != 0;
        val = sObjectMgr.GetSavedVariable(VAR_BATTLEBOT_AUTOJOIN_AB, 0, &exists);
        if (exists) m_confBattleBotAutoJoin_3 = val != 0;
    }

    // 3- Load usable account ID
    std::unique_ptr<QueryResult> result = LoginDatabase.PQuery(
                              "SELECT MAX(`id`)"
                              " FROM `account`");
    if (!result)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "Playerbot: unable to load max account id.");
        return;
    }
    Field* fields = result->Fetch();
    m_maxAccountId = fields[0].GetUInt32() + 10000;

    // 4- LoadFromDB
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> [PlayerBotMgr] Loading Bots ...");
    result = CharacterDatabase.PQuery(
                 "SELECT char_guid, chance, ai"
                 " FROM playerbot");
    if (!result)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "Table `playerbot` is empty.");
    else
    {
        do
        {
            fields = result->Fetch();
            uint32 guid = fields[0].GetUInt32();
            uint32 acc = GenBotAccountId();
            uint32 chance = fields[1].GetUInt32();

            std::shared_ptr<PlayerBotEntry> entry = std::make_shared<PlayerBotEntry>(guid, acc, chance);
            entry->ai.reset(CreatePlayerBotAI(fields[2].GetCppString()));
            entry->ai->botEntry = entry.get();
            if (!sObjectMgr.GetPlayerNameByGUID(guid, entry->name))
                entry->name = "<Unknown>";
            entry->ai->OnBotEntryLoad(entry.get());
            m_bots.insert({ entry->playerGUID, entry });
            m_totalChance += chance;
        }
        while (result->NextRow());
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "%u bots loaded", m_bots.size());
    }

    // 5- Check config/DB
    if (m_confMinRandomBots >= m_bots.size() && !m_bots.empty())
        m_confMinRandomBots = m_bots.size() - 1;
    if (m_confMaxRandomBots > m_bots.size())
        m_confMaxRandomBots = m_bots.size();
    if (m_confMaxRandomBots <= m_confMinRandomBots)
        m_confMaxRandomBots = m_confMinRandomBots + 1;

    // 6- Start initial bots
    if (m_confEnableRandomBots)
    {
        for (uint32 i = 0; i < m_confMinRandomBots; i++)
            AddRandomBot();
    }

    // 7- Fill stats info
    m_stats.confMaxOnline = m_confMaxRandomBots;
    m_stats.confMinOnline = m_confMinRandomBots;
    m_stats.totalBots = m_bots.size();
    m_stats.confRandomBotsRefresh = m_confRandomBotsRefresh;
    m_stats.confUpdateDiff = m_confUpdateDiff;

    // 8- Show stats if debug
    if (m_confDebug)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PlayerBotMgr] Between %u and %u bots online", m_confMinRandomBots, m_confMaxRandomBots);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PlayerBotMgr] %u now loading", m_stats.loadingCount);
    }
}

void PlayerBotMgr::DeleteAll()
{
    m_stats.onlineCount = 0;
    m_stats.loadingCount = 0;

    for (auto i = m_bots.begin(); i != m_bots.end(); i++)
    {
        if (i->second->state != PB_STATE_OFFLINE)
        {
            OnBotLogout(i->second.get());
            m_totalChance += i->second->chance;
        }
    }
    m_tempBots.clear();

    if (m_confDebug)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PlayerBotMgr] Deleting all bots [OK]");
}

void PlayerBotMgr::OnBotLogin(PlayerBotEntry *e)
{
    e->state = PB_STATE_ONLINE;
    e->loadingStartTime = 0;
    if (m_confDebug)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PlayerBot][Login]  '%s' GUID:%u Acc:%u", e->name.c_str(), e->playerGUID, e->accountId);
}
void PlayerBotMgr::OnBotLogout(PlayerBotEntry *e)
{
    e->state = PB_STATE_OFFLINE;
    e->loadingStartTime = 0;
    if (m_confDebug)
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[PlayerBot][Logout] '%s' GUID:%u Acc:%u", e->name.c_str(), e->playerGUID, e->accountId);
}

void PlayerBotMgr::OnPlayerInWorld(Player* player)
{
    if (PlayerBotEntry* e = player->GetSession()->GetBot())
    {
        player->SetAI(e->ai.get());
        e->ai->SetPlayer(player);
        e->ai->OnPlayerLogin();
    }
}

uint32 const BATTLEBOT_WSG_MIN_BOTS_PER_TEAM = 5;

static uint32 GetBattleBotFillTarget(BattleGroundTypeId bgTypeId, BattleGround const* bg)
{
    uint32 desiredCount = bg->GetMinPlayersPerTeam();
    switch (bgTypeId)
    {
        case BATTLEGROUND_WS:
            desiredCount = 8;
            break;
        case BATTLEGROUND_AB:
            desiredCount = 12;
            break;
        case BATTLEGROUND_AV:
            desiredCount = 30;
            break;
        default:
            break;
    }

    if (bg->GetMaxPlayersPerTeam() <= 1)
        return 0;

    return std::min<uint32>(desiredCount, bg->GetMaxPlayersPerTeam() - 1);
}

static uint32 GetBattleBotMaxAutoTeamCount(BattleGroundTypeId bgTypeId, BattleGround const* bg)
{
    // Always reserve 1 slot for a real player; the lock-fill at 5 min fills the last slot.
    if (bg->GetMaxPlayersPerTeam() > 1)
        return bg->GetMaxPlayersPerTeam() - 1;

    return bg->GetMaxPlayersPerTeam();
}

static bool BattleBotLoadingCountsForQueue(PlayerBotEntry const* entry)
{
    uint32 const loadingCountWindow = 8;
    return entry && entry->state == PB_STATE_LOADING && entry->loadingStartTime &&
        entry->loadingStartTime + loadingCountWindow >= sWorld.GetGameTime();
}

static bool BattleBotIsActiveForQueue(BattleBotAI const* battleBotAI, BattleGroundQueue& bgQueue, BattleGroundQueueTypeId queueType)
{
    if (!battleBotAI || battleBotAI->m_battlegroundId != queueType)
        return false;

    Player* bot = battleBotAI->me;
    if (!bot)
        return false;

    BattleGroundTypeId const bgTypeId = BattleGroundMgr::BgTemplateId(queueType);
    if (bot->InBattleGround())
    {
        if (BattleGround* bg = bot->GetBattleGround())
            return bg->GetTypeID() == bgTypeId;

        return bot->GetBattleGroundTypeId() == bgTypeId;
    }

    if (!bot->InBattleGroundQueueForBattleGroundQueueType(queueType))
        return false;

    GroupQueueInfo groupInfo;
    if (!bgQueue.GetPlayerGroupInfoData(bot->GetObjectGuid(), &groupInfo))
        return false;

    return groupInfo.bgTypeId == bgTypeId;
}

void PlayerBotMgr::Update(uint32 diff)
{
    // Temporary bots: decrement timer and clean up expired entries in a single
    // pass. Previous implementation iterated twice and restarted iteration from
    // begin() after every erase — O(T^2) plus an O(T*N) inner scan over m_bots.
    // erase() returns the iterator to the next element, so we keep O(T) here
    // (the O(N) inner search by accountId is unchanged but T is typically tiny).
    for (auto it = m_tempBots.begin(); it != m_tempBots.end(); )
    {
        if (it->second <= diff)
        {
            // Update of "chatBot" too.
            for (auto iter = m_bots.begin(); iter != m_bots.end(); ++iter)
            {
                if (iter->second->accountId == it->first)
                {
                    iter->second->state = PB_STATE_OFFLINE; // Will get logged out at next WorldSession::Update call
                    iter->second->loadingStartTime = 0;
                    m_bots.erase(iter);
                    break;
                }
            }
            it = m_tempBots.erase(it);
        }
        else
        {
            it->second -= diff;
            ++it;
        }
    }

    m_elapsedTime += diff;
    if (!((m_elapsedTime - m_lastUpdate) > m_confUpdateDiff))
        return; // No need to update
    m_lastUpdate = m_elapsedTime;

    for (auto iter = m_bots.begin(); iter != m_bots.end();)
    {
        if (!m_confEnableRandomBots && !iter->second->customBot)
        {
            ++iter;
            continue;
        }

        if (iter->second->state == PB_STATE_ONLINE)
        {
            if (iter->second->requestRemoval)
            {
                if (iter->second->ai && iter->second->ai->me)
                {
                    if (!iter->second->ai->me->IsAlive())
                    {
                        // don't leave permanent corpse
                        iter->second->ai->me->ResurrectPlayer(1.0f);
                        iter->second->ai->me->SpawnCorpseBones();
                    }
                    iter->second->ai->me->RemoveFromGroup();
                }

                DeleteBot(iter);

                if (WorldSession* sess = sWorld.FindSession(iter->second->accountId))
                    sess->LogoutPlayer(m_confAllowSaving);

                iter->second->requestRemoval = false;

                if (iter->second->customBot)
                    iter = m_bots.erase(iter);
                else
                    ++iter;
                continue;
            }
        }

        // Connection of pending bots
        if (iter->second->state != PB_STATE_LOADING)
        {
            ++iter;
            continue;
        }

        WorldSession* sess = sWorld.FindSession(iter->second->accountId);

        if (!sess)
        {
            // This may happen : just wait for the World to add the session.
            ++iter;
            continue;
        }

        if (iter->second->ai->OnSessionLoaded(iter->second.get(), sess))
        {
            OnBotLogin(iter->second.get());
            m_stats.loadingCount--;

            if (iter->second->isChatBot)
                m_stats.onlineChat++;
            else
                m_stats.onlineCount++;
        }
        else
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "PLAYERBOT: Unable to load session id %u", iter->second->accountId);
            DeleteBot(iter);

            if (iter->second->customBot)
                iter = m_bots.erase(iter);
            else
                ++iter;
            continue;
        }

        ++iter;
    }

    if ((m_lastBattleBotQueueUpdate <= (sWorld.GetGameTime() - 5)))
    {
        m_lastBattleBotQueueUpdate = sWorld.GetGameTime();
        for (uint32 queueType = BATTLEGROUND_QUEUE_AV; queueType < MAX_BATTLEGROUND_QUEUE_TYPES; ++queueType)
        {
            bool hasPlayerInQueue[MAX_BATTLEGROUND_BRACKETS] = {};
            uint32 waitingAlliancePlayers[MAX_BATTLEGROUND_BRACKETS] = {};
            uint32 waitingHordePlayers[MAX_BATTLEGROUND_BRACKETS] = {};
            uint32 queuedAllianceCount[MAX_BATTLEGROUND_BRACKETS] = {};
            uint32 queuedHordeCount[MAX_BATTLEGROUND_BRACKETS] = {};
            BattleGround* inProgressBg[MAX_BATTLEGROUND_BRACKETS] = {};
            uint32 inProgressTarget[MAX_BATTLEGROUND_BRACKETS] = {};
            BattleGroundQueue& bgQueue = sBattleGroundMgr.m_battleGroundQueues[queueType];
            BattleGroundQueueTypeId const queueTypeId = BattleGroundQueueTypeId(queueType);
            BattleGroundTypeId bgTypeId = BattleGroundMgr::BgTemplateId(BattleGroundQueueTypeId(queueType));
            for (auto const& itr : bgQueue.m_queuedPlayers)
            {
                if (Player* pPlayer = ObjectAccessor::FindPlayerNotInWorld(itr.first))
                {
                    BattleGroundBracketId bgBracketId = pPlayer->GetBattleGroundBracketIdFromLevel(itr.second.groupInfo->bgTypeId);
                    if (bgBracketId == BG_BRACKET_ID_NONE)
                        continue;

                    if (!pPlayer->IsBot())
                    {
                        if (itr.second.groupInfo->groupTeam == ALLIANCE)
                        {
                            ++queuedAllianceCount[bgBracketId];
                            if (!itr.second.groupInfo->isInvitedToBgInstanceGuid)
                                ++waitingAlliancePlayers[bgBracketId];
                        }
                        else
                        {
                            ++queuedHordeCount[bgBracketId];
                            if (!itr.second.groupInfo->isInvitedToBgInstanceGuid)
                                ++waitingHordePlayers[bgBracketId];
                        }

                        hasPlayerInQueue[bgBracketId] = true;
                    }
                }
            }

            for (auto const& botItr : m_bots)
            {
                PlayerBotEntry const* entry = botItr.second.get();
                if (!entry || entry->requestRemoval || entry->state == PB_STATE_OFFLINE)
                    continue;

                BattleBotAI* pBattleBotAI = dynamic_cast<BattleBotAI*>(entry->ai.get());
                if (!pBattleBotAI || pBattleBotAI->m_battlegroundId != queueType)
                    continue;

                if (entry->state == PB_STATE_LOADING)
                {
                    if (!BattleBotLoadingCountsForQueue(entry))
                        continue;
                }
                else if (!BattleBotIsActiveForQueue(pBattleBotAI, bgQueue, queueTypeId))
                    continue;

                BattleGroundBracketId bgBracketId = Player::GetBattleGroundBracketIdFromLevel(bgTypeId, pBattleBotAI->m_level);
                if (bgBracketId == BG_BRACKET_ID_NONE)
                    continue;

                Team const botTeam = Player::TeamForRace(pBattleBotAI->m_race);
                if (botTeam == ALLIANCE)
                    ++queuedAllianceCount[bgBracketId];
                else
                    ++queuedHordeCount[bgBracketId];
            }

            for (auto itr = sBattleGroundMgr.GetBattleGroundsBegin(bgTypeId); itr != sBattleGroundMgr.GetBattleGroundsEnd(bgTypeId); ++itr)
            {
                BattleGround* runningBg = itr->second;
                if (!runningBg || runningBg->GetClientInstanceID() == 0)
                    continue;

                if (runningBg->GetStatus() <= STATUS_WAIT_QUEUE || runningBg->GetStatus() >= STATUS_WAIT_LEAVE)
                    continue;

                BattleGroundBracketId bgBracketId = runningBg->GetBracketId();
                if (bgBracketId == BG_BRACKET_ID_NONE)
                    continue;

                if (runningBg->GetStatus() != STATUS_IN_PROGRESS)
                    continue;

                inProgressBg[bgBracketId] = runningBg;
                // Locked BGs are filled to max; unlocked BGs keep 1 slot open for a real player.
                inProgressTarget[bgBracketId] = runningBg->IsLocked()
                    ? runningBg->GetMaxPlayersPerTeam()
                    : (runningBg->GetMaxPlayersPerTeam() > 1 ? runningBg->GetMaxPlayersPerTeam() - 1 : 0);

                for (auto const& playerItr : runningBg->GetPlayers())
                {
                    Player const* player = sObjectMgr.GetPlayer(playerItr.first);
                    if (!player || player->IsBot())
                        continue;

                    if (player->GetTeam() == ALLIANCE)
                        ++queuedAllianceCount[bgBracketId];
                    else
                        ++queuedHordeCount[bgBracketId];

                    hasPlayerInQueue[bgBracketId] = true;
                }
            }

            auto removeInvitedBattleBot = [&](BattleGround* runningBg, Team team, BattleGroundBracketId bracketId) -> bool
            {
                for (auto const& botItr : m_bots)
                {
                    PlayerBotEntry* entry = botItr.second.get();
                    if (!entry || entry->requestRemoval || entry->state == PB_STATE_OFFLINE)
                        continue;

                    BattleBotAI* pBattleBotAI = dynamic_cast<BattleBotAI*>(entry->ai.get());
                    if (!pBattleBotAI || !pBattleBotAI->m_temporary || pBattleBotAI->m_battlegroundId != queueType)
                        continue;

                    if (Player::TeamForRace(pBattleBotAI->m_race) != team)
                        continue;

                    if (Player::GetBattleGroundBracketIdFromLevel(bgTypeId, pBattleBotAI->m_level) != bracketId)
                        continue;

                    Player* bot = pBattleBotAI->me;
                    if (!bot || bot->InBattleGround())
                        continue;

                    GroupQueueInfo groupInfo;
                    if (!bgQueue.GetPlayerGroupInfoData(bot->GetObjectGuid(), &groupInfo))
                        continue;

                    if (groupInfo.isInvitedToBgInstanceGuid != runningBg->GetInstanceID())
                        continue;

                    bot->RemoveBattleGroundQueueId(BattleGroundQueueTypeId(queueType));
                    bgQueue.RemovePlayer(bot->GetObjectGuid(), true);
                    entry->requestRemoval = true;
                    sLog.Out(LOG_BG, LOG_LVL_BASIC, "[PlayerBotMgr] Removed invited temporary battlebot %u from queue %u to free a player slot.", bot->GetGUIDLow(), queueType);
                    return true;
                }

                return false;
            };

            auto trimBattleBotForStartedBg = [&](BattleGround* runningBg, Team team, BattleGroundBracketId bracketId) -> bool
            {
                uint32 const target = inProgressTarget[bracketId];
                if (!target)
                    return false;

                uint32 const totalCount = team == ALLIANCE ? queuedAllianceCount[bracketId] : queuedHordeCount[bracketId];
                if (totalCount <= target)
                    return false;

                if (removeInvitedBattleBot(runningBg, team, bracketId))
                {
                    if (team == ALLIANCE)
                        --queuedAllianceCount[bracketId];
                    else
                        --queuedHordeCount[bracketId];

                    sBattleGroundMgr.ScheduleQueueUpdate(BattleGroundQueueTypeId(queueType), bgTypeId, bracketId);
                    return true;
                }

                if (runningBg->GetPlayersCountByTeam(team) > target &&
                    runningBg->GetBotPlayersCountByTeam(team) > (bgTypeId == BATTLEGROUND_WS ? BATTLEBOT_WSG_MIN_BOTS_PER_TEAM : 0) &&
                    runningBg->DeleteBattleBot(team))
                {
                    if (team == ALLIANCE)
                        --queuedAllianceCount[bracketId];
                    else
                        --queuedHordeCount[bracketId];

                    runningBg->AddToBGFreeSlotQueue();
                    sBattleGroundMgr.ScheduleQueueUpdate(BattleGroundQueueTypeId(queueType), bgTypeId, bracketId);
                    sLog.Out(LOG_BG, LOG_LVL_BASIC, "[PlayerBotMgr] Removed battlebot from started queue %u team %u to keep team at max-1.", queueType, team);
                    return true;
                }

                return false;
            };

            auto freeBattleBotSlotForWaitingPlayer = [&](Team team, BattleGroundBracketId bracketId) -> bool
            {
                for (auto itr = sBattleGroundMgr.GetBattleGroundsBegin(bgTypeId); itr != sBattleGroundMgr.GetBattleGroundsEnd(bgTypeId); ++itr)
                {
                    BattleGround* runningBg = itr->second;
                    if (!runningBg || runningBg->GetClientInstanceID() == 0)
                        continue;

                    if (runningBg->GetBracketId() != bracketId)
                        continue;

                    if (runningBg->GetStatus() < STATUS_WAIT_JOIN || runningBg->GetStatus() >= STATUS_WAIT_LEAVE)
                        continue;

                    // Never evict bots from locked BGs to make room for real players.
                    if (runningBg->IsLocked())
                        continue;

                    if (runningBg->GetFreeSlotsForTeam(team) > 0)
                        continue;

                    if (removeInvitedBattleBot(runningBg, team, bracketId))
                    {
                        sBattleGroundMgr.ScheduleQueueUpdate(BattleGroundQueueTypeId(queueType), bgTypeId, bracketId);
                        return true;
                    }

                    if (runningBg->GetBotPlayersCountByTeam(team) > (bgTypeId == BATTLEGROUND_WS ? BATTLEBOT_WSG_MIN_BOTS_PER_TEAM : 0) &&
                        runningBg->DeleteBattleBot(team))
                    {
                        runningBg->AddToBGFreeSlotQueue();
                        sBattleGroundMgr.ScheduleQueueUpdate(BattleGroundQueueTypeId(queueType), bgTypeId, bracketId);
                        sLog.Out(LOG_BG, LOG_LVL_BASIC, "[PlayerBotMgr] Removed in-battleground battlebot from queue %u team %u to free a player slot.", queueType, team);
                        return true;
                    }
                }

                return false;
            };

            for (uint32 bracketId = BG_BRACKET_ID_FIRST; bracketId < MAX_BATTLEGROUND_BRACKETS; ++bracketId)
            {
                if (!hasPlayerInQueue[bracketId])
                    continue;

                if (!queuedAllianceCount[bracketId] && !queuedHordeCount[bracketId])
                    continue;

                BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId);
                ASSERT(bg);

                uint32 const minLevel = bg->GetMinLevel() + 10 * bracketId;
                ASSERT(minLevel <= PLAYER_MAX_LEVEL);
                uint32 const maxLevel = std::min<uint32>(minLevel + 9, PLAYER_MAX_LEVEL);
                bool const isStartedBg = inProgressBg[bracketId] && inProgressTarget[bracketId];
                uint32 const fillTarget = isStartedBg ? inProgressTarget[bracketId] : GetBattleBotFillTarget(bgTypeId, bg);
                uint32 const maxAutoTeamCount = isStartedBg ? inProgressTarget[bracketId] : GetBattleBotMaxAutoTeamCount(bgTypeId, bg);
                uint32 const queueFillTarget = isStartedBg ? fillTarget : std::min<uint32>(maxAutoTeamCount, fillTarget + 1);

                bool toAddBattleBot = false;
                // BattleBot AutoJoin
                if (bgTypeId == BATTLEGROUND_AV)
                {
                    if (m_confBattleBotAutoJoin_1)
                        toAddBattleBot = true;
                }
                if (bgTypeId == BATTLEGROUND_WS)
                {
                    if (m_confBattleBotAutoJoin_2)
                        toAddBattleBot = true;
                }
                if (bgTypeId == BATTLEGROUND_AB)
                {
                    if (m_confBattleBotAutoJoin_3)
                        toAddBattleBot = true;
                }

                if (toAddBattleBot)
                {
                    if (isStartedBg)
                    {
                        for (auto bgItr = sBattleGroundMgr.GetBattleGroundsBegin(bgTypeId); bgItr != sBattleGroundMgr.GetBattleGroundsEnd(bgTypeId); ++bgItr)
                        {
                            BattleGround* runningBg = bgItr->second;
                            if (!runningBg || runningBg->GetClientInstanceID() == 0 ||
                                runningBg->GetBracketId() != BattleGroundBracketId(bracketId) ||
                                runningBg->GetStatus() != STATUS_IN_PROGRESS)
                                continue;
                            if (runningBg->IsLocked())
                                continue;
                            trimBattleBotForStartedBg(runningBg, ALLIANCE, BattleGroundBracketId(bracketId));
                            trimBattleBotForStartedBg(runningBg, HORDE, BattleGroundBracketId(bracketId));
                        }
                    }

                    if (waitingAlliancePlayers[bracketId])
                        if (freeBattleBotSlotForWaitingPlayer(ALLIANCE, BattleGroundBracketId(bracketId)))
                            --queuedAllianceCount[bracketId];
                    if (waitingHordePlayers[bracketId])
                        if (freeBattleBotSlotForWaitingPlayer(HORDE, BattleGroundBracketId(bracketId)))
                            --queuedHordeCount[bracketId];

                    uint32 const allianceTarget = std::min<uint32>(maxAutoTeamCount, std::max<uint32>(queueFillTarget, queuedHordeCount[bracketId]));
                    uint32 const hordeTarget = std::min<uint32>(maxAutoTeamCount, std::max<uint32>(queueFillTarget, queuedAllianceCount[bracketId]));

                    bool botsAdded = false;
                    for (uint32 i = queuedAllianceCount[bracketId]; i < allianceTarget; ++i)
                    {
                        uint32 const botLevel = urand(minLevel, maxLevel);
                        if (maxLevel > 50)
                        {
                            AddBattleBot(BattleGroundQueueTypeId(queueType), ALLIANCE, maxLevel, true);
                        }
                        else
                        {
                            AddBattleBot(BattleGroundQueueTypeId(queueType), ALLIANCE, botLevel, true);
                        }
                        ++queuedAllianceCount[bracketId];
                        botsAdded = true;
                    }
                    for (uint32 i = queuedHordeCount[bracketId]; i < hordeTarget; ++i)
                    {
                        uint32 const botLevel = urand(minLevel, maxLevel);
                        if (maxLevel > 50)
                        {
                            AddBattleBot(BattleGroundQueueTypeId(queueType), HORDE, maxLevel, true);
                        }
                        else
                        {
                            AddBattleBot(BattleGroundQueueTypeId(queueType), HORDE, botLevel, true);
                        }
                        ++queuedHordeCount[bracketId];
                        botsAdded = true;
                    }
                    if (botsAdded)
                        sBattleGroundMgr.ScheduleQueueUpdate(BattleGroundQueueTypeId(queueType), bgTypeId, BattleGroundBracketId(bracketId));

                    // If all in-progress BGs are locked (full and bots at minimum) for
                    // waiting real players, generate bots to support opening a new BG.
                    if (isStartedBg)
                    {
                        uint32 const minBotsPerTeam = bgTypeId == BATTLEGROUND_WS ? BATTLEBOT_WSG_MIN_BOTS_PER_TEAM : 0u;
                        bool allianceLocked = waitingAlliancePlayers[bracketId] > 0;
                        bool hordeLocked    = waitingHordePlayers[bracketId] > 0;
                        uint32 totalExistingAlliance = 0, totalExistingHorde = 0;
                        uint32 totalFreeAlliance = 0, totalFreeHorde = 0;

                        for (auto bgItr = sBattleGroundMgr.GetBattleGroundsBegin(bgTypeId); bgItr != sBattleGroundMgr.GetBattleGroundsEnd(bgTypeId); ++bgItr)
                        {
                            BattleGround* runningBg = bgItr->second;
                            if (!runningBg || runningBg->GetClientInstanceID() == 0 ||
                                runningBg->GetBracketId() != BattleGroundBracketId(bracketId) ||
                                runningBg->GetStatus() != STATUS_IN_PROGRESS)
                                continue;

                            totalExistingAlliance += runningBg->GetPlayersCountByTeam(ALLIANCE);
                            totalExistingHorde    += runningBg->GetPlayersCountByTeam(HORDE);
                            totalFreeAlliance     += runningBg->GetFreeSlotsForTeam(ALLIANCE);
                            totalFreeHorde        += runningBg->GetFreeSlotsForTeam(HORDE);

                            // Not locked if any BG still has a free slot, or has removable bots
                            // (but locked BGs never evict bots, so they don't count).
                            if (runningBg->GetFreeSlotsForTeam(ALLIANCE) > 0 ||
                                (!runningBg->IsLocked() && runningBg->GetBotPlayersCountByTeam(ALLIANCE) > minBotsPerTeam))
                                allianceLocked = false;
                            if (runningBg->GetFreeSlotsForTeam(HORDE) > 0 ||
                                (!runningBg->IsLocked() && runningBg->GetBotPlayersCountByTeam(HORDE) > minBotsPerTeam))
                                hordeLocked = false;
                        }

                        if (allianceLocked || hordeLocked)
                        {
                            uint32 const newFillTarget      = GetBattleBotFillTarget(bgTypeId, bg);
                            uint32 const newMaxCount        = GetBattleBotMaxAutoTeamCount(bgTypeId, bg);
                            uint32 const newQueueFillTarget = std::min<uint32>(newMaxCount, newFillTarget + 1);

                            // Players/bots not committed to any existing BG.
                            uint32 const newBgAllianceCount = queuedAllianceCount[bracketId] > totalExistingAlliance
                                ? queuedAllianceCount[bracketId] - totalExistingAlliance : 0;
                            uint32 const newBgHordeCount = queuedHordeCount[bracketId] > totalExistingHorde
                                ? queuedHordeCount[bracketId] - totalExistingHorde : 0;

                            // Add extra bots to compensate for existing BG free slots that
                            // will absorb newly queued bots before they reach the new BG.
                            uint32 const newAllianceTarget = std::min<uint32>(newMaxCount,
                                std::max<uint32>(newQueueFillTarget, newBgHordeCount) + totalFreeAlliance);
                            uint32 const newHordeTarget = std::min<uint32>(newMaxCount,
                                std::max<uint32>(newQueueFillTarget, newBgAllianceCount) + totalFreeHorde);

                            bool newBotsAdded = false;
                            for (uint32 i = newBgAllianceCount; i < newAllianceTarget; ++i)
                            {
                                uint32 const botLevel = urand(minLevel, maxLevel);
                                AddBattleBot(BattleGroundQueueTypeId(queueType), ALLIANCE, maxLevel > 50 ? maxLevel : botLevel, true);
                                ++queuedAllianceCount[bracketId];
                                newBotsAdded = true;
                            }
                            for (uint32 i = newBgHordeCount; i < newHordeTarget; ++i)
                            {
                                uint32 const botLevel = urand(minLevel, maxLevel);
                                AddBattleBot(BattleGroundQueueTypeId(queueType), HORDE, maxLevel > 50 ? maxLevel : botLevel, true);
                                ++queuedHordeCount[bracketId];
                                newBotsAdded = true;
                            }
                            if (newBotsAdded)
                                sBattleGroundMgr.ScheduleQueueUpdate(BattleGroundQueueTypeId(queueType), bgTypeId, BattleGroundBracketId(bracketId));
                        }
                    }
                }
            }
        }

        BalanceOverflowAVInstances();
    }

    if (m_confEnableRandomBots)
    {
        uint32 updatesCount = (m_elapsedTime - m_lastBotsRefresh) / m_confRandomBotsRefresh;
        for (uint32 i = 0; i < updatesCount; ++i)
        {
            AddOrRemoveBot();
            m_lastBotsRefresh += m_confRandomBotsRefresh;
        }
    }
}

/*
Toutes les X minutes, ajoute ou enleve un bot.
*/
bool PlayerBotMgr::AddOrRemoveBot()
{
    uint32 alea = urand(m_confMinRandomBots, m_confMaxRandomBots);
    /*
    10 --- --- --- --- --- --- --- --- --- --- 20 bots
                NumActuel
    [alea ici : remove    ][    ici, add    ]
    */
    if (alea > m_stats.onlineCount)
        return AddRandomBot();
    else
        return DeleteRandomBot();

}

bool PlayerBotMgr::AddBot(PlayerBotAI* ai)
{
    // Find a correct accountid ?
    std::shared_ptr<PlayerBotEntry> e = std::make_shared<PlayerBotEntry>();
    e->ai.reset(ai);
    e->accountId = GenBotAccountId();
    e->playerGUID = sObjectMgr.GeneratePlayerLowGuid();
    e->customBot = true;
    ai->botEntry = e.get();
    m_bots.insert({ e->playerGUID, e });
    return AddBot(e->playerGUID, false);
}

bool PlayerBotMgr::AddBot(uint32 playerGUID, bool chatBot, PlayerBotAI* pAI)
{
    uint32 accountId = 0;
    auto iter = m_bots.find(playerGUID);
    if (iter == m_bots.end())
        accountId = sObjectMgr.GetPlayerAccountIdByGUID(playerGUID);
    else
        accountId = iter->second->accountId;

    if (!accountId)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "Player account %u not found...", playerGUID);
        return false;
    }

    if (sWorld.FindSession(accountId))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[PlayerBotMgr] Account %u is already online!", accountId);
        return false;
    }

    std::shared_ptr<PlayerBotEntry> e;
    if (iter != m_bots.end())
    {
        e = iter->second;

        if (pAI) // new AI
        {
            e->ai.reset(pAI);
            e->customBot = true;
        }
    }
    else
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "[PlayerBotMgr] Adding temporary PlayerBot with GUID %u.", playerGUID);
        e = std::make_shared<PlayerBotEntry>();
        e->state        = PB_STATE_LOADING;
        e->loadingStartTime = sWorld.GetGameTime();
        e->playerGUID   = playerGUID;
        e->chance       = 10;
        e->accountId    = accountId;
        e->isChatBot    = chatBot;
        if (pAI)
        {
            e->ai.reset(pAI);
            e->customBot = true;
        }
        else
        {
            e->ai.reset(new PlayerBotAI(nullptr));
            e->customBot = false;
        }
        m_bots.insert({ playerGUID , e });
    }

    e->ai->botEntry = e.get();
    e->state = PB_STATE_LOADING;
    e->loadingStartTime = sWorld.GetGameTime();
    WorldSession* session = new WorldSession(accountId, nullptr, sAccountMgr.GetSecurity(accountId), 0, LOCALE_enUS);
    session->SetBot(e);
    sWorld.AddSession(session);
    m_stats.loadingCount++;
    if (chatBot)
        AddTempBot(accountId, 20000);

    return true;
}

bool PlayerBotMgr::AddRandomBot()
{
    uint32 alea = urand(0, m_totalChance);
    bool done = false;
    for (auto it = m_bots.begin(); it != m_bots.end() && !done; it++)
    {
        if (it->second->state != PB_STATE_OFFLINE)
            continue;
        if (it->second->customBot)
            continue;
        uint32 chance = it->second->chance;

        if (chance >= alea)
        {
            AddBot(it->first);
            done = true;
        }
        alea -= chance;
    }
    return done;
}

void PlayerBotMgr::AddTempBot(uint32 account, uint32 time)
{
    m_tempBots[account] = time;
}

void PlayerBotMgr::RefreshTempBot(uint32 account)
{
    if (m_tempBots.find(account) != m_tempBots.end())
    {
        uint32& delay = m_tempBots[account];
        if (delay < 1000)
            delay = 1000;
    }
}

bool PlayerBotMgr::DeleteBot(uint32 playerGUID)
{
    auto iter = m_bots.find(playerGUID);
    if (iter == m_bots.end())
        return false;

    return DeleteBot(iter);
}

bool PlayerBotMgr::DeleteBot(std::map<uint64, std::shared_ptr<PlayerBotEntry>>::iterator iter)
{
    if (iter->second->state == PB_STATE_LOADING)
        m_stats.loadingCount--;
    else if (iter->second->state == PB_STATE_ONLINE)
        m_stats.onlineCount--;

    OnBotLogout(iter->second.get());
    return true;
}

bool PlayerBotMgr::DeleteRandomBot()
{
    if (m_stats.onlineCount < 1)
        return false;

    // urand is inclusive on both ends. Picking [1, onlineCount] aligns with the
    // pre-increment match below (onlinePassed jumps to 1 on first eligible bot).
    // Original urand(0, onlineCount) had a 1/(N+1) chance of returning 0, in
    // which case the loop's `onlinePassed == 0` check could never match and the
    // function would silently fail to delete anything.
    uint32 const idDelete = urand(1, m_stats.onlineCount);
    uint32 onlinePassed = 0;
    for (auto iter = m_bots.begin(); iter != m_bots.end(); ++iter)
    {
        if (!iter->second->customBot && !iter->second->isChatBot && iter->second->state == PB_STATE_ONLINE)
        {
            onlinePassed++;
            if (onlinePassed == idDelete)
            {
                OnBotLogout(iter->second.get());
                m_stats.onlineCount--;
                return true;
            }
        }
    }
    return false;
}

uint8 SelectRandomRaceForClass(uint8 playerClass, Team playerTeam)
{
    std::vector<uint32> validRaces;
    for (uint32 raceId = 1; raceId < MAX_RACES; ++raceId)
    {
        if (playerTeam == ALLIANCE)
        {
            if (!((1 << (raceId - 1)) & RACEMASK_ALLIANCE))
                continue;
        }
        else if (playerTeam == HORDE)
        {
            if (!((1 << (raceId - 1)) & RACEMASK_HORDE))
                continue;
        }

        if (sObjectMgr.GetPlayerInfo(raceId, playerClass))
            validRaces.push_back(raceId);
    }

    if (validRaces.empty())
        return 0;

    return SelectRandomContainerElement(validRaces);
}

void PlayerBotMgr::AddBattleBot(BattleGroundQueueTypeId queueType, Team botTeam, uint32 botLevel, bool temporary)
{
    std::vector<uint32> availableClasses;

    if (queueType == BATTLEGROUND_QUEUE_WS)
    {
        // WSG class distribution. Hunter excluded (poor in indoor corridors). No tank specs.
        // Tier 1 (6x): Warrior, Mage
        // Tier 2 (4x): Rogue, Priest, Druid, Warlock, Paladin/Shaman
        availableClasses = {
            CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR,
            CLASS_MAGE,    CLASS_MAGE,    CLASS_MAGE,    CLASS_MAGE,    CLASS_MAGE,    CLASS_MAGE,
            CLASS_ROGUE,   CLASS_ROGUE,   CLASS_ROGUE,   CLASS_ROGUE,
            CLASS_PRIEST,  CLASS_PRIEST,  CLASS_PRIEST,  CLASS_PRIEST,
            CLASS_DRUID,   CLASS_DRUID,   CLASS_DRUID,   CLASS_DRUID,
            CLASS_WARLOCK, CLASS_WARLOCK, CLASS_WARLOCK, CLASS_WARLOCK,
        };
        if (botTeam == HORDE)
        {
            availableClasses.push_back(CLASS_SHAMAN);
            availableClasses.push_back(CLASS_SHAMAN);
            availableClasses.push_back(CLASS_SHAMAN);
            availableClasses.push_back(CLASS_SHAMAN);
        }
        else
        {
            availableClasses.push_back(CLASS_PALADIN);
            availableClasses.push_back(CLASS_PALADIN);
            availableClasses.push_back(CLASS_PALADIN);
            availableClasses.push_back(CLASS_PALADIN);
        }
    }
    else if (queueType == BATTLEGROUND_QUEUE_AB)
    {
        // AB class distribution. Hunter excluded (poor in node melee). No tank specs.
        // Tier 1 (6x): Warrior, Mage, Rogue, Warlock, Paladin/Shaman
        // Tier 2 (4x): Priest, Druid
        availableClasses = {
            CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR,
            CLASS_MAGE,    CLASS_MAGE,    CLASS_MAGE,    CLASS_MAGE,    CLASS_MAGE,    CLASS_MAGE,
            CLASS_ROGUE,   CLASS_ROGUE,   CLASS_ROGUE,   CLASS_ROGUE,   CLASS_ROGUE,   CLASS_ROGUE,
            CLASS_PRIEST,  CLASS_PRIEST,  CLASS_PRIEST,  CLASS_PRIEST,
            CLASS_DRUID,   CLASS_DRUID,   CLASS_DRUID,   CLASS_DRUID,
            CLASS_WARLOCK, CLASS_WARLOCK, CLASS_WARLOCK, CLASS_WARLOCK, CLASS_WARLOCK, CLASS_WARLOCK,
        };
        if (botTeam == HORDE)
        {
            availableClasses.push_back(CLASS_SHAMAN);
            availableClasses.push_back(CLASS_SHAMAN);
            availableClasses.push_back(CLASS_SHAMAN);
            availableClasses.push_back(CLASS_SHAMAN);
            availableClasses.push_back(CLASS_SHAMAN);
            availableClasses.push_back(CLASS_SHAMAN);
        }
        else
        {
            availableClasses.push_back(CLASS_PALADIN);
            availableClasses.push_back(CLASS_PALADIN);
            availableClasses.push_back(CLASS_PALADIN);
            availableClasses.push_back(CLASS_PALADIN);
            availableClasses.push_back(CLASS_PALADIN);
            availableClasses.push_back(CLASS_PALADIN);
        }
    }
    else
    {
        // AV and other BGs: general-purpose distribution with Hunter.
        // Tier 1 (6x): Warrior, Mage, Hunter
        // Tier 2 (4x): Priest, Rogue, Paladin/Shaman
        // Tier 3 (3x): Druid, Warlock
        availableClasses = {
            CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR,
            CLASS_MAGE,    CLASS_MAGE,    CLASS_MAGE,    CLASS_MAGE,    CLASS_MAGE,    CLASS_MAGE,
            CLASS_HUNTER,  CLASS_HUNTER,  CLASS_HUNTER,  CLASS_HUNTER,  CLASS_HUNTER,  CLASS_HUNTER,
            CLASS_PRIEST,  CLASS_PRIEST,  CLASS_PRIEST,  CLASS_PRIEST,
            CLASS_ROGUE,   CLASS_ROGUE,   CLASS_ROGUE,   CLASS_ROGUE,
            CLASS_DRUID,   CLASS_DRUID,   CLASS_DRUID,
            CLASS_WARLOCK, CLASS_WARLOCK, CLASS_WARLOCK,
        };
        if (botTeam == HORDE)
        {
            availableClasses.push_back(CLASS_SHAMAN);
            availableClasses.push_back(CLASS_SHAMAN);
            availableClasses.push_back(CLASS_SHAMAN);
            availableClasses.push_back(CLASS_SHAMAN);
        }
        else
        {
            availableClasses.push_back(CLASS_PALADIN);
            availableClasses.push_back(CLASS_PALADIN);
            availableClasses.push_back(CLASS_PALADIN);
            availableClasses.push_back(CLASS_PALADIN);
        }
    }

    uint8 botClass = SelectRandomContainerElement(availableClasses);
    uint8 botRace = SelectRandomRaceForClass(botClass, botTeam);
    if (!botRace)
        return;

    // Spawn bot on GM Island
    uint32 const instanceId = sMapMgr.GetContinentInstanceId(1, 16224.356f, 16284.763f);
    BattleBotAI* ai = new BattleBotAI(botRace, botClass, botLevel, 1, instanceId, 16224.356f, 16284.763f, 13.175f, 4.56f, queueType, temporary);
    AddBot(ai);

    // Bot queue additions are operational noise, not player/GM-facing news — log only,
    // no in-game broadcast (previously sent via SendWorldTextToBGAndQueue to GMs).
    if (botTeam == ALLIANCE)
    {
        if (temporary)
            sLog.Out(LOG_BG, LOG_LVL_BASIC, "[PlayerBotMgr] Adding temporary level %u alliance battlebot to bg queue %u.", botLevel, queueType);
        else
            sLog.Out(LOG_BG, LOG_LVL_BASIC, "[PlayerBotMgr] Adding level %u alliance battlebot to bg queue %u.", botLevel, queueType);
    }
    else
    {
        if (temporary)
            sLog.Out(LOG_BG, LOG_LVL_BASIC, "[PlayerBotMgr] Adding temporary level %u horde battlebot to bg queue %u.", botLevel, queueType);
        else
            sLog.Out(LOG_BG, LOG_LVL_BASIC, "[PlayerBotMgr] Adding level %u horde battlebot to bg queue %u.", botLevel, queueType);
    }
}

bool PlayerBotMgr::AddBattleRoyaleBot(uint32 brInstanceId)
{
    // BR: DPS only. Healer specs are prevented by pre-setting m_role before LearnPremadeSpecForClass().
    // Tier 1 (65%): Warrior 8 (20%), Rogue 8 (20%), Mage 5 (12.5%), Warlock 5 (12.5%)
    // Tier 2 (15%): Priest (shadow) 6 (15%)
    // Tier 3 (20%): Paladin (retri) 3, Druid (feral) 3, Shaman (elem) 2
    std::vector<uint32> availableClasses = {
        CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR,
        CLASS_WARRIOR, CLASS_WARRIOR, CLASS_WARRIOR,
        CLASS_MAGE,    CLASS_MAGE,    CLASS_MAGE,    CLASS_MAGE,    CLASS_MAGE,
        CLASS_WARLOCK, CLASS_WARLOCK, CLASS_WARLOCK, CLASS_WARLOCK, CLASS_WARLOCK,
        CLASS_ROGUE,   CLASS_ROGUE,   CLASS_ROGUE,   CLASS_ROGUE,   CLASS_ROGUE,
        CLASS_ROGUE,   CLASS_ROGUE,   CLASS_ROGUE,
        CLASS_PRIEST,  CLASS_PRIEST,  CLASS_PRIEST,  CLASS_PRIEST,  CLASS_PRIEST,  CLASS_PRIEST,
        CLASS_PALADIN, CLASS_PALADIN, CLASS_PALADIN,
        CLASS_DRUID,   CLASS_DRUID,   CLASS_DRUID,
        CLASS_SHAMAN,  CLASS_SHAMAN,
    };

    uint8 botClass = SelectRandomContainerElement(availableClasses);
    // Shaman is Horde-only; Paladin is Alliance-only in vanilla.
    Team botTeam = (botClass == CLASS_SHAMAN)  ? HORDE
                 : (botClass == CLASS_PALADIN) ? ALLIANCE
                 : (urand(0, 1) ? ALLIANCE : HORDE);
    uint8 botRace = SelectRandomRaceForClass(botClass, botTeam);
    if (!botRace)
        return false;

    uint32 const instanceId = sMapMgr.GetContinentInstanceId(1, 16224.356f, 16284.763f);
    BattleBotAI* ai = new BattleBotAI(botRace, botClass, 60, 1, instanceId,
                                       16224.356f, 16284.763f, 13.175f, 4.56f, 0, true);
    ai->m_isBattleRoyaleBot = true;
    ai->m_brInstanceId = brInstanceId;
    AddBot(ai);
    return true;
}

void PlayerBotMgr::DeleteBattleBots()
{
    for (auto const& itr : m_bots)
    {
        if (dynamic_cast<BattleBotAI*>(itr.second->ai.get()))
            itr.second->requestRemoval = true;
    }
}

// Helper: pick a level for lock-in fill bots that matches this BG's bracket.
static uint32 PickLockFillBotLevel(BattleGround const* bg)
{
    if (!bg)
        return PLAYER_MAX_LEVEL;

    uint32 const minLevel = bg->GetMinLevel() + 10 * bg->GetBracketId();
    uint32 const maxLevel = std::min<uint32>(minLevel + 9, PLAYER_MAX_LEVEL);
    if (maxLevel > 50)
        return maxLevel;       // 60-bracket: always lock to max so gear matches
    return urand(minLevel, maxLevel);
}

void PlayerBotMgr::RequestFillLockedBattleGround(BattleGround* bg)
{
    if (!bg)
        return;

    BattleGroundTypeId const bgTypeId = bg->GetTypeID();
    BattleGroundQueueTypeId const queueType = BattleGroundMgr::BgQueueTypeId(bgTypeId);
    uint32 const maxPerTeam = bg->GetMaxPlayersPerTeam();
    if (maxPerTeam <= 1)
        return;

    uint32 const allianceCount = bg->GetPlayersCountByTeam(ALLIANCE);
    uint32 const hordeCount    = bg->GetPlayersCountByTeam(HORDE);

    uint32 const allianceMissing = (allianceCount < maxPerTeam) ? (maxPerTeam - allianceCount) : 0;
    uint32 const hordeMissing    = (hordeCount    < maxPerTeam) ? (maxPerTeam - hordeCount)    : 0;

    if (!allianceMissing && !hordeMissing)
        return;

    sLog.Out(LOG_BG, LOG_LVL_BASIC,
        "[PlayerBotMgr] Lock-fill bg type %u instance %u: alliance +%u, horde +%u (to max %u/team).",
        bgTypeId, bg->GetInstanceID(), allianceMissing, hordeMissing, maxPerTeam);

    for (uint32 i = 0; i < allianceMissing; ++i)
        AddBattleBot(queueType, ALLIANCE, PickLockFillBotLevel(bg), true);
    for (uint32 i = 0; i < hordeMissing; ++i)
        AddBattleBot(queueType, HORDE, PickLockFillBotLevel(bg), true);

    if (allianceMissing || hordeMissing)
    {
        bg->AddToBGFreeSlotQueue();
        sBattleGroundMgr.ScheduleQueueUpdate(queueType, bgTypeId, bg->GetBracketId());
    }
}

void PlayerBotMgr::RequestReplaceWithBot(BattleGround* bg, Team team)
{
    if (!bg || team == TEAM_NONE)
        return;

    BattleGroundTypeId const bgTypeId = bg->GetTypeID();
    BattleGroundQueueTypeId const queueType = BattleGroundMgr::BgQueueTypeId(bgTypeId);

    sLog.Out(LOG_BG, LOG_LVL_BASIC,
        "[PlayerBotMgr] Lock-replace bg type %u instance %u team %u — spawning replacement bot.",
        bgTypeId, bg->GetInstanceID(), team);

    AddBattleBot(queueType, team, PickLockFillBotLevel(bg), true);
    bg->AddToBGFreeSlotQueue();
    sBattleGroundMgr.ScheduleQueueUpdate(queueType, bgTypeId, bg->GetBracketId());
}

// AV overflow instance (Alterac.MaxConcurrentInstances): an AV instance left unlocked
// because an older one was still running (see BattleGround::Update()'s lock-timer block)
// stays open on both sides for real players. But if one side fills up on its own — all
// real players, or just population drift — the match stays lopsided forever with no
// mechanism to ever complete it. Once a side is genuinely full and the other still has
// room, top the short side up to match so the match finishes like a normal one instead
// of running half-empty indefinitely.
void PlayerBotMgr::BalanceOverflowAVInstances()
{
    if (!sWorld.getConfig(CONFIG_UINT32_AV_MAX_CONCURRENT_INSTANCES))
        return;

    for (auto itr = sBattleGroundMgr.GetBattleGroundsBegin(BATTLEGROUND_AV); itr != sBattleGroundMgr.GetBattleGroundsEnd(BATTLEGROUND_AV); ++itr)
    {
        BattleGround* bg = itr->second;
        if (!bg || bg->GetClientInstanceID() == 0)
            continue;

        if (bg->GetStatus() != STATUS_IN_PROGRESS || bg->IsLocked())
            continue;

        uint32 const allianceFree = bg->GetFreeSlotsForTeam(ALLIANCE);
        uint32 const hordeFree = bg->GetFreeSlotsForTeam(HORDE);

        // Both sides caught up (the top-up from an earlier tick has now been seated) —
        // this instance is complete. Lock it so PlayerBotMgr::Update()'s queue-fill
        // bookkeeping — which otherwise treats unlocked AV as "keep 1 slot open per
        // side" — stops fighting the fully-seated side. Without this, an unlocked-but-
        // actually-full instance gets its just-added bot trimmed straight back out
        // every ~10s by that logic (queuedXCount is summed across all concurrent AV
        // instances, so it reads as over target even though this one instance is at
        // capacity), producing an endless add/remove loop that never finishes the match.
        if (allianceFree == 0 && hordeFree == 0)
        {
            bg->LockForNewPlayers();
            continue;
        }

        // Reserve 1 slot on the topped-up side for a real player instead of handing the
        // whole gap to bots in one shot - otherwise this overflow instance (meant to stay
        // open for queued real players once the primary AV is full) gets bot-flooded to
        // full capacity and locked within minutes of just one side filling up naturally,
        // leaving nowhere for later queuers to go until a running match ends.
        Team teamToFill = TEAM_NONE;
        uint32 slotsToFill = 0;
        if (allianceFree == 0 && hordeFree > 1)
        {
            teamToFill = HORDE;
            slotsToFill = hordeFree - 1;
        }
        else if (hordeFree == 0 && allianceFree > 1)
        {
            teamToFill = ALLIANCE;
            slotsToFill = allianceFree - 1;
        }

        if (teamToFill == TEAM_NONE)
            continue;

        BattleGroundQueueTypeId const queueType = BattleGroundMgr::BgQueueTypeId(bg->GetTypeID());
        for (uint32 i = 0; i < slotsToFill; ++i)
            AddBattleBot(queueType, teamToFill, PickLockFillBotLevel(bg), true);

        bg->AddToBGFreeSlotQueue();
        sBattleGroundMgr.ScheduleQueueUpdate(queueType, BATTLEGROUND_AV, bg->GetBracketId());

        sLog.Out(LOG_BG, LOG_LVL_BASIC,
            "[PlayerBotMgr] AV overflow instance %u: %s side full, topping up %s with %u bots to complete the match.",
            bg->GetInstanceID(), teamToFill == HORDE ? "alliance" : "horde", teamToFill == HORDE ? "horde" : "alliance", slotsToFill);
    }
}

void PlayerBotMgr::SwitchAutoJoinBattleBots(bool payload, uint32 bgTypeId)
{
    char const* bgName = nullptr;
    switch (bgTypeId)
    {
        case 1:
            m_confBattleBotAutoJoin_1 = payload;
            sObjectMgr.SetSavedVariable(VAR_BATTLEBOT_AUTOJOIN_AV,  payload ? 1 : 0, true);
            bgName = "奥特兰克山谷";
            break;
        case 2:
            m_confBattleBotAutoJoin_2 = payload;
            sObjectMgr.SetSavedVariable(VAR_BATTLEBOT_AUTOJOIN_WSG, payload ? 1 : 0, true);
            bgName = "战歌峡谷";
            break;
        case 3:
            m_confBattleBotAutoJoin_3 = payload;
            sObjectMgr.SetSavedVariable(VAR_BATTLEBOT_AUTOJOIN_AB,  payload ? 1 : 0, true);
            bgName = "阿拉希盆地";
            break;
        default:
            return;
    }

    // GM 开关战场机器人自动填充，等同于该战场对玩家来说"开启/关闭"，发全服公告
    char msg[128];
    if (payload)
        snprintf(msg, sizeof(msg), "[战场] 号角已鸣，%s战鼓再擂，速往战场指挥官处，一展身手！", bgName);
    else
        snprintf(msg, sizeof(msg), "[战场] 烽烟渐息，%s暂偃旗鼓，且待来日再战。", bgName);
    sWorld.SendGlobalText(msg, nullptr);
    sWebChatMgr.WriteBroadcast(msg);
    sWebChatMgr.NotifyWorldBroadcastToJianJia(msg);
}

bool PlayerBotMgr::ForceAccountConnection(WorldSession* sess)
{
    if (sess->GetBot())
        return sess->GetBot()->state != PB_STATE_OFFLINE;

    // Temporary bots.
    return m_tempBots.find(sess->GetAccountId()) != m_tempBots.end();
}

bool PlayerBotMgr::IsPermanentBot(uint32 playerGUID)
{
    auto iter = m_bots.find(playerGUID);
    return iter != m_bots.end();
}

bool PlayerBotMgr::IsChatBot(uint32 playerGuid)
{
    auto iter = m_bots.find(playerGuid);
    return iter != m_bots.end() && iter->second->isChatBot;
}

void PlayerBotMgr::AddAllBots()
{
    for (auto it = m_bots.begin(); it != m_bots.end(); it++)
    {
        if (!it->second->isChatBot && it->second->state == PB_STATE_OFFLINE)
            AddBot(it->first);
    }
}

bool ChatHandler::HandleBotReloadCommand(char * args)
{
    sPlayerBotMgr.Load();
    SendSysMessage("PlayerBot system reloaded");
    return true;
}

bool ChatHandler::HandleBotAddRandomCommand(char * args)
{
    uint32 count = 1;
    char* sCount = strtok((char*)args, " ");
    if (sCount)
        count = uint32(atoi(sCount));
    for (uint32 i = 0; i < count; ++i)
        sPlayerBotMgr.AddRandomBot();
    PSendSysMessage("%u bots added", count);
    return true;
}

bool ChatHandler::HandleBotStopCommand(char * args)
{
    sPlayerBotMgr.DeleteAll();
    SendSysMessage("All the bots have been unloaded.");
    return true;
}

bool ChatHandler::HandleBotAddAllCommand(char * args)
{
    sPlayerBotMgr.AddAllBots();
    SendSysMessage("All bots have been loaded.");
    return true;
}

bool ChatHandler::HandleBotAddCommand(char* args)
{
    uint32 guid = 0;
    char *charname = nullptr;
    if (*args)
    {
        charname = strtok((char*)args, " ");
        if (charname && strcmp(charname, "") == 0)
            return false;

        guid = sObjectMgr.GetPlayerGuidByName(charname).GetCounter();
        if (!guid)
        {
            PSendSysMessage("Player not found : '%s'", charname);
            SetSentErrorMessage(true);
            return false;
        }
    }
    if (!guid || !sPlayerBotMgr.AddBot(guid))
    {
        SendSysMessage("[PlayerBotMgr] Unable to load bot.");
        return true;
    }
    PSendSysMessage("[PlayerBotMgr] Bot added : '%s', GUID=%u", charname ? charname : "NULL", guid);
    return true;
}

bool ChatHandler::HandleBotDeleteCommand(char * args)
{
    char *charname = strtok((char*)args, " ");

    if (!charname || strcmp(charname, "") == 0)
    {
        SendSysMessage("Syntax : $playerName");
        SetSentErrorMessage(true);
        return false;
    }
    uint32 lowGuid = sObjectMgr.GetPlayerGuidByName(charname).GetCounter();
    if (!lowGuid)
    {
        PSendSysMessage("Unable to find player: '%s'", charname);
        SetSentErrorMessage(true);
        return false;
    }
    if (sPlayerBotMgr.DeleteBot(lowGuid))
    {
        PSendSysMessage("Bot %s (GUID:%u) disconnected.", charname, lowGuid);
        return true;
    }
    else
    {
        PSendSysMessage("Bot %s (GUID:%u) : unable to disconnect.", charname, lowGuid);
        SetSentErrorMessage(true);
        return false;
    }
}

bool ChatHandler::HandleBotInfoCommand(char * args)
{
    uint32 online = sWorld.GetActiveSessionCount();

    PlayerBotStats stats = sPlayerBotMgr.GetStats();
    SendSysMessage("-- PlayerBot stats --");
    PSendSysMessage("Min:%u Max:%u Total:%u", stats.confMinOnline, stats.confMaxOnline, stats.totalBots);
    PSendSysMessage("Loading : %u, Online : %u, Chat : %u", stats.loadingCount, stats.onlineCount, stats.onlineChat);
    PSendSysMessage("%up + %ub = %u",
                    (online - stats.onlineCount), stats.onlineCount, online);
    return true;
}

bool ChatHandler::HandleBotStartCommand(char * args)
{
    sPlayerBotMgr.Start();
    return true;
}

bool ChatHandler::PartyBotAddRequirementCheck(Player const* pPlayer, Player const* pTarget)
{
    if (pPlayer->IsTaxiFlying())
    {
        SendSysMessage("Cannot add bots while flying.");
        return false;
    }

    // Spawning bots inside BG will cause server crash on BG end.
    if (pPlayer->InBattleGround())
    {
        SendSysMessage("Cannot add bots inside battlegrounds.");
        return false;
    }

    if (pPlayer->GetGroup() && (pPlayer->GetGroup()->IsFull() || sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_MAX_BOTS) &&
        (pPlayer->GetGroup()->GetMembersCount() - 1 >= sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_MAX_BOTS))))
    {
        SendSysMessage("Cannot add more bots. Group is full.");
        return false;
    }

    if (Map const* pMap = pPlayer->GetMap())
    {
        if (pMap->IsDungeon() &&
            pMap->GetPlayers().getSize() >= pMap->GetMapEntry()->maxPlayers)
        {
            SendSysMessage("Cannot add more bots. Instance is full.");
            return false;
        }
    }

    if (pTarget && pTarget->GetTeam() != pPlayer->GetTeam())
    {
        SendSysMessage("Cannot clone enemy faction characters.");
        return false;
    }

    // Restrictions when the command is made public to avoid abuse.
    if (GetSession()->GetSecurity() <= SEC_PLAYER && !sWorld.getConfig(CONFIG_BOOL_PARTY_BOT_SKIP_CHECKS))
    {
        if (pPlayer->IsDead())
        {
            SendSysMessage("Cannot add bots while dead.");
            return false;
        }

        if (pPlayer->IsInCombat())
        {
            SendSysMessage("Cannot add bots while in combat.");
            return false;
        }

        if (pPlayer->GetMap()->IsDungeon())
        {
            SendSysMessage("Cannot add bots while inside instances.");
            return false;
        }

        // Clone command.
        if (pTarget)
        {
            if (pTarget->IsDead())
            {
                SendSysMessage("Cannot clone dead characters.");
                return false;
            }

            if (pTarget->IsInCombat())
            {
                SendSysMessage("Cannot clone characters that are in combat.");
                return false;
            }

            if (pTarget->GetLevel() > pPlayer->GetLevel() + 10)
            {
                SendSysMessage("Cannot clone higher level characters.");
                return false;
            }
        }
    }

    return true;
}

bool ChatHandler::HandlePartyBotAddCommand(char* args)
{
    Player* pPlayer = m_session->GetPlayer();
    if (!pPlayer)
        return false;

    if (!PartyBotAddRequirementCheck(pPlayer, nullptr))
    {
        SetSentErrorMessage(true);
        return false;
    }

    if (!args)
    {
        SendSysMessage("Incorrect syntax. Expected role or class.");
        SetSentErrorMessage(true);
        return false;
    }

    uint8 botClass = 0;
    uint32 botLevel = pPlayer->GetLevel();
    CombatBotRoles botRole = ROLE_INVALID;

    if (char* arg1 = ExtractArg(&args))
    {
        std::string option = arg1;
        if (option == "warrior")
            botClass = CLASS_WARRIOR;
        else if (option == "paladin" && pPlayer->GetTeam() == ALLIANCE)
            botClass = CLASS_PALADIN;
        else if (option == "hunter")
            botClass = CLASS_HUNTER;
        else if (option == "rogue")
            botClass = CLASS_ROGUE;
        else if (option == "priest")
            botClass = CLASS_PRIEST;
        else if (option == "shaman" && pPlayer->GetTeam() == HORDE)
            botClass = CLASS_SHAMAN;
        else if (option == "mage")
            botClass = CLASS_MAGE;
        else if (option == "warlock")
            botClass = CLASS_WARLOCK;
        else if (option == "druid")
            botClass = CLASS_DRUID;
        else if (option == "dps")
        {
            botClass = PickRandomValue(CLASS_WARRIOR, CLASS_HUNTER, CLASS_ROGUE, CLASS_MAGE, CLASS_WARLOCK);
            botRole = CombatBotBaseAI::IsMeleeDamageClass(botClass) ? ROLE_MELEE_DPS : ROLE_RANGE_DPS;
        }
        else if (option == "healer")
        {
            std::vector<uint32> dpsClasses = { CLASS_PRIEST, CLASS_DRUID };
            if (pPlayer->GetTeam() == HORDE)
                dpsClasses.push_back(CLASS_SHAMAN);
            else
                dpsClasses.push_back(CLASS_PALADIN);
            botClass = SelectRandomContainerElement(dpsClasses);
            botRole = ROLE_HEALER;
        }
        else if (option == "tank")
        {
            botClass = CLASS_WARRIOR;
            botRole = ROLE_TANK;
        }

        // Prevent setting a custom level for bots unless the account is a GM or skipping checks is enabled.
        if (GetSession()->GetSecurity() > SEC_PLAYER || sWorld.getConfig(CONFIG_BOOL_PARTY_BOT_SKIP_CHECKS))
            ExtractUInt32(&args, botLevel);
    }

    if (!botClass)
    {
        SendSysMessage("Incorrect syntax. Expected role or class.");
        SetSentErrorMessage(true);
        return false;
    }

    uint8 botRace = SelectRandomRaceForClass(botClass, pPlayer->GetTeam());
    if (!botRace)
    {
        SendSysMessage("Unable to select race for bot.");
        SetSentErrorMessage(true);
        return false;
    }

    float x, y, z;
    pPlayer->GetNearPoint(pPlayer, x, y, z, 0, 5.0f, frand(0.0f, 6.0f));

    PartyBotAI* ai = new PartyBotAI(pPlayer, nullptr, botRole, botRace, botClass, botLevel, pPlayer->GetMapId(), pPlayer->GetMap()->GetInstanceId(), x, y, z, pPlayer->GetOrientation());
    if (sPlayerBotMgr.AddBot(ai))
        SendSysMessage("New party bot added.");
    else
    {
        delete ai;
        SendSysMessage("Error spawning bot.");
        SetSentErrorMessage(true);
        return false;
    }

    return true;
}

bool ChatHandler::HandlePartyBotCloneCommand(char* args)
{
    Player* pPlayer = m_session->GetPlayer();
    if (!pPlayer)
        return false;

    Player* pTarget = GetSelectedPlayer();
    if (!pTarget)
    {
        SendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }

    if (!PartyBotAddRequirementCheck(pPlayer, pTarget))
    {
        SetSentErrorMessage(true);
        return false;
    }

    uint8 botRace = pTarget->GetRace();
    uint8 botClass = pTarget->GetClass();

    float x, y, z;
    pPlayer->GetNearPoint(pPlayer, x, y, z, 0, 5.0f, frand(0.0f, 6.0f));

    PartyBotAI* ai = new PartyBotAI(pPlayer, pTarget, ROLE_INVALID, botRace, botClass, pPlayer->GetLevel(), pPlayer->GetMapId(), pPlayer->GetMap()->GetInstanceId(), x, y, z, pPlayer->GetOrientation());
    if (sPlayerBotMgr.AddBot(ai))
        SendSysMessage("New party bot added.");
    else
    {
        delete ai;
        SendSysMessage("Error spawning bot.");
        SetSentErrorMessage(true);
        return false;
    }

    return true;
}

bool ChatHandler::HandlePartyBotLoadCommand(char* args)
{
    Player* pPlayer = m_session->GetPlayer();
    if (!pPlayer)
        return false;

    std::string name = ExtractPlayerNameFromLink(&args);
    if (name.empty())
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(name).GetCounter();
    if (!guid)
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    if (sObjectAccessor.FindPlayerNotInWorld(guid))
    {
        SendSysMessage("Player is already online!");
        SetSentErrorMessage(true);
        return false;
    }

    float x, y, z;
    pPlayer->GetNearPoint(pPlayer, x, y, z, 0, 5.0f, frand(0.0f, 6.0f));

    PartyBotAI* pAI = new PartyBotAI(pPlayer, pPlayer->GetMapId(), pPlayer->GetMap()->GetInstanceId(), x, y, z, pPlayer->GetOrientation());

    if (!sPlayerBotMgr.AddBot(guid, false, pAI))
    {
        delete pAI;
        SendSysMessage("Error spawning bot.");
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage("Loading %s as party bot.", name.c_str());
    return true;
}

bool ChatHandler::HandlePartyBotSetRoleCommand(char* args)
{
    if (!args)
        return false;

    Player* pTarget = GetSelectedPlayer();
    if (!pTarget)
    {
        SendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }

    CombatBotRoles role = ROLE_INVALID;
    std::string roleStr = args;

    if (roleStr == "tank")
        role = ROLE_TANK;
    else if (roleStr == "dps")
        role = CombatBotBaseAI::IsMeleeDamageClass(pTarget->GetClass()) ? ROLE_MELEE_DPS : ROLE_RANGE_DPS;
    else if (roleStr == "meleedps")
        role = ROLE_MELEE_DPS;
    else if (roleStr == "rangedps")
        role = ROLE_RANGE_DPS;
    else if (roleStr == "healer")
        role = ROLE_HEALER;

    if (role == ROLE_INVALID)
        return false;

    if (pTarget->AI())
    {
        if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pTarget->AI()))
        {
            pAI->m_role = role;
            pAI->ResetSpellData();
            pAI->PopulateSpellData();
            PSendSysMessage("%s is now a %s.", pTarget->GetName(), roleStr.c_str());
            return true;
        }
    }

    SendSysMessage("Target is not a party bot.");
    SetSentErrorMessage(true);
    return false;
}

bool ChatHandler::HandlePartyBotAttackStartCommand(char* args)
{
    Player* pPlayer = GetSession()->GetPlayer();
    Unit* pTarget = GetSelectedUnit();
    if (!pTarget || (pTarget == pPlayer))
    {
        SendSysMessage(LANG_SELECT_CHAR_OR_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == pPlayer)
                continue;

            if (pMember->AI())
            {
                if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI()))
                {
                    if (pMember->IsValidAttackTarget(pTarget))
                        pAI->AttackStart(pTarget);
                }
            }
        }
    }

    PSendSysMessage("All party bots are now attacking %s.", pTarget->GetName());
    return true;
}

void StopPartyBotAttackHelper(PartyBotAI* pAI, Player* pBot)
{
    pBot->AttackStop(true);
    pBot->InterruptNonMeleeSpells(false);
    if (!pBot->IsStopped())
        pBot->StopMoving();
    if (pBot->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
        pBot->GetMotionMaster()->Clear();
    if (pAI->m_updateTimer.GetExpiry() < 3000)
        pAI->m_updateTimer.Reset(3000);
}

bool ChatHandler::HandlePartyBotAttackStopCommand(char* args)
{
    Player* pPlayer = GetSession()->GetPlayer();
    Unit* pTarget = GetSelectedUnit();
    if (!pTarget || (pTarget == pPlayer))
    {
        SendSysMessage(LANG_SELECT_CHAR_OR_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == pPlayer)
                continue;

            if (pMember->AI())
            {
                if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI()))
                {
                    if (pMember->GetVictim() == pTarget)
                        StopPartyBotAttackHelper(pAI, pMember);
                }
            }
        }
    }

    PSendSysMessage("All party bots have stopped attacking %s.", pTarget->GetName());
    return true;
}

bool ChatHandler::HandlePartyBotAoECommand(char* args)
{
    Player* pPlayer = GetSession()->GetPlayer();
    Unit* pTarget = GetSelectedUnit();
    if (!pTarget || !pPlayer->IsValidAttackTarget(pTarget, true))
    {
        SendSysMessage(LANG_SELECT_CHAR_OR_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == pPlayer)
                continue;

            if (pMember->AI())
            {
                if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI()))
                {
                    for (auto const& pSpell : pAI->m_spells.raw.spells)
                    {
                        if (pSpell && pSpell->IsAreaOfEffectSpell() &&
                           !pSpell->IsPositiveSpell() &&
                            pSpell->IsTargetInRange(pMember, pTarget))
                        {
                            if (pMember->GetCurrentSpell(CURRENT_GENERIC_SPELL) &&
                               !pMember->GetCurrentSpell(CURRENT_GENERIC_SPELL)->m_spellInfo->IsAreaOfEffectSpell())
                                pMember->InterruptSpell(CURRENT_GENERIC_SPELL);

                            if (pMember->CastSpell(pTarget, pSpell, false) == SPELL_CAST_OK)
                                break;
                        }
                    }
                }
            }
        }
    }

    PSendSysMessage("All party bots are casting AoE spells at %s.", pTarget->GetName());
    return true;
}

bool ChatHandler::HandlePartyBotStartCastingCommand(char * args)
{
    return HandlePartyBotToggleCastingCommand(true);
}

bool ChatHandler::HandlePartyBotStopCastingCommand(char * args)
{
    return HandlePartyBotToggleCastingCommand(false);
}

bool ChatHandler::HandlePartyBotToggleCastingCommand(bool allowCasting)
{
    Player* pPlayer = GetSession()->GetPlayer();
    Player* pTarget = GetSelectedPlayer();

    if (pTarget && (pTarget != pPlayer))
    {
        if (pTarget->AI())
        {
            if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pTarget->AI()))
            {
                if (allowCasting)
                {
                    pAI->m_preventCasting = false;
                    PSendSysMessage("%s will be allowed to cast spells.", pTarget->GetName());
                }
                else
                {
                    pAI->m_preventCasting = true;
                    pTarget->InterruptNonMeleeSpells(false);
                    PSendSysMessage("%s will no longer cast spells.", pTarget->GetName());
                }
                return true;
            }
        }
        SendSysMessage("Target is not a party bot.");
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == pPlayer)
                continue;

            if (pMember->AI())
            {
                if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI()))
                {
                    if (allowCasting)
                    {
                        pAI->m_preventCasting = false;
                    }
                    else
                    {
                        pAI->m_preventCasting = true;
                        pTarget->InterruptNonMeleeSpells(false);
                    }
                }
            }
        }
    }

    if (allowCasting)
        SendSysMessage("All bots are now allowed to cast spells again.");
    else
        SendSysMessage("All bots are now forbidden from casting spells.");

    return true;
}

static std::map<std::string, RaidTargetIcon> raidTargetIcons =
{
    { "star",     RAID_TARGET_ICON_STAR     },
    { "circle",   RAID_TARGET_ICON_CIRCLE   },
    { "diamond",  RAID_TARGET_ICON_DIAMOND  },
    { "triangle", RAID_TARGET_ICON_TRIANGLE },
    { "moon",     RAID_TARGET_ICON_MOON     },
    { "square",   RAID_TARGET_ICON_SQUARE   },
    { "cross",    RAID_TARGET_ICON_CROSS    },
    { "skull",    RAID_TARGET_ICON_SKULL    },
};

bool ChatHandler::HandlePartyBotControlMarkCommand(char* args)
{
    std::string mark = args;
    auto itrMark = raidTargetIcons.find(mark);
    if (itrMark == raidTargetIcons.end())
    {
        SendSysMessage("Unknown target mark. Valid names are: star, circle, diamond, triangle, moon, square, cross, skull");
        SetSentErrorMessage(true);
        return false;
    }

    Player* pPlayer = GetSession()->GetPlayer();
    Player* pTarget = GetSelectedPlayer();

    if (pTarget && (pTarget != pPlayer))
    {
        if (pTarget->AI())
        {
            if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pTarget->AI()))
            {
                PSendSysMessage("%s will crowd control %s.", pTarget->GetName(), args);
                pAI->m_marksToCC.push_back(itrMark->second);
                return true;
            }
        }
        SendSysMessage("Target is not a party bot.");
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == pPlayer)
                continue;

            if (pMember->AI())
            {
                if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI()))
                {
                    pAI->m_marksToCC.push_back(itrMark->second);
                }
            }
        }
    }

    PSendSysMessage("All party bots will crowd control %s.", args);
    return true;
}

bool ChatHandler::HandlePartyBotFocusMarkCommand(char* args)
{
    std::string mark = args;
    auto itrMark = raidTargetIcons.find(mark);
    if (itrMark == raidTargetIcons.end())
    {
        SendSysMessage("Unknown target mark. Valid names are: star, circle, diamond, triangle, moon, square, cross, skull");
        SetSentErrorMessage(true);
        return false;
    }

    Player* pPlayer = GetSession()->GetPlayer();
    Player* pTarget = GetSelectedPlayer();

    if (pTarget && (pTarget != pPlayer))
    {
        if (pTarget->AI())
        {
            if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pTarget->AI()))
            {
                if (std::find(pAI->m_marksToFocus.begin(), pAI->m_marksToFocus.end(), itrMark->second) != pAI->m_marksToFocus.end())
                {
                    PSendSysMessage("%s already have focus %s.", pTarget->GetName(), args);
                    return false;
                }

                PSendSysMessage("%s will focus %s.", pTarget->GetName(), args);
                pAI->m_marksToFocus.push_back(itrMark->second);
                return true;
            }
        }
        SendSysMessage("Target is not a party bot.");
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == pPlayer)
                continue;

            if (pMember->AI())
            {
                if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI()))
                {
                    if (std::find(pAI->m_marksToFocus.begin(), pAI->m_marksToFocus.end(), itrMark->second) != pAI->m_marksToFocus.end())
                    {
                        // Already have focus mark
                        continue;
                    }
                    pAI->m_marksToFocus.push_back(itrMark->second);
                }
            }
        }
    }

    PSendSysMessage("All party bots will focus %s.", args);
    return true;
}

bool ChatHandler::HandlePartyBotClearMarksCommand(char* args)
{
    Player* pPlayer = GetSession()->GetPlayer();
    Player* pTarget = GetSelectedPlayer();

    if (pTarget && (pTarget != pPlayer))
    {
        if (pTarget->AI())
        {
            if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pTarget->AI()))
            {
                PSendSysMessage("All mark assignments cleared for %s.", pTarget->GetName());
                pAI->m_marksToCC.clear();
                pAI->m_marksToFocus.clear();
                return true;
            }
        }
        SendSysMessage("Target is not a party bot.");
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == pPlayer)
                continue;

            if (pMember->AI())
            {
                if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI()))
                {
                    pAI->m_marksToCC.clear();
                    pAI->m_marksToFocus.clear();
                }
            }
        }
    }

    SendSysMessage("Mark assignments cleared for all bots.");
    return true;
}

bool HandlePartyBotComeToMeHelper(Player* pBot, Player* pPlayer)
{
    if (pBot->AI() && pBot->IsAlive() && pBot->IsInMap(pPlayer) && !pBot->HasUnitState(UNIT_STATE_NO_FREE_MOVE))
    {
        if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pBot->AI()))
        {
            if (pBot->GetVictim())
                StopPartyBotAttackHelper(pAI, pBot);

            if (pBot->GetStandState() != UNIT_STAND_STATE_STAND)
                pBot->SetStandState(UNIT_STAND_STATE_STAND);

            pBot->InterruptSpellsWithInterruptFlags(SPELL_INTERRUPT_FLAG_MOVEMENT);
            pBot->MonsterMove(pPlayer->GetPositionX(), pPlayer->GetPositionY(), pPlayer->GetPositionZ());
            return true;
        }
    }

    return false;
}

bool ChatHandler::HandlePartyBotComeToMeCommand(char* args)
{
    Player* pPlayer = GetSession()->GetPlayer();
    Player* pTarget = GetSelectedPlayer();

    bool ok = false;

    if (pTarget && pTarget != pPlayer)
    {
        if (ok = HandlePartyBotComeToMeHelper(pTarget, pPlayer))
            PSendSysMessage("%s is coming to your position.", pTarget->GetName());
        else
            PSendSysMessage("%s is not a party bot or it cannot move.", pTarget->GetName());
        return ok;
    }
    else if (Group* pGroup = pPlayer->GetGroup())
    {
        bool ok = false;
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Player* pMember = itr->getSource())
            {
                if (pMember == pPlayer)
                    continue;

                ok = HandlePartyBotComeToMeHelper(pMember, pPlayer) || ok;
            }
        }

        if (ok)
            SendSysMessage("All party bots are coming to your position.");
        else
            SendSysMessage("There are no party bots in the group or they cannot move.");
        return ok;
    }

    SendSysMessage("You are not in a group.");
    SetSentErrorMessage(true);
    return false;
}

bool HandlePartyBotUseGObjectHelper(Player* pTarget, GameObject* pGo)
{
    if (pTarget->AI())
    {
        if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pTarget->AI()))
        {
            if (pTarget->IsWithinDist(pGo, INTERACTION_DISTANCE))
            {
                pGo->Use(pTarget);
                return true;
            }
        }
    }

    return false;
}

bool ChatHandler::HandlePartyBotUseGObjectCommand(char* args)
{
    Player* pPlayer = GetSession()->GetPlayer();
    Player* pTarget = GetSelectedPlayer();

    GameObject* pGo = getSelectedGameObject();
    if (!pGo)
    {
        SendSysMessage(LANG_COMMAND_NOGAMEOBJECTFOUND);
        return false;
    }

    bool ok = false;

    if (pTarget && pTarget != pPlayer)
    {
        if (ok = HandlePartyBotUseGObjectHelper(pTarget, pGo))
            PSendSysMessage("%s has used the object.", pTarget->GetName());
        else
            PSendSysMessage("%s is not in range or is not a party bot.", pTarget->GetName());
        return ok;
    }
    else if (Group* pGroup = pPlayer->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Player* pMember = itr->getSource())
                ok = HandlePartyBotUseGObjectHelper(pMember, pGo) || ok;
        }

        if (ok)
            SendSysMessage("All party bots in range have used the object.");
        else
            SendSysMessage("There are no party bots in range of the object.");
        return ok;
    }

    SendSysMessage("You are not in a group.");
    SetSentErrorMessage(true);
    return false;
}

bool HandlePartyBotPauseApplyHelper(Player* pTarget, uint32 duration)
{
    if (pTarget->AI())
    {
        if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pTarget->AI()))
        {
            pAI->m_updateTimer.Reset(duration);

            if (duration)
            {
                pTarget->StopMoving();
                pTarget->GetMotionMaster()->MoveIdle();
            }

            return true;
        }
    }

    return false;
}

bool ChatHandler::HandlePartyBotPauseHelper(char* args, bool pause)
{
    bool all = false;
    uint32 duration = 0;
    if (char* arg1 = ExtractArg(&args))
    {
        if (!(all = (strcmp(arg1, "all") == 0)) && pause)
            duration = atoi(arg1);

        if (char* arg2 = ExtractArg(&args))
        {
            if (!duration && pause)
                duration = atoi(arg2);
            else if (!all)
                all = strcmp(arg2, "all") == 0;
        }
    }

    if (pause && !duration)
        duration = 5 * MINUTE * IN_MILLISECONDS;

    if (all)
    {
        Player* pPlayer = GetSession()->GetPlayer();
        Group* pGroup = pPlayer->GetGroup();
        if (!pGroup)
        {
            SendSysMessage("You are not in a group.");
            SetSentErrorMessage(true);
            return false;
        }

        bool success = false;
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Player* pMember = itr->getSource())
            {
                if (pMember == pPlayer)
                    continue;

                if (HandlePartyBotPauseApplyHelper(pMember, duration))
                    success = true;
            }
        }

        if (success)
        {
            if (pause)
                PSendSysMessage("All party bots paused for %u seconds.", (duration / IN_MILLISECONDS));
            else
                SendSysMessage("All party bots unpaused.");
        }
        else
            SendSysMessage("No party bots in group.");
    }
    else
    {
        Player* pTarget = GetSelectedPlayer();
        if (!pTarget)
        {
            SendSysMessage(LANG_NO_CHAR_SELECTED);
            SetSentErrorMessage(true);
            return false;
        }

        if (HandlePartyBotPauseApplyHelper(pTarget, duration))
        {
            if (pause)
                PSendSysMessage("%s paused for %u seconds.", pTarget->GetName(), (duration / IN_MILLISECONDS));
            else
                PSendSysMessage("%s unpaused.", pTarget->GetName());
        }

        else
            SendSysMessage("Target is not a party bot.");
    }

    return true;
}

bool ChatHandler::HandlePartyBotPauseCommand(char* args)
{
    return HandlePartyBotPauseHelper(args, true);
}

bool ChatHandler::HandlePartyBotUnpauseCommand(char* args)
{
    return HandlePartyBotPauseHelper(args, false);
}

bool ChatHandler::HandlePartyBotPullCommand(char* args)
{
    Player* pPlayer = GetSession()->GetPlayer();
    Unit* pTarget = GetSelectedUnit();
    if (!pTarget || !pPlayer->IsValidAttackTarget(pTarget, true))
    {
        SendSysMessage(LANG_SELECT_CHAR_OR_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    Group* pGroup = pPlayer->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("You are not in a group.");
        SetSentErrorMessage(true);
        return false;
    }

    uint32 duration;
    if (!ExtractUInt32(&args, duration))
        duration = 10 * IN_MILLISECONDS;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == pPlayer)
                continue;

            if (pMember->AI())
            {
                if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pMember->AI()))
                {
                    if (pAI->m_role == ROLE_MELEE_DPS || pAI->m_role == ROLE_RANGE_DPS)
                    {
                        HandlePartyBotPauseApplyHelper(pMember, duration);
                        continue;
                    }
                    else if (pAI->m_role == ROLE_TANK)
                    {
                        if (pMember->IsValidAttackTarget(pTarget))
                            pAI->AttackStart(pTarget);
                    }
                }
            }
        }
    }

    PSendSysMessage("Tank party bots are pulling %s, DPS party bots are paused for %d seconds.", pTarget->GetName(), (duration / IN_MILLISECONDS));
    return true;
}

bool ChatHandler::HandlePartyBotUnequipCommand(char* args)
{
    Player* pTarget = GetSelectedPlayer();
    if (!pTarget)
    {
        SendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }

    char* cId = ExtractKeyFromLink(&args, "Hitem");
    if (!cId)
        return false;

    uint32 itemId = 0;
    if (!ExtractUInt32(&cId, itemId))
        return false;

    uint32 count = pTarget->GetItemCount(itemId, false);
    if (!count)
    {
        SendSysMessage("Target does not have that item.");
        SetSentErrorMessage(true);
        return false;
    }

    if (pTarget->AI())
    {
        if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pTarget->AI()))
        {
            pTarget->DestroyItemCount(itemId, count, true);
            return true;
        }
    }

    SendSysMessage("Target is not a party bot.");
    SetSentErrorMessage(true);
    return false;
}

bool ChatHandler::HandlePartyBotRemoveCommand(char* args)
{
    Player* pTarget = GetSelectedPlayer();
    if (!pTarget)
    {
        SendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }

    if (pTarget->AI())
    {
        if (PartyBotAI* pAI = dynamic_cast<PartyBotAI*>(pTarget->AI()))
        {
            pAI->botEntry->requestRemoval = true;
            return true;
        }
    }

    SendSysMessage("Target is not a party bot.");
    SetSentErrorMessage(true);
    return false;
}

bool ChatHandler::HandleBattleBotAddAlteracCommand(char* args)
{
    return HandleBattleBotAddCommand(args, BATTLEGROUND_QUEUE_AV);
}

bool ChatHandler::HandleBattleBotAddArathiCommand(char* args)
{
    return HandleBattleBotAddCommand(args, BATTLEGROUND_QUEUE_AB);
}

bool ChatHandler::HandleBattleBotAddWarsongCommand(char* args)
{
    return HandleBattleBotAddCommand(args, BATTLEGROUND_QUEUE_WS);
}

bool ChatHandler::HandleBattleBotAddCommand(char* args, uint8 bg)
{
    if (!*args)
    {
        SendSysMessage("Incorrect syntax. Expected faction");
        SetSentErrorMessage(true);
        return false;
    }

    Team botTeam = HORDE;
    uint32 botLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
    bool isTemporary = false;
    std::string option;
    if (char* arg1 = ExtractArg(&args))
    {
        option = arg1;
        if (option == "horde")
            botTeam = HORDE;
        else if (option == "alliance")
            botTeam = ALLIANCE;
        else
        {
            SendSysMessage("Incorrect syntax. Expected faction");
            SetSentErrorMessage(true);
            return false;
        }


        ExtractUInt32(&args, botLevel);


        if (char* tempStr = ExtractArg(&args))
        {
            if (strcmp(tempStr, "temp") == 0)
                isTemporary = true;
        }
    }

    sPlayerBotMgr.AddBattleBot(BattleGroundQueueTypeId(bg), botTeam, botLevel, isTemporary);
    return true;
}

bool ChatHandler::HandleBattleBotRemoveCommand(char* args)
{
    Player* pTarget = GetSelectedPlayer();
    if (!pTarget)
    {
        SendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }

    if (pTarget->AI())
    {
        if (BattleBotAI* pAI = dynamic_cast<BattleBotAI*>(pTarget->AI()))
        {
            pAI->botEntry->requestRemoval = true;
            return true;
        }
    }

    SendSysMessage("Target is not a battle bot.");
    SetSentErrorMessage(true);
    return false;
}

bool ChatHandler::HandleBattleBotRemoveAllCommand(char* args)
{
    sPlayerBotMgr.DeleteBattleBots();
    SendSysMessage("Removed all battlebots.");
    return true;
}

bool ChatHandler::HandleBattleBotAutoJoin1Command(char* args)
{
    bool value;
    if (!ExtractOnOff(&args, value))
    {
        SendSysMessage(LANG_USE_BOL);
        SetSentErrorMessage(true);
        return false;
    }
    sPlayerBotMgr.SwitchAutoJoinBattleBots(value, 1);
    return true;    
}

bool ChatHandler::HandleBattleBotAutoJoin2Command(char* args)
{
    bool value;
    if (!ExtractOnOff(&args, value))
    {
        SendSysMessage(LANG_USE_BOL);
        SetSentErrorMessage(true);
        return false;
    }
    sPlayerBotMgr.SwitchAutoJoinBattleBots(value, 2);
    return true;    
}

bool ChatHandler::HandleBattleBotAutoJoin3Command(char* args)
{
    bool value;
    if (!ExtractOnOff(&args, value))
    {
        SendSysMessage(LANG_USE_BOL);
        SetSentErrorMessage(true);
        return false;
    }
    sPlayerBotMgr.SwitchAutoJoinBattleBots(value, 3);
    return true;
}

#define SPELL_RED_GLOW 20370

void ShowBattleBotPathHelper(Map* pMap, BattleBotPath* pPath, uint32 id)
{
    for (const auto& point : *pPath)
    {
        if (Creature* pWaypoint = pMap->SummonCreature(VISUAL_WAYPOINT, point.x, point.y, point.z, 0.0f, TEMPSUMMON_TIMED_DESPAWN, 120000, true))
        {
            // Show path id as level to distinguish individual paths.
            pWaypoint->SetUInt32Value(UNIT_FIELD_LEVEL, id);

            // Mark points that have script attached.
            if (point.pFunc)
                pWaypoint->CastSpell(pWaypoint, SPELL_RED_GLOW, true);
        }
    }
}

bool ChatHandler::HandleBattleBotShowPathCommand(char* args)
{
    Player* pTarget = GetSelectedPlayer();
    if (!pTarget)
    {
        SendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }

    if (pTarget->AI())
    {
        if (BattleBotAI* pAI = dynamic_cast<BattleBotAI*>(pTarget->AI()))
        {
            if (pAI->m_currentPath)
                ShowBattleBotPathHelper(pTarget->GetMap(), pAI->m_currentPath, 1);
            else
                SendSysMessage("Target is not following a path.");

            return true;
        }
    }

    SendSysMessage("Target is not a battle bot.");
    SetSentErrorMessage(true);
    return false;
}

bool ChatHandler::HandleBattleBotShowAllPathsCommand(char* args)
{
    Player* pPlayer = m_session->GetPlayer();
    BattleGround* pBG = pPlayer->GetBattleGround();
    if (!pBG)
    {
        SendSysMessage("You are not in a battleground.");
        return false;
    }

    std::vector<BattleBotPath*> const* pPaths;

    switch (pBG->GetTypeID())
    {
        case BATTLEGROUND_AB:
        {
            pPaths = &vPaths_AB;
            break;
        }
        case BATTLEGROUND_AV:
        {
            pPaths = &vPaths_AV;
            break;
        }
        case BATTLEGROUND_WS:
        {
            pPaths = &vPaths_WS;
            break;
        }
        default:
            return false;
    }

    uint32 id = 1;
    for (const auto& path : *pPaths)
    {
        ShowBattleBotPathHelper(pPlayer->GetMap(), path, id++);
    }

    PSendSysMessage("Showing %u paths for battleground.", id);
    return true;
}

// ---------------------------------------------------------------------------
// .battlebot path start|stop|status — GM waypoint path recorder
// ---------------------------------------------------------------------------

bool ChatHandler::HandleBattleBotPathStartCommand(char* args)
{
    if (!args || !*args)
    {
        SendSysMessage("Usage: .battlebot path start <PathName>  (no spaces in name)");
        SetSentErrorMessage(true);
        return false;
    }

    Player* player = m_session->GetPlayer();
    if (sOOMgr.IsRecording(player->GetObjectGuid()))
    {
        PSendSysMessage("Already recording (%u points). Use .battlebot path stop first.",
            sOOMgr.GetRecordedPointCount(player->GetObjectGuid()));
        SetSentErrorMessage(true);
        return false;
    }

    std::string name = args;
    // Replace spaces with underscores to ensure a valid C++ identifier
    for (char& c : name)
        if (c == ' ') c = '_';

    sOOMgr.StartPathRecording(player->GetObjectGuid(), name);
    PSendSysMessage("Path recording started: '%s'. Walk the route, then type .battlebot path stop.", name.c_str());
    return true;
}

bool ChatHandler::HandleBattleBotPathStopCommand(char* args)
{
    Player* player = m_session->GetPlayer();
    if (!sOOMgr.IsRecording(player->GetObjectGuid()))
    {
        SendSysMessage("No active recording. Use .battlebot path start <Name> first.");
        SetSentErrorMessage(true);
        return false;
    }

    std::string message;
    sOOMgr.StopPathRecording(player->GetObjectGuid(), message);
    SendSysMessage(message.c_str());
    return true;
}

bool ChatHandler::HandleBattleBotPathStatusCommand(char* args)
{
    Player* player = m_session->GetPlayer();
    if (!sOOMgr.IsRecording(player->GetObjectGuid()))
        SendSysMessage("No active recording.");
    else
        PSendSysMessage("Recording in progress: %u points captured so far.",
            sOOMgr.GetRecordedPointCount(player->GetObjectGuid()));
    return true;
}
