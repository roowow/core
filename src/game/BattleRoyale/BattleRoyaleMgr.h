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

// A real player's join info, captured at lock time (see BattleRoyaleMgr's countdown
// lock mechanism) but not acted on (AddPlayer/TeleportTo) until
// BattleRoyaleMgr::AdmitPendingRealPlayers() runs, once the countdown reaches zero.
struct BRPendingRealPlayerJoin
{
    ObjectGuid   guid;
    BRSpawnPoint landingPoint;
    uint32       deploymentPathId;
};

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
    // .br start [templateId] — no longer bypasses the normal minimum-player/countdown
    // gate. Just forces which template the *next* countdown (current one, if already
    // running) will use instead of a random pick; the match still only actually
    // starts once enough real players are queued and the countdown reaches zero,
    // same as automatic matchmaking. templateId = 0 clears any forced selection.
    bool ForceStartNow(uint32 templateId, std::string* outError);
    BattleRoyale* GetInstanceForPlayer(ObjectGuid guid);

    // Anonymous display name for a real player currently in an active BR match
    // (see BattleRoyalePlayer::anonName; GM observers get one too, same as any
    // other real player). Returns false for bots or players not currently in a
    // match — callers should fall back to the real name.
    bool TryGetAnonName(ObjectGuid guid, std::string& outName);

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

    // Load (or reload) spawn_index -> custom_taxi_path_id mappings from
    // battle_royale_deployment_path into the template. Safe to call at runtime.
    void LoadDeploymentPaths();

    // Delete a player's corpse (with BR loot on it) after delayMs — gives other
    // participants a window to loot it first. Tracked here (not on the per-match
    // BattleRoyale instance) because that instance can be destroyed almost
    // immediately after the last ReturnPlayer() calls at match end (see Update()'s
    // CANCELLED cleanup), before a multi-minute timer tracked on it could ever fire.
    void ScheduleCorpseCleanup(ObjectGuid guid, uint32 delayMs);

private:
    bool CanEnqueue(Player* player, std::string& outError) const;
    // Selects + validates real players from the queue and a template (forced or
    // random), then calls CreateInstance(). Always defers real players' actual
    // join (see CreateInstance()) — callers decide when to admit them via
    // AdmitPendingRealPlayers(). outInstance receives the created match on success.
    bool TryCreateGame(uint32 templateId, std::string* outError, BattleRoyale** outInstance);
    BattleRoyale* CreateInstance(std::vector<Player*> const& players, BattleRoyaleTemplate const& tmpl);
    void SendMsgToParticipants(char const* msg) const; // queue + active instances only, not world

    // Actually admits (AddPlayer + TeleportTo) the real players CreateInstance()
    // deferred for this instance, then releases any bot still in its holding loop.
    // Called once the pre-match countdown reaches zero (see Update()).
    void AdmitPendingRealPlayers(uint32 instanceId, BattleRoyale* br);

    std::deque<ObjectGuid>                        m_queue;
    std::map<uint32, BattleRoyale*>               m_instances;       // instanceId -> BattleRoyale
    std::map<ObjectGuid, uint32>                  m_playerInstMap;   // playerGuid -> instanceId
    std::map<uint32, std::vector<uint32>>          m_botSpawnIndexes; // instanceId -> remaining shuffled spawn indexes for bots
    std::vector<std::pair<ObjectGuid, int32>>      m_pendingCorpseCleanup; // guid, remaining ms (see ScheduleCorpseCleanup)
    std::map<uint32, std::vector<BRPendingRealPlayerJoin>> m_pendingRealPlayerJoins; // instanceId -> deferred real player joins

    uint32  m_countdownTimer    = 0;
    uint32  m_nextReminderSec   = 0;
    bool    m_countdownActive   = false;
    bool    m_enabled           = true;

    // Countdown lock: once m_countdownTimer drops to BR_LOCK_THRESHOLD_MS or below,
    // the current batch of queued players + a template are selected and
    // CreateInstance() is called right away (bots start logging in and circling),
    // instead of waiting for the countdown to reach zero. m_lockedInstanceId is
    // admitted (real players teleported in) once the countdown actually hits zero.
    bool    m_locked            = false;
    uint32  m_lockedInstanceId  = 0;
    // Set by .br start <templateId> (ForceStartNow); consumed (reset to 0) the next
    // time a template is selected, whether at lock time or as the immediate
    // fallback (see Update()) if the countdown was already under the lock threshold.
    uint32  m_forcedTemplateId  = 0;

    static uint32 const REMINDER_INTERVAL_SEC  = 60;   // broadcast reminder every 60s
};

#define sBattleRoyaleMgr MaNGOS::Singleton<BattleRoyaleMgr>::Instance()

#endif // MANGOS_BATTLEROYALEMGR_H
