#include "BattleRoyale.h"
#include "BattleGroundBR.h"

#include "BattleRoyaleMgr.h"
#include "Player.h"
#include "Map.h"
#include "MirrorTimer.h"
#include "BattleGround.h"
#include "BattleGroundMgr.h"
#include "Chat.h"
#include "CustomTaxiMgr.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "PlayerBotMgr.h"

#include "Mail.h"
#include "Corpse.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "Database/DatabaseEnv.h"

#include <cmath>
#include <cstdio>

static uint32 const BR_FINISH_DELAY_MS           = 10000;
static float  const BR_LANDING_CORRECTION_DISTANCE = 5.0f;
// Orbit path ID is now per-template (BattleRoyaleTemplate::orbitPathId).
static float  const BR_TWO_PI                    = 6.2831853071795864769f;

// Reference loot table entry for BR corpse drops (reference_loot_template.entry).
static uint32 const BR_CORPSE_LOOT_REF_ID = 9001;

// Season score awarded per placement and per kill.
static uint32 const BR_SCORE_RANK1    = 10;
static uint32 const BR_SCORE_RANK2    = 5;
static uint32 const BR_SCORE_RANK3    = 3;
static uint32 const BR_SCORE_PER_KILL = 1;
static uint32 const BR_WINNER_CELEBRATION_SPELL_ID = 27571;

static uint32 const BR_KILL_REWARD_BUFFS[] =
{
    10938, // Power Word: Fortitude
    14819, // Divine Spirit
    10958, // Shadow Protection
    10157, // Arcane Intellect
    9885,  // Mark of the Wild
    9910,  // Thorns
    19838, // Blessing of Might
    19979, // Blessing of Light
    20217, // Blessing of Kings
    19853, // Blessing of Wisdom
};

static void ApplyBattleRoyaleKillRewardBuff(Player* killer)
{
    if (!killer || !killer->IsAlive())
        return;

    uint32 const buffCount = sizeof(BR_KILL_REWARD_BUFFS) / sizeof(BR_KILL_REWARD_BUFFS[0]);
    uint32 const spellId = BR_KILL_REWARD_BUFFS[urand(0, buffCount - 1)];
    killer->CastSpell(killer, spellId, true);
}

static uint32 BattleRoyaleMixSeed(uint32 value)
{
    value ^= value >> 16;
    value *= 0x7feb352d;
    value ^= value >> 15;
    value *= 0x846ca68b;
    value ^= value >> 16;
    return value;
}

// BR item entries — must match what is actually in item_template (DB).
// Used only for CleanupBRItems() when a player exits the match.
// 900214-900221 reserved for future items; not yet in DB, omitted here.
static uint32 const BR_ITEM_ENTRIES[] =
{
    900210, 900211, 900213,
    900222, 900223, 900225, 900226
};


BattleRoyale::BattleRoyale(BattleRoyaleTemplate const* tmpl, BattleGroundBR* host)
    : m_status(BattleRoyaleStatus::DEPLOYING), m_tmpl(tmpl), m_host(host),
      m_landedCount(0),
      m_aliveCount(0), m_totalCount(0), m_finishTimer(0), m_runningTime(0)
{
    m_zone.Init(tmpl);
}

void BattleRoyale::AddPlayer(Player* player, BRSpawnPoint const& landingPoint, uint32 deploymentPathId, bool isBot)
{
    ObjectGuid guid = player->GetObjectGuid();

    BattleRoyalePlayer brPlayer;
    brPlayer.guid             = guid;
    brPlayer.alive            = true;
    brPlayer.bot              = isBot;
    brPlayer.outsideZone      = false;
    brPlayer.zoneWarnTimer    = 0;
    brPlayer.placementRank    = 0;
    brPlayer.landingPoint     = landingPoint;
    brPlayer.deploymentPathId = deploymentPathId;
    brPlayer.savedPosition    = WorldLocation(player->GetMapId(),
                                              player->GetPositionX(),
                                              player->GetPositionY(),
                                              player->GetPositionZ(),
                                              player->GetOrientation());
    brPlayer.savedFFAPvP = player->IsFFAPvP();

    if (!isBot)
        sBattleRoyaleMgr.SavePendingRestore(player, m_host ? m_host->GetInstanceID() : 0);

    m_players[guid] = brPlayer;
    ++m_totalCount;
    ++m_aliveCount;
    if (isBot && m_pendingBotCount > 0)
        --m_pendingBotCount;

    player->SetFFAPvP(true);
    player->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_IMMUNE_TO_NPC);
    player->SetBattleGroundId(m_host->GetInstanceID(), BATTLEGROUND_BR);
    player->SetBGTeam(TEAM_NONE);

    if (!isBot)
        ChatHandler(player).PSendSysMessage("[孤胆称雄] 风云已起，你将踏空入局。此番已有 %u 名侠士赴约。", m_totalCount);
}

