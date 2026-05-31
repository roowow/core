#include "BattleRoyaleMgr.h"
#include "BattleGroundBR.h"

#include "Policies/SingletonImp.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "MapManager.h"
#include "BattleGroundMgr.h"
#include "Chat.h"
#include "Log.h"
#include "World.h"
#include "WorldPacket.h"

#include <algorithm>

INSTANTIATE_SINGLETON_1(BattleRoyaleMgr);

BattleRoyaleMgr::BattleRoyaleMgr() = default;

void BattleRoyaleMgr::Update(uint32 diff)
{
    // BattleRoyale::Update is driven by BattleGroundMap::Update -> BattleGroundBR::Update.
    // Here we only check for completed instances and clean them up.
    for (auto it = m_instances.begin(); it != m_instances.end(); )
    {
        BattleRoyale* br = it->second;

        if (br->GetStatus() == BattleRoyaleStatus::CANCELLED)
        {
            uint32 instanceId = it->first;

            // Null the owner pointer BEFORE deleting to prevent use-after-free
            // (BattleGroundBR may still receive Update calls from the map)
            if (BattleGroundBR* host = br->GetHost())
                host->SetOwner(nullptr);

            // Remove player -> instance mappings
            for (auto jt = m_playerInstMap.begin(); jt != m_playerInstMap.end(); )
            {
                if (jt->second == instanceId)
                    jt = m_playerInstMap.erase(jt);
                else
                    ++jt;
            }

            delete br;
            it = m_instances.erase(it);
        }
        else
            ++it;
    }

    // Start countdown when queue fills
    if (!m_countdownActive && m_queue.size() >= MIN_PLAYERS)
    {
        m_countdownActive = true;
        m_countdownTimer  = COUNTDOWN_SEC * 1000;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, "[Battle Royale] 队列人数已达到，60 秒后开始对局！");
        sWorld.SendGlobalMessage(&data);
    }

    if (m_countdownActive)
    {
        if (m_countdownTimer <= diff)
        {
            m_countdownActive = false;
            m_countdownTimer  = 0;
            TryCreateGame();
        }
        else
            m_countdownTimer -= diff;
    }
}

bool BattleRoyaleMgr::EnqueuePlayer(Player* player, std::string& outError)
{
    if (!CanEnqueue(player, outError))
        return false;

    ObjectGuid guid = player->GetObjectGuid();
    m_queue.push_back(guid);
    ChatHandler(player).PSendSysMessage("[Battle Royale] 已加入队列（当前 %u 人）。", uint32(m_queue.size()));
    return true;
}

bool BattleRoyaleMgr::DequeuePlayer(Player* player)
{
    ObjectGuid guid = player->GetObjectGuid();
    auto it = std::find(m_queue.begin(), m_queue.end(), guid);
    if (it == m_queue.end())
        return false;

    m_queue.erase(it);

    if (m_queue.size() < MIN_PLAYERS)
    {
        m_countdownActive = false;
        m_countdownTimer  = 0;
    }

    ChatHandler(player).PSendSysMessage("[Battle Royale] 已离开队列。");
    return true;
}

bool BattleRoyaleMgr::IsPlayerInQueue(ObjectGuid guid) const
{
    return std::find(m_queue.begin(), m_queue.end(), guid) != m_queue.end();
}

bool BattleRoyaleMgr::IsPlayerInGame(ObjectGuid guid) const
{
    return m_playerInstMap.find(guid) != m_playerInstMap.end();
}

void BattleRoyaleMgr::ForceStartNow()
{
    m_countdownActive = false;
    m_countdownTimer  = 0;
    TryCreateGame();
}

BattleRoyale* BattleRoyaleMgr::GetInstanceForPlayer(ObjectGuid guid)
{
    auto it = m_playerInstMap.find(guid);
    if (it == m_playerInstMap.end())
        return nullptr;
    auto jt = m_instances.find(it->second);
    return jt != m_instances.end() ? jt->second : nullptr;
}

