#include "BattleRoyaleZone.h"

#include "Player.h"
#include "Map.h"
#include "MirrorTimer.h"
#include "Chat.h"
#include "SharedDefines.h"

#include <cmath>

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

uint32 BattleRoyaleZone::GetCurrentDamage() const
{
    if (!m_tmpl || m_phase >= uint32(m_tmpl->phases.size()))
        return 0;
    return m_tmpl->phases[m_phase].damagePerSec;
}

void BattleRoyaleZone::Update(uint32 diff, std::map<ObjectGuid, BattleRoyalePlayer>& players, Map* map)
{
    if (!m_tmpl || m_tmpl->phases.empty())
        return;

    // Smooth radius shrink
    BRZonePhase const& phase = m_tmpl->phases[m_phase];
    if (phase.durationMs > 0 && m_phaseTimer > 0)
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

void BattleRoyaleZone::Cleanup(Map* /*map*/)
{
    // TODO: remove fire-pillar GameObjects here
}

void BattleRoyaleZone::ApplyZoneDamage(Player* player)
{
    uint32 dmg = GetCurrentDamage();
    if (!dmg || !player->IsAlive())
        return;
    player->DealDamage(player, dmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_FIRE, nullptr, false);
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
    m_currentRadius = m_startRadius;
    m_phaseTimer    = m_tmpl->phases[next].durationMs;
}