void BattleRoyale::Update(uint32 diff)
{
    Map* map = m_host ? m_host->GetBgMap() : nullptr;

    if (m_status == BattleRoyaleStatus::DEPLOYING)
    {
        UpdateDeploying(diff, map);
        return;
    }


    if (m_status == BattleRoyaleStatus::RUNNING)
    {
        m_runningTime += diff;

        if (map)
        {
            uint32 phaseBefore = m_zone.GetPhase();
            m_zone.Update(diff, m_players, map);
            uint32 phaseAfter = m_zone.GetPhase();
            if (phaseAfter != phaseBefore)
                BroadcastPhaseChange(phaseAfter);
        }

        // Check for player deaths and re-enforce FFA.
        // Player::UpdateArea() clears PLAYER_FLAGS_FFA_PVP on every sub-zone change (AB has
        // several sub-zones), so we re-apply it each update if it was cleared.
        if (map)
        {
            std::vector<ObjectGuid> toEliminate;
            for (auto it = m_players.begin(); it != m_players.end(); ++it)
            {
                if (!it->second.alive)
                    continue;
                Player* player = map->GetPlayer(it->first);
                if (!player || !player->IsAlive())
                {
                    toEliminate.push_back(it->first);
                    continue;
                }
                if (!player->IsFFAPvP())
                    player->SetFFAPvP(true);
                if (player->IsMounted())
                    player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
            }
            for (ObjectGuid const& guid : toEliminate)
                Eliminate(guid);
        }
        return;
    }

    if (m_status == BattleRoyaleStatus::FINISHED)
    {
        if (m_finishTimer <= diff)
        {
            if (map)
            {
                for (auto it = m_players.begin(); it != m_players.end(); ++it)
                {
                    Player* player = map->GetPlayer(it->first);
                    if (player)
                        ReturnPlayer(player, it->second);
                }
            }
            m_status = BattleRoyaleStatus::CANCELLED;
        }
        else
            m_finishTimer -= diff;
    }
}

void BattleRoyale::OnPlayerDied(ObjectGuid victim, ObjectGuid killer)
{
    auto it = m_players.find(victim);
    if (it != m_players.end() && it->second.alive)
        Eliminate(victim, true, killer);
}

void BattleRoyale::OnPlayerLeftMap(ObjectGuid guid)
{
    auto it = m_players.find(guid);
    if (it == m_players.end())
        return;

    // If the player leaves during deployment (e.g. disconnect), count them as landed
    // so the remaining players are not stuck waiting forever.
    if (m_status == BattleRoyaleStatus::DEPLOYING && !it->second.landed)
    {
        it->second.landed = true;
        ++m_landedCount;
    }

    if (it->second.alive)
        Eliminate(guid, false);
}

uint32 BattleRoyale::GetAliveCount() const
{
    return m_aliveCount;
}

bool BattleRoyale::IsAlive(ObjectGuid guid) const
{
    auto it = m_players.find(guid);
    return it != m_players.end() && it->second.alive;
}

void BattleRoyale::ForceSetPhase(uint32 phase)
{
    m_zone.ForcePhase(phase);
    if (Map* map = m_host ? m_host->GetBgMap() : nullptr)
        m_zone.RefreshMarkers(map);
}

void BattleRoyale::ForceSetRadius(float radius)
{
    m_zone.ForceRadius(radius);
    if (Map* map = m_host ? m_host->GetBgMap() : nullptr)
        m_zone.RefreshMarkers(map);
}

// --- private ---

