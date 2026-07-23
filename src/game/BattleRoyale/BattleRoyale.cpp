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
#include "OO/OOMgr.h"

#include "Mail.h"
#include "Corpse.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "Database/DatabaseEnv.h"

#include "Utilities/Random.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

static uint32 const BR_FINISH_DELAY_MS           = 10000;
static float  const BR_LANDING_CORRECTION_DISTANCE = 5.0f;

// 每隔这么久给所有存活玩家(含bot)脚下打一发照明弹，防止潜行者一直蹲草不动
static uint32 const BR_FLARE_INTERVAL_MS = 60000;
static uint32 const BR_FLARE_SPELL_ID    = 1543; // Flare
// Orbit path ID is now per-template (BattleRoyaleTemplate::orbitPathId).
static float  const BR_TWO_PI                    = 6.2831853071795864769f;

// Reference loot table entry for BR corpse drops (reference_loot_template.entry).
static uint32 const BR_CORPSE_LOOT_REF_ID = 9001;

// Season score awarded per placement and per kill.
static uint32 const BR_SCORE_RANK1    = 5;
static uint32 const BR_SCORE_RANK2    = 3;
static uint32 const BR_SCORE_RANK3    = 1;
static uint32 const BR_SCORE_PER_KILL = 1;
static uint32 const BR_WINNER_CELEBRATION_SPELL_ID = 27571;

// 夺冠音效：按获胜者阵营播放原版战场胜利号角，只发给获胜者本人（PlayDirectSound 传target就是单播）
static uint32 const BR_WINNER_SOUND_ALLIANCE = 8455; // PVPVictoryAlliance
static uint32 const BR_WINNER_SOUND_HORDE    = 8454; // PVPVictoryHorde

// 物理系职业（战士/盗贼/猎人）击杀奖励池：AP/力量/敏捷/攻速向
static uint32 const BR_KILL_BUFFS_PHYSICAL[] =
{
    10938, // Power Word: Fortitude  (通用)
    9885,  // Mark of the Wild       (通用)
    20217, // Blessing of Kings      (通用)
    16618, // Spirit of the Wind     (通用, +30% 移速)
    19838, // Blessing of Might      (+185 AP)
    16329, // Juju Might             (+40 AP)
    16323, // Juju Power             (+30 力量)
    16327, // Juju Guile             (+30 敏捷)
    16322, // Juju Flurry            (攻速提升)
    17013, // Agamaggan's Agility    (+10 敏捷)
    16612, // Agamaggan's Strength   (+10 力量)
};

// 法系职业（法师/术士/牧师）击杀奖励池：智力/精神向
static uint32 const BR_KILL_BUFFS_CASTER[] =
{
    10938, // Power Word: Fortitude  (通用)
    9885,  // Mark of the Wild       (通用)
    20217, // Blessing of Kings      (通用)
    16618, // Spirit of the Wind     (通用, +30% 移速)
    10157, // Arcane Intellect       (+31 智力)
    7764,  // Wisdom of Agamaggan    (+10 智力)
    10767, // Rising Spirit          (+25 精神)
};

// 混合职业（圣骑士/萨满/德鲁伊）：全量池
static uint32 const BR_KILL_BUFFS_HYBRID[] =
{
    10938, // Power Word: Fortitude
    9885,  // Mark of the Wild
    20217, // Blessing of Kings
    16618, // Spirit of the Wind
    19838, // Blessing of Might
    10157, // Arcane Intellect
    16329, // Juju Might
    16323, // Juju Power
    16327, // Juju Guile
    16322, // Juju Flurry
    17013, // Agamaggan's Agility
    16612, // Agamaggan's Strength
    7764,  // Wisdom of Agamaggan
    10767, // Rising Spirit
};

static bool GiveBRItemToBot(Player* bot, uint32 entry, uint32 count = 1)
{
    ItemPosCountVec dest;
    if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, count) == EQUIP_ERR_OK)
    {
        bot->StoreNewItem(dest, entry, true);
        return true;
    }
    return false;
}

