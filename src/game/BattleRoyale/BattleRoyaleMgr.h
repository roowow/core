#ifndef MANGOS_BATTLEROYALEMGR_H
#define MANGOS_BATTLEROYALEMGR_H

#include "BattleRoyale.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
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
    void SetEnabled(bool enabled)
    {
        m_enabled = enabled;
        sObjectMgr.SetSavedVariable(VAR_BATTLE_ROYALE_ENABLED, enabled ? 1u : 0u, true);
    }
    bool IsEnabled()        const { return m_enabled; }
    bool ForceStartNow(uint32 templateId = 0, std::string* outError = nullptr); // bypasses MIN_PLAYERS for single-GM testing
    BattleRoyale* GetInstanceForPlayer(ObjectGuid guid);

    // Real players are restored to their pre-BR position on normal exit, relog,
    // or after a server restart. Bots are not persisted.
    void SavePendingRestore(Player const* player, uint32 instanceId) const;
    void ClearPendingRestore(ObjectGuid guid) const;
    bool RestorePendingPlayer(Player* player) const;

    // Called by BattleRoyale when a player is returned to the world (eliminated or game over).
    // Removes the player from m_playerInstMap so they can re-queue immediately.
    void RemovePlayerFromInstance(ObjectGuid guid);

    // Called by BattleRoyale when it finishes
    void OnInstanceEnd(uint32 instanceId);

    // Called by BattleBotAI when a BR bot finishes initialization and is ready to enter the instance
    void OnBotReady(Player* bot, uint32 instanceId);

    // Load (or reload) player spawn points from battle_royale_spawn_point table into the template.
    // Safe to call at runtime (e.g., after .br spawn add).
    void LoadSpawnPoints();

private:
    bool CanEnqueue(Player* player, std::string& outError) const;
    bool TryCreateGame(bool ignoreMinPlayers = false, uint32 templateId = 0, std::string* outError = nullptr);
    BattleRoyale* CreateInstance(std::vector<Player*> const& players, BattleRoyaleTemplate const& tmpl);
    void SendMsgToParticipants(char const* msg) const; // queue + active instances only, not world

    std::deque<ObjectGuid>                        m_queue;
    std::map<uint32, BattleRoyale*>               m_instances;       // instanceId -> BattleRoyale
    std::map<ObjectGuid, uint32>                  m_playerInstMap;   // playerGuid -> instanceId
    std::map<uint32, std::vector<uint32>>          m_botSpawnIndexes; // instanceId -> remaining shuffled spawn indexes for bots

    uint32  m_countdownTimer    = 0;
    uint32  m_nextReminderSec   = 0;
    bool    m_countdownActive   = false;
    bool    m_enabled           = true;

    static uint32 const REMINDER_INTERVAL_SEC  = 60;   // broadcast reminder every 60s
};

#define sBattleRoyaleMgr MaNGOS::Singleton<BattleRoyaleMgr>::Instance()

#endif // MANGOS_BATTLEROYALEMGR_H