void BattleRoyale::UpdateDeploying(uint32 diff, Map* map)
{
    if (!map)
        return;

    if (!m_orbitStarted)
        m_orbitStarted = true;

    // Look up orbit nodes once per tick (shared by all players, per-template path).
    auto const& taxiPaths = sCustomTaxiMgr.GetPaths();
    auto const orbitIt = m_tmpl ? taxiPaths.find(m_tmpl->orbitPathId) : taxiPaths.end();
    std::vector<TaxiPathNodeEntry> const* orbitNodes =
        (orbitIt != taxiPaths.end() && !orbitIt->second.nodes.empty())
        ? &orbitIt->second.nodes : nullptr;

    for (auto it = m_players.begin(); it != m_players.end(); ++it)
    {
        BattleRoyalePlayer& brPlayer = it->second;
        if (brPlayer.landed)
            continue;

        Player* player = map->GetPlayer(it->first);
        if (!player)
            continue;

        if (player->IsTaxiFlying())
            continue; // combined orbit+drop flight in progress

        if (brPlayer.deploymentStarted)
        {
            // Combined flight ended — land.
            CompleteDeployment(player, brPlayer, false);
            continue;
        }

        // Player hasn't started the combined flight yet.
        // Hold them in place with hover until the flight starts.
        if (!player->IsHovering() && !player->HasPendingMovementChange(SET_HOVER))
            player->SetHover(true);
        player->SetHoverReal(true);
        player->SetFallInformation(0);

        if (!orbitNodes)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                     "[BattleRoyale] Orbit path %u not loaded; teleporting %s to landing point.",
                     m_tmpl ? m_tmpl->orbitPathId : 0u, player->GetName());
            player->SetHover(false);
            player->SetHoverReal(false);
            CompleteDeployment(player, brPlayer, true);
            continue;
        }

        if (!brPlayer.deploymentPathId)
        {
            // No drop path — land directly.
            player->SetHover(false);
            player->SetHoverReal(false);
            CompleteDeployment(player, brPlayer, true);
            continue;
        }

        auto const dropIt = taxiPaths.find(brPlayer.deploymentPathId);
        if (dropIt == taxiPaths.end() || dropIt->second.nodes.empty())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                     "[BattleRoyale] Drop path %u not loaded for %s; teleporting to landing point.",
                     brPlayer.deploymentPathId, player->GetName());
            player->SetHover(false);
            player->SetHoverReal(false);
            CompleteDeployment(player, brPlayer, true);
            continue;
        }

        // Build a personal combined path. Everyone starts from the staging point,
        // immediately fans out to a different point on the orbit ring, then follows
        // the shared orbit from a rotated node before taking their own drop path.
        std::vector<TaxiPathNodeEntry> combined;
        combined.reserve(orbitNodes->size() + dropIt->second.nodes.size() + 2);

        uint32 const seed = BattleRoyaleMixSeed(player->GetObjectGuid().GetCounter() ^ brPlayer.deploymentPathId);
        size_t const orbitCount = orbitNodes->size();
        uint32 const angleBucket = seed % 10000;
        float const angle = BR_TWO_PI * float(angleBucket) / 10000.0f;
        size_t const orbitStart = size_t(float(angleBucket) * float(orbitCount) / 10000.0f + 0.5f) % orbitCount;

        TaxiPathNodeEntry startNode = (*orbitNodes)[orbitStart];
        startNode.index = 0;
        startNode.mapid = player->GetMapId();
        startNode.x = player->GetPositionX();
        startNode.y = player->GetPositionY();
        startNode.z = player->GetPositionZ();
        startNode.delay = 0;
        combined.push_back(startNode);

        float const centerX = m_tmpl ? m_tmpl->centerX : (*orbitNodes)[orbitStart].x;
        float const centerY = m_tmpl ? m_tmpl->centerY : (*orbitNodes)[orbitStart].y;
        float const radiusX = (*orbitNodes)[orbitStart].x - centerX;
        float const radiusY = (*orbitNodes)[orbitStart].y - centerY;
        float radius = std::sqrt(radiusX * radiusX + radiusY * radiusY);
        if (radius < 5.0f)
            radius = 60.0f;

        TaxiPathNodeEntry spreadNode = (*orbitNodes)[orbitStart];
        spreadNode.index = 1;
        spreadNode.mapid = player->GetMapId();
        spreadNode.x = centerX + std::cos(angle) * radius;
        spreadNode.y = centerY + std::sin(angle) * radius;
        spreadNode.z = (*orbitNodes)[orbitStart].z;
        spreadNode.delay = 0;
        combined.push_back(spreadNode);

        for (size_t i = 0; i < orbitCount; ++i)
        {
            TaxiPathNodeEntry node = (*orbitNodes)[(orbitStart + i) % orbitCount];
            node.index = uint32(combined.size());
            combined.push_back(node);
        }

        uint32 const offset = uint32(combined.size());
        for (auto const& n : dropIt->second.nodes)
        {
            TaxiPathNodeEntry node = n;
            node.index = offset + n.index;
            combined.push_back(node);
        }

        // Setup and start combined flight (mirrors CustomTaxiMgr::Play internals).
        player->CombatStop();
        player->TradeCancel(true);
        player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
        if (player->IsInDisallowedMountForm())
        {
            player->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
            player->RemoveSpellsCausingAura(SPELL_AURA_TRANSFORM);
        }
        player->InterruptNonMeleeSpells(false);

        if (!player->GetTaxi().SetCustomTaxiPath(combined))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                     "[BattleRoyale] SetCustomTaxiPath failed for %s; teleporting to landing point.", player->GetName());
            player->SetHover(false);
            player->SetHoverReal(false);
            CompleteDeployment(player, brPlayer, true);
            continue;
        }

        // Prefer the drop path's own mountDisplayId, then orbit path's, then faction default.
        uint32 mountDisplayId = dropIt->second.mountDisplayId
            ? dropIt->second.mountDisplayId
            : orbitIt->second.mountDisplayId;
        if (!mountDisplayId)
        {
            uint32 const sourceTaxiNode = player->GetTeam() == ALLIANCE ? 2 : 23;
            mountDisplayId = sObjectMgr.GetTaxiMountDisplayId(sourceTaxiNode, player->GetTeam(), true);
        }
        if (!mountDisplayId)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                     "[BattleRoyale] No mount display ID for %s; teleporting to landing point.", player->GetName());
            player->SetHover(false);
            player->SetHoverReal(false);
            CompleteDeployment(player, brPlayer, true);
            continue;
        }

        player->SetHover(false);
        player->SetHoverReal(false);
        player->GetSession()->SendDoFlight(mountDisplayId, 0);

        brPlayer.orbitStarted    = true;
        brPlayer.deploymentStarted = true;
        ChatHandler(player).PSendSysMessage("[孤胆称雄] 长风送客，御空而下。落地之后，刀剑无情，唯凭本事。");
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL,
                 "[BattleRoyale] Combined orbit+drop flight started for %s instance %u.",
                 player->GetName(), m_host ? m_host->GetInstanceID() : 0u);
    }

    // m_pendingBotCount tracks bots requested but not yet arrived. A bot whose
    // creation failed never calls OnBotReady, so it never calls AddPlayer and
    // never enters m_totalCount. Excluding it from the trigger would stall the
    // game indefinitely, so we only gate on the players we actually registered.
    if (m_landedCount >= m_totalCount)
        StartRunning();
}