static void ApplyBattleRoyaleKillRewardBuff(Player* killer)
{
    if (!killer || !killer->IsAlive())
        return;

    uint32 const* pool     = BR_KILL_BUFFS_HYBRID;
    uint32        poolSize = sizeof(BR_KILL_BUFFS_HYBRID) / sizeof(BR_KILL_BUFFS_HYBRID[0]);

    switch (killer->GetClass())
    {
        case CLASS_WARRIOR:
        case CLASS_ROGUE:
        case CLASS_HUNTER:
            pool     = BR_KILL_BUFFS_PHYSICAL;
            poolSize = sizeof(BR_KILL_BUFFS_PHYSICAL) / sizeof(BR_KILL_BUFFS_PHYSICAL[0]);
            break;
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        case CLASS_PRIEST:
            pool     = BR_KILL_BUFFS_CASTER;
            poolSize = sizeof(BR_KILL_BUFFS_CASTER) / sizeof(BR_KILL_BUFFS_CASTER[0]);
            break;
        default: // CLASS_PALADIN, CLASS_SHAMAN, CLASS_DRUID → 混合全量池
            break;
    }

    // 优先给还没有的 buff，避免重复；若全部已有则随机覆盖一个。
    uint32 available[14];
    uint32 availableCount = 0;
    for (uint32 i = 0; i < poolSize; ++i)
        if (!killer->HasAura(pool[i]))
            available[availableCount++] = pool[i];

    uint32 const spellId = availableCount > 0
        ? available[urand(0, availableCount - 1)]
        : pool[urand(0, poolSize - 1)];

    killer->CastSpell(killer, spellId, true);
}

static char const* PickBattleRoyaleLine(char const* const* lines, uint32 count)
{
    return lines[urand(0, count - 1)];
}

static float GetBattleRoyaleLandingZ(Map* map, BRSpawnPoint const& landing)
{
    if (!map)
        return landing.z;

    // Spawn points are GM-recorded on the intended floor. Searching from MAX_HEIGHT
    // can pick a higher outdoor/roof surface on layered maps, making bots float.
    float groundZ = map->GetHeight(landing.x, landing.y, landing.z + 2.0f, true, 8.0f);
    if (groundZ > INVALID_HEIGHT)
        return groundZ;

    groundZ = map->GetHeight(landing.x, landing.y, landing.z + 2.0f, false, 8.0f);
    if (groundZ > INVALID_HEIGHT)
        return groundZ;

    return landing.z;
}

// BR item entries — must match what is actually in item_template (DB).
// Used only for CleanupBRItems() when a player exits the match.
// 900214-900216, 900219, 900221 reserved for future items; not yet in DB, omitted here.
static uint32 const BR_ITEM_ENTRIES[] =
{
    900210, 900211, 900213, 900233, 900234,
    900217, 900218, 900220,
    900222, 900223, 900225, 900226,
    900227,
    900228, 900229, 900230, 900231, 900232
};


