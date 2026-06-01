#ifndef MANGOS_BATTLEROYALEMGR_H
#define MANGOS_BATTLEROYALEMGR_H

#include "BattleRoyale.h"
#include "ObjectGuid.h"
#include "Policies/Singleton.h"

#include <deque>
#include <map>
#include <string>

class Player;

class BattleRoyaleMgr
{
public:
    BattleRoyaleMgr();

    // Called from World::Update
    void Update(uint32 diff);

    // Queue management (called by gossip NPC)
    bool EnqueuePlayer(Player* player, std::string& outError);
    bool DequeuePlayer(Player* player);
    bool IsPlayerInQueue(ObjectGuid guid) const;
    bool IsPlayerInGame(ObjectGuid guid)  const;
    uint32 GetQueueSize() const { return uint32(m_queue.size()); }

    // GM commands
    void ForceStartNow();                  // bypasses MIN_PLAYERS for single-GM testing
    BattleRoyale* GetInstanceForPlayer(ObjectGuid guid);

    // Called by BattleRoyale when it finishes
    void OnInstanceEnd(uint32 instanceId);

    // Called by BattleBotAI when a BR bot finishes initialization and is ready to enter the instance
    void OnBotReady(Player* bot, uint32 instanceId);

private:
    bool CanEnqueue(Player* player, std::string& outError) const;
    void TryCreateGame(bool ignoreMinPlayers = false);
    BattleRoyale* CreateInstance(std::vector<Player*> const& players);

    std::deque<ObjectGuid>                        m_queue;
    std::map<uint32, BattleRoyale*>               m_instances;       // instanceId -> BattleRoyale
    std::map<ObjectGuid, uint32>                  m_playerInstMap;   // playerGuid -> instanceId
    std::map<uint32, std::vector<uint32>>          m_botSpawnIndexes; // instanceId -> remaining shuffled spawn indexes for bots

    uint32  m_countdownTimer  = 0;
    bool    m_countdownActive = false;

    static uint32 const MIN_PLAYERS   = 2;    // minimum real players to start (low for GM testing)
    static uint32 const COUNTDOWN_SEC = 60;
};

#define sBattleRoyaleMgr MaNGOS::Singleton<BattleRoyaleMgr>::Instance()

#endif // MANGOS_BATTLEROYALEMGR_H