void BattleRoyale::CompleteDeployment(Player* player, BattleRoyalePlayer& brPlayer, bool teleportToLandingPoint)
{
    if (brPlayer.landed)
        return;

    if (player && teleportToLandingPoint)
    {
        if (player->IsTaxiFlying())
        {
            player->GetMotionMaster()->MovementExpired();
            player->GetTaxi().ClearTaxiDestinations();
        }

        BRSpawnPoint const& landing = brPlayer.landingPoint;
        float landZ = landing.z;
        // Bots have no client-side physics. Snap their Z to actual terrain so they
        // don't float at spawn points that are slightly above the ground mesh.
        if (brPlayer.bot)
        {
            Map* bgMap = m_host ? m_host->GetBgMap() : nullptr;
            if (bgMap)
            {
                float groundZ = bgMap->GetHeight(landing.x, landing.y, MAX_HEIGHT, false, MAX_HEIGHT);
                if (groundZ > INVALID_HEIGHT)
                    landZ = groundZ;
            }
        }
        player->TeleportTo(m_tmpl->mapId, landing.x, landing.y, landZ, landing.o);
    }
    else if (player)
    {
        // Keep DB spawn points authoritative. If an old or hand-written route ends away
        // from the assigned spawn point, snap the participant to the recorded landing.
        // Bots also need a terrain-Z snap because they do not fall via client physics.
        BRSpawnPoint const& landing = brPlayer.landingPoint;
        float landZ = landing.z;
        Map* bgMap = m_host ? m_host->GetBgMap() : nullptr;
        if (brPlayer.bot && bgMap)
        {
            float groundZ = bgMap->GetHeight(landing.x, landing.y, MAX_HEIGHT, false, MAX_HEIGHT);
            if (groundZ > INVALID_HEIGHT)
                landZ = groundZ;
        }

        bool const farFromLanding = player->GetDistance(landing.x, landing.y, landZ) > BR_LANDING_CORRECTION_DISTANCE;
        bool const botFloating = brPlayer.bot && player->GetPositionZ() > landZ + 0.5f;
        if (farFromLanding || botFloating)
            player->TeleportTo(m_tmpl->mapId, landing.x, landing.y, landZ, landing.o);
    }

    if (player && player->IsMounted())
        player->Unmount();

    brPlayer.deploymentStarted = false;
    brPlayer.landed = true;
    ++m_landedCount;
}