BattleRoyale::BattleRoyale(BattleRoyaleTemplate const* tmpl, BattleGroundBR* host)
    : m_status(BattleRoyaleStatus::DEPLOYING), m_tmpl(tmpl), m_host(host),
      m_landedCount(0),
      m_aliveCount(0), m_totalCount(0), m_finishTimer(0), m_runningTime(0), m_flareTimer(BR_FLARE_INTERVAL_MS)
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
    // GM账号（典型：.br join 混入观察）：不广播其加入/淘汰、不计入积分日志，见 BattleRoyalePlayer::isGM
    brPlayer.isGM             = !isBot && player->GetSession() && player->GetSession()->GetSecurity() > SEC_PLAYER;
    brPlayer.outsideZone      = false;
    brPlayer.zoneWarnTimer    = 0;
    brPlayer.placementRank    = 0;
    brPlayer.landingPoint     = landingPoint;
    brPlayer.deploymentPathId = deploymentPathId;
    brPlayer.orbitSlot        = m_totalCount; // 0-based join order; see m_orbitTotalSlots for the fixed denominator that spreads angles evenly
    // Bots already show a fictional name; only real players need an anonymous
    // stand-in so opponents can't identify them by name during the match (see
    // WorldSession::SendNameQueryOpcode). Reuses the same name pool/theme bots
    // use, keyed to this instance, so anonymized players and bots look consistent.
    if (!isBot)
        brPlayer.anonName = sOOMgr.GetBotName(m_host ? m_host->GetInstanceID() : 0, player->GetTeam() == ALLIANCE);
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
    Map* map = m_host ? m_host->GetHostMap() : nullptr;

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
            ReturnPendingPlayers(map);
        }

        // 每 BR_FLARE_INTERVAL_MS 给全体存活玩家(含bot)脚下打一发照明弹(1543)，逼出附近潜行/隐匿的人，
        // 避免有人一直蹲草不动苟到最后。
        bool fireFlare = false;
        if (m_flareTimer <= diff)
        {
            fireFlare = true;
            m_flareTimer = BR_FLARE_INTERVAL_MS;
        }
        else
            m_flareTimer -= diff;

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

                // 野外开放世界地图（OPEN_WORLD）没有天然的地形/地图边界墙，玩家理论上可以无限
                // 往外走，跟缩圈是两回事——缩圈是"圈外持续扣血"，这里是硬边界，越界直接淘汰。
                // 复用现有 Eliminate()/ReturnPendingPlayers() 流程，走跟"离开地图"完全一样的路径，
                // 连播报文案都共用 Eliminate() 里已有的"未能脱出险境"那组 hazardLines，无需新增文案。
                if (m_tmpl && m_tmpl->hostMode == BRMapHostMode::OPEN_WORLD)
                {
                    float const x = player->GetPositionX();
                    float const y = player->GetPositionY();
                    if (x < m_tmpl->openWorldMinX || x > m_tmpl->openWorldMaxX ||
                        y < m_tmpl->openWorldMinY || y > m_tmpl->openWorldMaxY)
                    {
                        toEliminate.push_back(it->first);
                        continue;
                    }
                }

                if (!player->IsFFAPvP())
                    player->SetFFAPvP(true);
                if (player->IsMounted())
                    player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
                if (fireFlare && !it->second.isGM)
                    player->CastSpell(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), BR_FLARE_SPELL_ID, true);
            }
            for (ObjectGuid const& guid : toEliminate)
                Eliminate(guid);
            ReturnPendingPlayers(map);
        }

        // Phase 2: break bot alliances when only 1 human player remains.
        if (!m_allianceBroken && GetAliveHumanCount() <= 1)
            m_allianceBroken = true;

        return;
    }

    if (m_status == BattleRoyaleStatus::FINISHED)
    {
        ReturnPendingPlayers(map);

        if (m_finishTimer <= diff)
        {
            if (map)
            {
                for (auto it = m_players.begin(); it != m_players.end(); ++it)
                {
                    Player* player = map->GetPlayer(it->first);
                    if (player)
                        ReturnPlayer(player, it->second);
                    else if (!it->second.bot)
                        sBattleRoyaleMgr.ClearPendingRestore(it->first);
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

uint32 BattleRoyale::GetAliveHumanCount() const
{
    uint32 count = 0;
    for (auto const& kv : m_players)
        if (kv.second.alive && !kv.second.bot)
            ++count;
    return count;
}

uint8 BattleRoyale::GetBotFaction(ObjectGuid guid) const
{
    auto it = m_botFactions.find(guid);
    return it != m_botFactions.end() ? it->second : 0;
}

void BattleRoyale::AssignBotFactions()
{
    std::vector<ObjectGuid> botGuids;
    botGuids.reserve(m_players.size());
    for (auto const& kv : m_players)
        if (kv.second.bot && kv.second.alive)
            botGuids.push_back(kv.first);

    if (botGuids.empty())
        return;

    // Shuffle so faction assignment is random, then distribute round-robin into 4 factions.
    std::shuffle(botGuids.begin(), botGuids.end(), std::default_random_engine(urand(0, 0xFFFFFF)));
    uint8 const numFactions = static_cast<uint8>(urand(3, 5));
    uint8 faction = 1;
    for (ObjectGuid const& guid : botGuids)
    {
        m_botFactions[guid] = faction;
        faction = (faction % numFactions) + 1;
    }
}

bool BattleRoyale::IsAlive(ObjectGuid guid) const
{
    auto it = m_players.find(guid);
    return it != m_players.end() && it->second.alive;
}

void BattleRoyale::ForceSetPhase(uint32 phase)
{
    m_zone.ForcePhase(phase);
    if (Map* map = m_host ? m_host->GetHostMap() : nullptr)
        m_zone.RefreshMarkers(map);
}

void BattleRoyale::ForceSetRadius(float radius)
{
    m_zone.ForceRadius(radius);
    if (Map* map = m_host ? m_host->GetHostMap() : nullptr)
        m_zone.RefreshMarkers(map);
}

// --- private ---

void BattleRoyale::ReturnPendingPlayers(Map* map)
{
    if (!map)
        return;

    for (auto it = m_players.begin(); it != m_players.end(); ++it)
    {
        if (!it->second.pendingReturn)
            continue;

        Player* player = map->GetPlayer(it->first);
        if (!player)
        {
            it->second.pendingReturn = false;
            sBattleRoyaleMgr.RemovePlayerFromInstance(it->first);
            continue;
        }

        it->second.pendingReturn = false;
        ReturnPlayer(player, it->second);
        it->second.outsideZone = false;
    }
}

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
            // Always teleport to the landing point so 1.14 clients (via proxy)
            // receive an explicit position anchor packet after the taxi ends.
            CompleteDeployment(player, brPlayer, !brPlayer.bot);
            continue;
        }

        // The initial deployment TeleportTo() (same-map "near" teleport) can be
        // deferred by Player::SetDelayedTeleportFlagIfCan() and not actually applied
        // yet — observed as the player never visibly teleporting to deploymentStart,
        // just taking off and slowly flying from wherever they really were. Force it
        // through immediately instead of waiting for the client round trip, so the
        // combined flight always starts from the correct staging point.
        if (player->IsBeingTeleportedNear())
            player->ExecuteTeleportNear();

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
        // the shared orbit (possibly several laps, see orbitLapCount) from a rotated
        // node before taking their own drop path.
        uint32 const orbitLaps = m_tmpl ? std::max(1u, m_tmpl->orbitLapCount) : 1;
        std::vector<TaxiPathNodeEntry> combined;
        combined.reserve(orbitNodes->size() * orbitLaps + dropIt->second.nodes.size() + 2);

        // Even fan-out: each player's join order (orbitSlot) gets its own fixed-width
        // slice of the circle. The denominator (totalSlots) is fixed once per match
        // (m_orbitTotalSlots, set in CreateInstance — normally maxPlayers, or the
        // real headcount if that's larger for an uncapRealPlayers template), not the
        // live join count, so slots assigned to early joiners don't shift as later
        // players/bots join. A random per-player hash used to be here, but with
        // ~30 entrants it visibly clustered.
        size_t const orbitCount = orbitNodes->size();
        uint32 const totalSlots = m_orbitTotalSlots ? m_orbitTotalSlots
                                : (m_tmpl ? std::max(1u, m_tmpl->maxPlayers) : 30);
        float const angle = BR_TWO_PI * float(brPlayer.orbitSlot % totalSlots) / float(totalSlots);
        size_t const orbitStart = size_t(angle / BR_TWO_PI * float(orbitCount) + 0.5f) % orbitCount;

        // Use the template's staging point instead of the player's live position.
        // A same-map ("near") teleport can be deferred (Player::SetDelayedTeleportFlagIfCan())
        // and not yet applied by the time this runs, so GetPosition* here could still read
        // the pre-teleport location — observed as the player never visibly teleporting,
        // just taking off and slowly flying from wherever they actually were.
        TaxiPathNodeEntry startNode = (*orbitNodes)[orbitStart];
        startNode.index = 0;
        startNode.mapid = player->GetMapId();
        startNode.x = m_tmpl ? m_tmpl->deploymentStart.x : player->GetPositionX();
        startNode.y = m_tmpl ? m_tmpl->deploymentStart.y : player->GetPositionY();
        startNode.z = m_tmpl ? m_tmpl->deploymentStart.z : player->GetPositionZ();
        startNode.delay = 0;
        combined.push_back(startNode);

        float const centerX = m_tmpl ? m_tmpl->GetOrbitCenterX() : (*orbitNodes)[orbitStart].x;
        float const centerY = m_tmpl ? m_tmpl->GetOrbitCenterY() : (*orbitNodes)[orbitStart].y;
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

        for (uint32 lap = 0; lap < orbitLaps; ++lap)
        {
            for (size_t i = 0; i < orbitCount; ++i)
            {
                TaxiPathNodeEntry node = (*orbitNodes)[(orbitStart + i) % orbitCount];
                node.index = uint32(combined.size());
                combined.push_back(node);
            }
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
            Map* bgMap = m_host ? m_host->GetHostMap() : nullptr;
            landZ = GetBattleRoyaleLandingZ(bgMap, landing);
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
        Map* bgMap = m_host ? m_host->GetHostMap() : nullptr;
        if (brPlayer.bot && bgMap)
            landZ = GetBattleRoyaleLandingZ(bgMap, landing);

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

    Map* map = m_host ? m_host->GetHostMap() : nullptr;

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

        if (it->second.bot)
        {
            GiveBRItemToBot(player, 900234, 5); // 大型治疗药水
            GiveBRItemToBot(player, 900218, 5); // 自由行动药水
            uint8 const cls = player->GetClass();
            if (cls == CLASS_MAGE   || cls == CLASS_PRIEST  || cls == CLASS_WARLOCK ||
                cls == CLASS_DRUID  || cls == CLASS_SHAMAN  || cls == CLASS_PALADIN)
                GiveBRItemToBot(player, 900233, 5); // 卓越法力药水
        }
    }

    BroadcastPhaseChange(0);

    AssignBotFactions();
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
    uint32 killerKillCount = 0;
    if (killerGuid && killerGuid != guid)
    {
        auto killerIt = m_players.find(killerGuid);
        if (killerIt != m_players.end())
        {
            ++killerIt->second.killCount;
            killerKillCount = killerIt->second.killCount;
            if (Map* map = m_host ? m_host->GetHostMap() : nullptr)
                ApplyBattleRoyaleKillRewardBuff(map->GetPlayer(killerGuid));
        }
    }

    Map* map = m_host ? m_host->GetHostMap() : nullptr;
    Player* player = map ? map->GetPlayer(guid) : nullptr;

    if (notify && player)
        ChatHandler(player).PSendSysMessage("[孤胆称雄] 此番江湖路止于此，最终名次第 %u。", it->second.placementRank);

    // Broadcast to survivors — 跳过GM观察者自己的淘汰播报，不暴露其在场
    if (!it->second.isGM)
    {
        char buf[384];
        std::string victimName = player ? player->GetName() : "一名试炼者";

        if (killerGuid && killerGuid != guid)
        {
            Player* killer = map ? map->GetPlayer(killerGuid) : nullptr;
            std::string killerName = killer ? killer->GetName() : "未知猎手";

            if (m_aliveCount <= 3)
            {
                static char const* const finalLines[] =
                {
                    "[孤胆称雄] %s 被 %s 斩落，最后几人已入生死局。场中尚余 %u 人。",
                    "[孤胆称雄] %s 止步于 %s 手下，江湖风声忽然安静。场中尚余 %u 人。",
                    "[孤胆称雄] %s 出局，%s 离魁首又近了一步。场中尚余 %u 人。"
                };
                snprintf(buf, sizeof(buf), PickBattleRoyaleLine(finalLines, sizeof(finalLines) / sizeof(finalLines[0])),
                         victimName.c_str(), killerName.c_str(), m_aliveCount);
            }
            else if (killerKillCount >= 3)
            {
                static char const* const streakLines[] =
                {
                    "[孤胆称雄] %s 被 %s 收作第 %u 个战果，场上众人该留神了。尚余 %u 人。",
                    "[孤胆称雄] %s 一败，%s 的剑上已记下 %u 道名姓。尚余 %u 人。",
                    "[孤胆称雄] %s 没能挡住 %s 的连胜势头，第 %u 人就此出局。尚余 %u 人。",
                    "[孤胆称雄] %s 倒下，%s 气势正盛，已连取 %u 人。尚余 %u 人。"
                };
                snprintf(buf, sizeof(buf), PickBattleRoyaleLine(streakLines, sizeof(streakLines) / sizeof(streakLines[0])),
                         victimName.c_str(), killerName.c_str(), killerKillCount, m_aliveCount);
            }
            else if (killerKillCount == 2)
            {
                static char const* const doubleLines[] =
                {
                    "[孤胆称雄] %s 倒在 %s 手下，%s 已连下两城。场中尚余 %u 人。",
                    "[孤胆称雄] %s 败走，%s 再添一胜，%s 手感正热。场中尚余 %u 人。",
                    "[孤胆称雄] %s 被 %s 请出论剑场，%s 的第二笔战绩落定。场中尚余 %u 人。"
                };
                snprintf(buf, sizeof(buf), PickBattleRoyaleLine(doubleLines, sizeof(doubleLines) / sizeof(doubleLines[0])),
                         victimName.c_str(), killerName.c_str(), killerName.c_str(), m_aliveCount);
            }
            else
            {
                static char const* const killLines[] =
                {
                    "[孤胆称雄] %s 与 %s 刀光一闪，胜负已分。场中尚余 %u 人。",
                    "[孤胆称雄] %s 被 %s 请出了江湖局，行囊可别忘了摸。场中尚余 %u 人。",
                    "[孤胆称雄] %s 方才露头，便被 %s 收了这一局。场中尚余 %u 人。",
                    "[孤胆称雄] %s 棋差一招，%s 再添一笔战绩。场中尚余 %u 人。",
                    "[孤胆称雄] %s 没能躲过 %s 的锋芒，江湖路暂告一段。场中尚余 %u 人。",
                    "[孤胆称雄] %s 与 %s 狭路相逢，今日笑到最后的是后者。场中尚余 %u 人。"
                };
                snprintf(buf, sizeof(buf), PickBattleRoyaleLine(killLines, sizeof(killLines) / sizeof(killLines[0])),
                         victimName.c_str(), killerName.c_str(), m_aliveCount);
            }
        }
        else if (!notify)
        {
            static char const* const leaveLines[] =
            {
                "[孤胆称雄] %s 收剑离席，此番江湖不再相见。场中尚余 %u 人。",
                "[孤胆称雄] %s 抽身离局，山高水远，下回再战。场中尚余 %u 人。",
                "[孤胆称雄] %s 忽然退场，众人只听见衣袂一响。场中尚余 %u 人。"
            };
            snprintf(buf, sizeof(buf), PickBattleRoyaleLine(leaveLines, sizeof(leaveLines) / sizeof(leaveLines[0])),
                     victimName.c_str(), m_aliveCount);
        }
        else
        {
            static char const* const hazardLines[] =
            {
                "[孤胆称雄] %s 未能脱出险境，被天地收了这一局。场中尚余 %u 人。",
                "[孤胆称雄] %s 走慢半步，圈外风雪已至。场中尚余 %u 人。",
                "[孤胆称雄] %s 误入死地，江湖册上又少一名。场中尚余 %u 人。"
            };
            snprintf(buf, sizeof(buf), PickBattleRoyaleLine(hazardLines, sizeof(hazardLines) / sizeof(hazardLines[0])),
                     victimName.c_str(), m_aliveCount);
        }
        BroadcastToAll(buf);
    }

    // Send battle report mail before returning the player（GM观察者跳过，避免收到一封宣称"本局积分+N"但
    // 实际从未写入 AwardSeasonScore 的战报邮件）
    if (!it->second.bot && !it->second.isGM)
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

    // Defer teleport/removal until the next BR update. BattleGroundBR::HandleKillPlayer()
    // runs inside Unit::Kill(), and moving the victim before Unit::Kill finishes leaves
    // core death cleanup operating on a unit that no longer has the BR map.
    if (player)
        it->second.pendingReturn = true;

    // Win condition: last survivor, or all real players eliminated (no point continuing bot-only)
    bool noRealPlayersAlive = true;
    for (auto const& jt : m_players)
    {
        if (!jt.second.alive)
            continue;
        if (!jt.second.bot)
            noRealPlayersAlive = false;
    }

    // 冠军播报统一交给 Finish() 结尾那条广播处理（发给本局所有真人参与者，不只是冠军自己），
    // 这里不再单独私聊冠军一句"唯你执剑而立"——不然冠军会连续收到两条内容重复的夺冠消息。
    if (m_aliveCount <= 1 || noRealPlayersAlive)
        Finish();
}

void BattleRoyale::Finish()
{
    // Guard against double-call (can happen if two players die in the same Update frame)
    if (m_status == BattleRoyaleStatus::FINISHED || m_status == BattleRoyaleStatus::CANCELLED)
        return;

    // Assign rank 1 to the last survivor, capture log context, and send battle report
    uint32 const finishSurvivalSec = m_runningTime / 1000;
    Map* map = m_host ? m_host->GetHostMap() : nullptr;
    std::string winnerName;
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
                p->PlayDirectSound(p->GetTeam() == ALLIANCE ? BR_WINNER_SOUND_ALLIANCE : BR_WINNER_SOUND_HORDE, p);
                winnerName = p->GetName();
                if (p->GetSession())
                {
                    it->second.logName = p->GetName();
                    it->second.logIp   = p->GetSession()->GetRemoteAddress();
                    it->second.logZone = p->GetZoneId();
                    it->second.logMap  = p->GetMapId();
                }
            }
        }
        if (!it->second.bot && !it->second.isGM)
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
        // GM观察者（kv.second.isGM）不计入赛季积分/对局日志，不参与排行榜
        if (!kv.second.bot && !kv.second.isGM)
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
    m_zone.Cleanup(m_host ? m_host->GetHostMap() : nullptr);

    // 播报本局冠军给所有参与这局的真人玩家（不含bot）。用sObjectMgr全服查找而不是
    // BroadcastToAll那种"只发给还在BR地图上的人"，因为更早出局的玩家这时候大多已经被
    // ReturnPlayer送回原位置、离开了BR地图，但同样应该收到"谁赢了"这条播报。
    if (!winnerName.empty())
    {
        char buf[192];
        snprintf(buf, sizeof(buf), "[孤胆称雄] 尘埃落定，%s 技压群雄，独占鳌头！论剑已终，十息之后，战场消散。", winnerName.c_str());
        for (auto const& kv : m_players)
        {
            if (kv.second.bot)
                continue;
            if (Player* p = sObjectMgr.GetPlayer(kv.first))
                ChatHandler(p).PSendSysMessage("%s", buf);
        }
    }
    else
        BroadcastToAll("[孤胆称雄] 尘埃落定，论剑已终。十息之后，各归来处。");
}