void BattleRoyaleMgr::OnInstanceEnd(uint32 /*instanceId*/)
{
    // Cleanup handled in Update when status == CANCELLED
}

// --- private ---

bool BattleRoyaleMgr::CanEnqueue(Player* player, std::string& outError) const
{
    ObjectGuid guid = player->GetObjectGuid();

    if (IsPlayerInQueue(guid))    { outError = "你已在队列中。";       return false; }
    if (IsPlayerInGame(guid))     { outError = "你已在对局中。";       return false; }
    if (player->GetLevel() < 60)  { outError = "需要 60 级才能参与。"; return false; }
    if (player->GetGroup())       { outError = "请先退出队伍再排队。"; return false; }
    if (player->InBattleGround()) { outError = "请先离开当前战场。";   return false; }
    if (player->IsInCombat())     { outError = "请先脱离战斗状态。";   return false; }
    if (player->IsDead())         { outError = "请在存活状态下排队。"; return false; }
    if (player->IsTaxiFlying())   { outError = "飞行途中无法排队。";   return false; }
    if (player->GetMap() && player->GetMap()->IsDungeon())
                                  { outError = "请先离开副本再排队。"; return false; }

    return true;
}

void BattleRoyaleMgr::TryCreateGame()
{
    if (m_queue.empty())
        return;

    BattleRoyaleTemplate const& tmpl = GetABTemplate();

    // Collect online players up to maxPlayers
    std::vector<Player*> players;
    std::deque<ObjectGuid> remaining;

    for (auto it = m_queue.begin(); it != m_queue.end(); ++it)
    {
        ObjectGuid const& guid = *it;
        if (uint32(players.size()) >= tmpl.maxPlayers)
        {
            remaining.push_back(guid);
            continue;
        }
        Player* p = sObjectMgr.GetPlayer(guid);
        if (p && p->IsInWorld())
            players.push_back(p);
        else
            remaining.push_back(guid); // offline, preserve in queue
    }

    m_queue = remaining;

    if (uint32(players.size()) < MIN_PLAYERS)
    {
        // Not enough online players – put them back and wait
        for (Player* p : players)
            m_queue.push_front(p->GetObjectGuid());
        return;
    }

    BattleRoyale* br = CreateInstance(players);
    if (!br)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[BattleRoyaleMgr] Failed to create BR instance.");
        return;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "[BattleRoyaleMgr] Created BR instance with %u players.", uint32(players.size()));
}

BattleRoyale* BattleRoyaleMgr::CreateInstance(std::vector<Player*> const& players)
{
    BattleRoyaleTemplate const& tmpl = GetABTemplate();

    auto* host = new BattleGroundBR();
    host->SetMaxPlayers(tmpl.maxPlayers);
    host->SetMinPlayers(1);
    host->SetMapId(tmpl.mapId);

    Map* map = sMapMgr.CreateBgMap(tmpl.mapId, host);
    if (!map)
    {
        delete host;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[BattleRoyaleMgr] Failed to create BG map %u.", tmpl.mapId);
        return nullptr;
    }

    sBattleGroundMgr.AddBattleGround(host->GetInstanceID(), BATTLEGROUND_BR, host);

    auto* br = new BattleRoyale(&tmpl, host);
    host->SetOwner(br);

    uint32 instanceId = host->GetInstanceID();
    m_instances[instanceId] = br;

    std::vector<BRSpawnPoint> const& spawns = tmpl.spawnPoints;
    for (uint32 i = 0; i < uint32(players.size()); ++i)
    {
        Player* player = players[i];
        BRSpawnPoint const& sp = spawns[i % spawns.size()];

        player->SetBattleGroundEntryPoint();
        player->TeleportTo(tmpl.mapId, sp.x, sp.y, sp.z, sp.o);

        br->AddPlayer(player);
        m_playerInstMap[player->GetObjectGuid()] = instanceId;
    }

    return br;
}