void BattleRoyale::StartRunning()
{
    if (m_status != BattleRoyaleStatus::DEPLOYING)
        return;

    m_status = BattleRoyaleStatus::RUNNING;
    m_pendingBotCount = 0; // any bot that hasn't arrived is abandoned; clear the counter

    Map* map = m_host ? m_host->GetBgMap() : nullptr;

    // Re-apply FFA PvP now that players are on the BG map.
    // Player::UpdateArea() clears the flag when entering a non-arena area,
    // so we must re-set it after teleport completes.
    if (map)
    {
        for (auto it = m_players.begin(); it != m_players.end(); ++it)
        {
            if (!it->second.alive)
                continue;
            if (Player* player = map->GetPlayer(it->first))
                player->SetFFAPvP(true);
        }
    }

    // Spawn zone markers and drain the queue immediately so the boundary
    // is visible from the first moment the game starts.
    if (map)
    {
        m_zone.RefreshMarkers(map);
        m_zone.DrainSpawnQueue(map);
    }

    for (auto it = m_players.begin(); it != m_players.end(); ++it)
    {
        if (!it->second.alive)
            continue;
        Player* player = map ? map->GetPlayer(it->first) : nullptr;
        if (!player)
            continue;
        player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_IMMUNE_TO_NPC);
    }

    BroadcastPhaseChange(0);

}