void BattleRoyale::Cancel()
{
    m_status = BattleRoyaleStatus::CANCELLED;
    m_zone.Cleanup(m_host ? m_host->GetHostMap() : nullptr);

    Map* map = m_host ? m_host->GetHostMap() : nullptr;
    for (auto it = m_players.begin(); it != m_players.end(); ++it)
    {
        Player* player = map ? map->GetPlayer(it->first) : nullptr;
        if (player)
        {
            player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_IMMUNE_TO_NPC);
            ReturnPlayer(player, it->second);
        }
        else if (!it->second.bot)
            sBattleRoyaleMgr.ClearPendingRestore(it->first);
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

    // Resurrecting only revives the player; the corpse GameObject (with BR loot on it)
    // is a separate object left behind on the map. For INSTANCED templates this was
    // never an issue — the whole match map unloads and takes every corpse with it —
    // but OPEN_WORLD (Hyjal) hosts on the persistent Kalimdor map, so an unlooted
    // corpse would otherwise sit there for up to the default 3-day resurrectable
    // timeout (Corpse::IsExpired). Delete it outright here instead of letting it
    // decay into bones, since the owner is never coming back to reclaim it.
    if (Corpse* corpse = player->GetCorpse())
    {
        sObjectAccessor.RemoveCorpse(corpse);
        corpse->DeleteFromDB();
        delete corpse;
    }

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
    // `season_points` is the spendable balance (shop purchases reduce it);
    // `season_points_earned` only ever grows and is what the leaderboard should rank on.
    CharacterDatabase.PExecute(
        "INSERT INTO `battle_royale_season_score` "
        "  (`guid`, `season_points`, `season_points_earned`, `total_matches`, `total_wins`, `total_kills`) "
        "VALUES (%u, %u, %u, 1, %u, %u) "
        "ON DUPLICATE KEY UPDATE "
        "  `season_points`        = `season_points`        + %u, "
        "  `season_points_earned` = `season_points_earned` + %u, "
        "  `total_matches`        = `total_matches`        + 1, "
        "  `total_wins`           = `total_wins`           + %u, "
        "  `total_kills`          = `total_kills`          + %u",
        guid, totalPts, totalPts, isWin, killCount,
        totalPts, totalPts, isWin, killCount);

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
    // 全服查找而不是只查BR地图上的人：玩家死亡淘汰后会被ReturnPlayer送回原位置、离开BR地图，
    // 但在这局真正结束（Finish/Cancel）之前，应该继续收到后续播报（谁又出局了、进入第几层圈
    // 之类），不能因为人已经不在地图上就收不到。
    for (auto it = m_players.begin(); it != m_players.end(); ++it)
    {
        if (Player* player = sObjectMgr.GetPlayer(it->first))
            ChatHandler(player).PSendSysMessage("%s", msg.c_str());
    }
}
