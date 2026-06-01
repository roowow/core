#include "BattleRoyaleZone.h"

#include "Player.h"
#include "Map.h"
#include "GameObject.h"
#include "GameObjectDefines.h"
#include "GridMap.h"
#include "MirrorTimer.h"
#include "Chat.h"
#include "SharedDefines.h"
#include "Log.h"

#include <cmath>

static uint32 const ZONE_MARKER_ENTRY            = 900101;
static float  const ZONE_MARKER_SPACING          = 50.0f;  // yards between markers
static uint32 const ZONE_MARKER_MIN              = 24;
static uint32 const ZONE_MARKER_MAX              = 96;
static float  const ZONE_MARKER_CHANGE_THRESHOLD = 30.0f;  // force refresh if radius shifts this much
static float  const ZONE_MARKER_INNER_OFFSET     = 20.0f;  // inner warning ring distance from boundary

static uint32 const ZONE_WARN_INTERVAL_MS = 5000;

void BattleRoyaleZone::Init(BattleRoyaleTemplate const* tmpl)
{
    m_tmpl          = tmpl;
    m_centerX       = tmpl->centerX;
    m_centerY       = tmpl->centerY;
    m_phase         = 0;
    m_damageTimer   = 1000;

    if (!tmpl->phases.empty())
    {
        m_currentRadius = tmpl->phases[0].startRadius;
        m_startRadius   = tmpl->phases[0].startRadius;
        m_targetRadius  = tmpl->phases[0].endRadius;
        m_phaseTimer    = tmpl->phases[0].durationMs;
    }
}

bool BattleRoyaleZone::IsInsideZone(float x, float y) const
{
    float dx = x - m_centerX;
    float dy = y - m_centerY;
    return (dx * dx + dy * dy) <= (m_currentRadius * m_currentRadius);
}

float BattleRoyaleZone::GetCurrentDamagePercent() const
{
    if (!m_tmpl || m_phase >= uint32(m_tmpl->phases.size()))
        return 0.0f;
    return m_tmpl->phases[m_phase].damagePercent;
}

void BattleRoyaleZone::Update(uint32 diff, std::map<ObjectGuid, BattleRoyalePlayer>& players, Map* map)
{
    if (!m_tmpl || m_tmpl->phases.empty())
        return;

    // Smooth radius shrink
    BRZonePhase const& phase = m_tmpl->phases[m_phase];
    if (!m_radiusForced && phase.durationMs > 0 && m_phaseTimer > 0)
    {
        float elapsed  = float(phase.durationMs - m_phaseTimer);
        float progress = elapsed / float(phase.durationMs);
        m_currentRadius = phase.startRadius + (phase.endRadius - phase.startRadius) * progress;
    }

    // Advance phase
    if (phase.durationMs > 0)
    {
        if (m_phaseTimer <= diff)
            StartNextPhase();
        else
            m_phaseTimer -= diff;
    }

    // Rebuild markers only when radius shifts by the threshold or on first call.
    // No periodic rebuild — static GOs flicker when deleted+recreated unnecessarily.
    if (map)
    {
        bool radiusChanged = std::fabs(m_currentRadius - m_lastMarkerRadius) >= ZONE_MARKER_CHANGE_THRESHOLD;
        if (m_lastMarkerRadius < 0.0f || radiusChanged)
            UpdateMarkers(map);
    }

    // Damage tick
    bool doDamage = false;
    if (m_damageTimer <= diff)
    {
        doDamage      = true;
        m_damageTimer = 1000;
    }
    else
    {
        m_damageTimer -= diff;
    }

    for (auto it = players.begin(); it != players.end(); ++it)
    {
        BattleRoyalePlayer& brPlayer = it->second;
        if (!brPlayer.alive)
            continue;

        Player* player = map->GetPlayer(it->first);
        if (!player || !player->IsAlive())
            continue;

        bool outside = !IsInsideZone(player->GetPositionX(), player->GetPositionY());

        if (outside && !brPlayer.outsideZone)
        {
            brPlayer.outsideZone   = true;
            brPlayer.zoneWarnTimer = ZONE_WARN_INTERVAL_MS;
            player->SendMirrorTimerStart(MirrorTimer::FATIGUE, 10000, 10000, -1);
        }
        else if (!outside && brPlayer.outsideZone)
        {
            brPlayer.outsideZone   = false;
            brPlayer.zoneWarnTimer = 0;
            player->SendMirrorTimerStop(MirrorTimer::FATIGUE);
        }

        if (!brPlayer.outsideZone)
            continue;

        if (doDamage)
        {
            ApplyZoneDamage(player);
            // Refresh the mirror timer bar on each damage tick
            player->SendMirrorTimerStart(MirrorTimer::FATIGUE, 10000, 10000, -1);
        }

        if (brPlayer.zoneWarnTimer <= diff)
        {
            brPlayer.zoneWarnTimer = ZONE_WARN_INTERVAL_MS;
            SendZoneWarning(player);
        }
        else
        {
            brPlayer.zoneWarnTimer -= diff;
        }
    }
}

void BattleRoyaleZone::Cleanup(Map* map)
{
    RemoveMarkers(map);
}