void BattleRoyale::Eliminate(ObjectGuid guid, bool notify, ObjectGuid killerGuid)
{
    auto it = m_players.find(guid);
    if (it == m_players.end() || !it->second.alive)
        return;

    it->second.alive         = false;
    it->second.placementRank = m_aliveCount;
    --m_aliveCount;

    uint32 const survivalSec = m_runningTime / 1000;

    BRRankEntry entry;
    entry.guid        = guid;
    entry.rank        = it->second.placementRank;
    entry.survivalSec = survivalSec;
    m_ranks.push_back(entry);

    // Credit kill to the killer (works for both real players and bots)
    if (killerGuid && killerGuid != guid)
    {
        auto killerIt = m_players.find(killerGuid);
        if (killerIt != m_players.end())
        {
            ++killerIt->second.killCount;
            if (Map* map = m_host ? m_host->GetBgMap() : nullptr)
                ApplyBattleRoyaleKillRewardBuff(map->GetPlayer(killerGuid));
        }
    }

    Map* map = m_host ? m_host->GetBgMap() : nullptr;
    Player* player = map ? map->GetPlayer(guid) : nullptr;

    if (notify && player)
        ChatHandler(player).PSendSysMessage("[孤胆称雄] 此番江湖路止于此，最终名次第 %u。", it->second.placementRank);

    // Broadcast to survivors
    {
        char buf[192];
        std::string victimName = player ? player->GetName() : "一名试炼者";

        if (killerGuid && killerGuid != guid)
        {
            Player* killer = map ? map->GetPlayer(killerGuid) : nullptr;
            std::string killerName = killer ? killer->GetName() : "未知猎手";
            snprintf(buf, sizeof(buf), "[孤胆称雄] %s 败于 %s 之手，场中尚余 %u 人。",
                     victimName.c_str(), killerName.c_str(), m_aliveCount);
        }
        else if (!notify)
        {
            snprintf(buf, sizeof(buf), "[孤胆称雄] %s 抽身离局，场中尚余 %u 人。",
                     victimName.c_str(), m_aliveCount);
        }
        else
        {
            snprintf(buf, sizeof(buf), "[孤胆称雄] %s 未能脱出险境，场中尚余 %u 人。",
                     victimName.c_str(), m_aliveCount);
        }
        BroadcastToAll(buf);
    }

    // Send battle report mail before returning the player
    if (!it->second.bot)
        SendBattleReport(guid, it->second, survivalSec);

    // Fill BR loot on the corpse — applies to all death types (PvP, zone damage, etc.).
    // If the player is still on the BR map, create the corpse first if needed.
    // If they already left (released spirit), ObjectAccessor finds them globally.
    if (player && !player->IsAlive() && !player->GetCorpse())
        player->BuildPlayerRepop();

    if (Player* lp = player ? player : sObjectAccessor.FindPlayer(guid))
    {
        if (Corpse* corpse = lp->GetCorpse())
        {
            corpse->loot.FillLoot(BR_CORPSE_LOOT_REF_ID, LootTemplates_Reference, lp, true);
            corpse->loot.gold       = urand(100, 1000);
            corpse->loot.m_personal = true;
            corpse->lootForBody     = true;
            corpse->SetShowLootableToFriendly(true);
            corpse->SetFlag(CORPSE_FIELD_DYNAMIC_FLAGS, CORPSE_DYNFLAG_LOOTABLE);
            corpse->ForceValuesUpdateAtIndex(CORPSE_FIELD_DYNAMIC_FLAGS);
            // The 1.12 client may not re-evaluate corpse sparkle from a VALUES update
            // alone. Re-send a creation packet to observers that already have the corpse
            // in their visible list so the sparkle appears immediately.
            if (Map* corpseMap = corpse->GetMap())
            {
                for (auto const& kv : m_players)
                {
                    if (Player* observer = corpseMap->GetPlayer(kv.first))
                        if (observer->IsInVisibleList(corpse))
                            corpse->SendCreateUpdateToPlayer(observer);
                }
            }
        }
    }

    // Capture log context while the player is still in the BR map.
    if (player && player->GetSession())
    {
        it->second.logName = player->GetName();
        it->second.logIp   = player->GetSession()->GetRemoteAddress();
        it->second.logZone = player->GetZoneId();
        it->second.logMap  = player->GetMapId();
    }

    // MVP: return player immediately (no spectating)
    if (player)
    {
        ReturnPlayer(player, it->second);
        it->second.outsideZone = false;
    }

    // Win condition: last survivor, or all real players eliminated (no point continuing bot-only)
    bool noRealPlayersAlive = true;
    ObjectGuid winnerGuid;
    bool winnerIsBot = false;
    for (auto const& jt : m_players)
    {
        if (!jt.second.alive)
            continue;
        if (!jt.second.bot)
            noRealPlayersAlive = false;
        if (!winnerGuid)
        {
            winnerGuid = jt.first;
            winnerIsBot = jt.second.bot;
        }
    }

    if (m_aliveCount <= 1 || noRealPlayersAlive)
    {
        if (m_aliveCount == 1 && winnerGuid && !winnerIsBot)
        {
            if (Player* winner = map ? map->GetPlayer(winnerGuid) : nullptr)
                ChatHandler(winner).PSendSysMessage("[孤胆称雄] 群雄皆寂，唯你执剑而立。此局魁首，江湖记名。");
        }

        Finish();
    }
}

void BattleRoyale::Finish()
{
    // Guard against double-call (can happen if two players die in the same Update frame)
    if (m_status == BattleRoyaleStatus::FINISHED || m_status == BattleRoyaleStatus::CANCELLED)
        return;

    // Assign rank 1 to the last survivor, capture log context, and send battle report
    uint32 const finishSurvivalSec = m_runningTime / 1000;
    Map* map = m_host ? m_host->GetBgMap() : nullptr;
    for (auto it = m_players.begin(); it != m_players.end(); ++it)
    {
        if (!it->second.alive)
            continue;
        it->second.placementRank = 1;
        if (map)
        {
            if (Player* p = map->GetPlayer(it->first))
            {
                p->CastSpell(p, BR_WINNER_CELEBRATION_SPELL_ID, true);
                if (p->GetSession())
                {
                    it->second.logName = p->GetName();
                    it->second.logIp   = p->GetSession()->GetRemoteAddress();
                    it->second.logZone = p->GetZoneId();
                    it->second.logMap  = p->GetMapId();
                }
            }
        }
        if (!it->second.bot)
            SendBattleReport(it->first, it->second, finishSurvivalSec);
    }

    // Award season scores and write per-match logs for all real players.
    // Build a guid->survivalSec map from m_ranks (eliminated players) plus the winner.
    std::map<ObjectGuid, uint32> survivalMap;
    for (auto const& r : m_ranks)
        survivalMap[r.guid] = r.survivalSec;
    // Winner survival time is finishSurvivalSec
    for (auto const& kv : m_players)
        if (kv.second.alive)
            survivalMap[kv.first] = finishSurvivalSec;

    for (auto const& kv : m_players)
    {
        if (!kv.second.bot)
        {
            auto survIt = survivalMap.find(kv.first);
            uint32 const survSec = (survIt != survivalMap.end()) ? survIt->second : 0;
            AwardSeasonScore(kv.first, kv.second.placementRank, kv.second.killCount,
                             m_totalCount, survSec,
                             kv.second.logName, kv.second.logIp,
                             kv.second.logZone, kv.second.logMap);
        }
    }

    m_status      = BattleRoyaleStatus::FINISHED;
    m_finishTimer = BR_FINISH_DELAY_MS;
    m_zone.Cleanup(m_host ? m_host->GetBgMap() : nullptr);
    BroadcastToAll("[孤胆称雄] 尘埃落定，论剑已终。十息之后，各归来处。");

    sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "[BattleRoyale] Instance %u finished. Total players: %u",
             m_host ? m_host->GetInstanceID() : 0u, m_totalCount);
}

void BattleRoyale::Cancel()
{
    m_status = BattleRoyaleStatus::CANCELLED;
    m_zone.Cleanup(m_host ? m_host->GetBgMap() : nullptr);

    Map* map = m_host ? m_host->GetBgMap() : nullptr;
    for (auto it = m_players.begin(); it != m_players.end(); ++it)
    {
        Player* player = map ? map->GetPlayer(it->first) : nullptr;
        if (player)
        {
            player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_IMMUNE_TO_NPC);
            ReturnPlayer(player, it->second);
        }
    }
}

void BattleRoyale::ReturnPlayer(Player* player, BattleRoyalePlayer const& brPlayer)
{
    // Remove from instance map immediately so the player can re-queue via NPC
    // without waiting for the instance to fully cancel.
    sBattleRoyaleMgr.RemovePlayerFromInstance(player->GetObjectGuid());

    player->SendMirrorTimerStop(MirrorTimer::FATIGUE);

    if (player->IsTaxiFlying())
    {
        player->GetMotionMaster()->MovementExpired();
        player->GetTaxi().ClearTaxiDestinations();
    }

    if (player->IsHovering())
    {
        player->SetHover(false);
        player->SetHoverReal(false);
    }

    CleanupBRItems(player);

    // BuildPlayerRepop() in Eliminate() sets the player to DEAD (ghost) so the corpse
    // lands on the BR map. Resurrect here before teleporting so the player arrives at
    // their original location alive, not as a ghost.
    if (!player->IsAlive())
        player->ResurrectPlayer(1.0f);

    player->SetFFAPvP(brPlayer.savedFFAPvP);
    player->SetBGTeam(TEAM_NONE);
    player->SetBattleGroundId(0, BATTLEGROUND_TYPE_NONE);
    player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_IMMUNE_TO_NPC);

    if (brPlayer.bot)
    {
        // Bots are removed from the world when the session is cleaned up
        if (PlayerBotEntry* entry = player->GetSession() ? player->GetSession()->GetBot() : nullptr)
            entry->requestRemoval = true;
        return;
    }

    WorldLocation const& pos = brPlayer.savedPosition;
    if (player->TeleportTo(pos.mapId, pos.x, pos.y, pos.z, pos.o))
        sBattleRoyaleMgr.ClearPendingRestore(player->GetObjectGuid());
    // If TeleportTo fails the pending restore record is kept so the player can
    // be recovered on next login.
}

void BattleRoyale::BroadcastPhaseChange(uint32 phase)
{
    uint32 totalPhases = m_tmpl ? uint32(m_tmpl->phases.size()) : 0;
    bool isFinal = (totalPhases > 0 && phase == totalPhases - 1 &&
                    m_tmpl->phases[phase].durationMs == 0);

    char buf[128];
    if (isFinal)
    {
        snprintf(buf, sizeof(buf), "[孤胆称雄] 最后一重险境已成，胜负只在眼前。圈外每秒伤害 %.0f%%。",
                 m_zone.GetCurrentDamagePercent());
    }
    else
    {
        snprintf(buf, sizeof(buf), "[孤胆称雄] 天地渐狭，第 %u 重险境已至，圈外每秒伤害 %.0f%%。",
                 phase + 1, m_zone.GetCurrentDamagePercent());
    }
    BroadcastToAll(buf);
}

void BattleRoyale::CleanupBRItems(Player* player)
{
    for (uint32 entry : BR_ITEM_ENTRIES)
        player->DestroyItemCount(entry, 200, true, false);
}