void BattleRoyaleZone::RemoveMarkers(Map* map)
{
    if (!map)
    {
        m_markerGuids.clear();
        return;
    }
    for (ObjectGuid const& guid : m_markerGuids)
    {
        if (GameObject* go = map->GetGameObject(guid))
        {
            go->SetRespawnTime(0);
            go->Delete();
        }
    }
    m_markerGuids.clear();
    m_lastMarkerRadius = -1.0f;
}

void BattleRoyaleZone::SpawnRing(Map* map, float radius)
{
    if (radius <= 0.0f)
        return;

    uint32 count = uint32(std::ceil(2.0f * float(M_PI_F) * radius / ZONE_MARKER_SPACING));
    if (count < ZONE_MARKER_MIN) count = ZONE_MARKER_MIN;
    if (count > ZONE_MARKER_MAX) count = ZONE_MARKER_MAX;

    float angleStep = 2.0f * float(M_PI_F) / float(count);
    for (uint32 i = 0; i < count; ++i)
    {
        float angle = angleStep * float(i);
        float x = m_centerX + radius * std::cos(angle);
        float y = m_centerY + radius * std::sin(angle);
        float z = map->GetHeight(x, y, MAX_HEIGHT, false, MAX_HEIGHT);
        if (z <= INVALID_HEIGHT)
            continue;

        GameObject* go = new GameObject;
        if (!go->Create(map->GenerateLocalLowGuid(HIGHGUID_GAMEOBJECT), ZONE_MARKER_ENTRY, map,
                        x, y, z + 0.1f, angle, 0.0f, 0.0f, 0.0f, 0.0f, GO_ANIMPROGRESS_DEFAULT, GO_STATE_READY))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[BattleRoyaleZone] Failed to create zone marker (entry %u) at %.1f,%.1f,%.1f",
                     ZONE_MARKER_ENTRY, x, y, z);
            delete go;
            continue;
        }
        go->SetRespawnTime(0);
        map->Add(go);
        m_markerGuids.push_back(go->GetObjectGuid());
    }
}

void BattleRoyaleZone::UpdateMarkers(Map* map)
{
    RemoveMarkers(map);
    if (!map)
        return;

    SpawnRing(map, m_currentRadius);                              // outer ring: zone boundary
    if (m_currentRadius > ZONE_MARKER_INNER_OFFSET * 2.0f)
        SpawnRing(map, m_currentRadius - ZONE_MARKER_INNER_OFFSET); // inner warning ring

    m_lastMarkerRadius = m_currentRadius;
}

void BattleRoyaleZone::ApplyZoneDamage(Player* player)
{
    float pct = GetCurrentDamagePercent();
    if (pct <= 0.0f || !player->IsAlive())
        return;
    uint32 dmg = uint32(float(player->GetMaxHealth()) * pct / 100.0f);
    if (dmg < 1)
        dmg = 1;
    // DAMAGE_DROWNING: no school → bypasses fire resistance and all other mitigation
    player->EnvironmentalDamage(DAMAGE_DROWNING, dmg);
}

void BattleRoyaleZone::SendZoneWarning(Player* player)
{
    float dx = m_centerX - player->GetPositionX();
    float dy = m_centerY - player->GetPositionY();
    float angle = std::atan2(dy, dx);

    static char const* const dirs[8] = { "东", "东北", "北", "西北", "西", "西南", "南", "东南" };
    int idx = int(std::fmod(angle + 2.5f * float(M_PI_F), 2.0f * float(M_PI_F)) / (float(M_PI_F) / 4.0f)) % 8;
    if (idx < 0) idx += 8;

    float dist = std::sqrt(dx * dx + dy * dy) - m_currentRadius;
    if (dist < 0.0f) dist = 0.0f;

    ChatHandler(player).PSendSysMessage("[安全区] 你在圈外！安全区在你的【%s】，距边界 %.0f 码。", dirs[idx], dist);
}

void BattleRoyaleZone::ForcePhase(uint32 phase)
{
    if (!m_tmpl || phase >= uint32(m_tmpl->phases.size()))
        return;
    m_phase         = phase;
    m_startRadius   = m_tmpl->phases[phase].startRadius;
    m_targetRadius  = m_tmpl->phases[phase].endRadius;
    m_currentRadius = m_startRadius;
    m_phaseTimer    = m_tmpl->phases[phase].durationMs;
    m_radiusForced  = false;
    m_lastMarkerRadius = -1.0f;
}

void BattleRoyaleZone::StartNextPhase()
{
    if (!m_tmpl)
        return;

    uint32 next = m_phase + 1;
    if (next >= uint32(m_tmpl->phases.size()))
    {
        m_currentRadius = m_tmpl->phases.back().endRadius;
        m_phaseTimer    = 0;
        return;
    }

    m_phase         = next;
    m_startRadius   = m_tmpl->phases[next].startRadius;
    m_targetRadius  = m_tmpl->phases[next].endRadius;
    if (!m_radiusForced)
        m_currentRadius = m_startRadius;
    m_phaseTimer    = m_tmpl->phases[next].durationMs;
}