/*static*/ void BattleRoyale::AwardSeasonScore(ObjectGuid playerGuid, uint32 placementRank,
                                                uint32 killCount, uint32 totalPlayers,
                                                uint32 survivalSec,
                                                std::string const& name, std::string const& ip,
                                                uint32 zone, uint32 mapId)
{
    uint32 placementPts = 0;
    if      (placementRank == 1) placementPts = BR_SCORE_RANK1;
    else if (placementRank == 2) placementPts = BR_SCORE_RANK2;
    else if (placementRank == 3) placementPts = BR_SCORE_RANK3;

    uint32 const totalPts = placementPts + killCount * BR_SCORE_PER_KILL;
    uint32 const isWin    = (placementRank == 1) ? 1u : 0u;
    uint32 const guid     = playerGuid.GetCounter();
    std::string safeName  = name;
    std::string safeIp    = ip;
    CharacterDatabase.escape_string(safeName);
    CharacterDatabase.escape_string(safeIp);

    // Accumulate season score
    CharacterDatabase.PExecute(
        "INSERT INTO `battle_royale_season_score` "
        "  (`guid`, `season_points`, `total_matches`, `total_wins`, `total_kills`) "
        "VALUES (%u, %u, 1, %u, %u) "
        "ON DUPLICATE KEY UPDATE "
        "  `season_points` = `season_points` + %u, "
        "  `total_matches` = `total_matches` + 1, "
        "  `total_wins`    = `total_wins`    + %u, "
        "  `total_kills`   = `total_kills`   + %u",
        guid, totalPts, isWin, killCount,
        totalPts, isWin, killCount);

    // Append per-match log entry (mirrors character_log_pvpkill layout)
    CharacterDatabase.PExecute(
        "INSERT INTO `character_log_battle_royale` "
        "  (`guid`, `name`, `placement`, `total_players`, `kill_count`, "
        "   `survival_sec`, `score_earned`, `zone`, `map`, `ip`) "
        "VALUES (%u, '%s', %u, %u, %u, %u, %u, %u, %u, '%s')",
        guid, safeName.c_str(),
        placementRank, totalPlayers, killCount, survivalSec, totalPts,
        zone, mapId, safeIp.c_str());
}

void BattleRoyale::SendBattleReport(ObjectGuid playerGuid, BattleRoyalePlayer const& brPlayer, uint32 survivalSec) const
{
    uint32 const rank  = brPlayer.placementRank;
    uint32 const kills = brPlayer.killCount;
    uint32 const total = m_totalCount;
    uint32 const mm    = survivalSec / 60;
    uint32 const ss    = survivalSec % 60;

    uint32 placementPts = 0;
    if      (rank == 1) placementPts = BR_SCORE_RANK1;
    else if (rank == 2) placementPts = BR_SCORE_RANK2;
    else if (rank == 3) placementPts = BR_SCORE_RANK3;
    uint32 const totalPts = placementPts + kills * BR_SCORE_PER_KILL;

    char const* closing = rank == 1
        ? "你是最后执剑而立之人。此战之后，江湖留名。"
        : "胜负一时，江湖尚远。下一次风起，仍可再赴此局。";

    char body[768];
    snprintf(body, sizeof(body),
             "孤胆称雄战报\n\n"
             "此番论剑已经收场，你的名字已入战册。\n\n"
             "最终名次：第 %u 名 / 共 %u 人\n"
             "击倒对手：%u 人\n"
             "存活时间：%02u:%02u\n"
             "本局积分：+%u 分（名次 +%u，击杀 +%u）\n\n"
             "%s",
             rank, total, kills, mm, ss,
             totalPts, placementPts, kills * BR_SCORE_PER_KILL,
             closing);

    const char* subject = (rank == 1) ? "「孤胆称雄」魁首战报" : "「孤胆称雄」论剑战报";

    MailDraft(subject, std::string(body))
        .SendMailTo(MailReceiver(playerGuid),
                    MailSender(MAIL_NORMAL, uint32(0), MAIL_STATIONERY_DEFAULT));
}

void BattleRoyale::BroadcastToAll(std::string const& msg)
{
    Map* map = m_host ? m_host->GetBgMap() : nullptr;
    if (!map)
        return;
    for (auto it = m_players.begin(); it != m_players.end(); ++it)
    {
        Player* player = map->GetPlayer(it->first);
        if (player)
            ChatHandler(player).PSendSysMessage("%s", msg.c_str());
    }
}
