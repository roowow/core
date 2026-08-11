// Corridor-checkpoint override for AQ40 tunnel guards (Anubisath Warder / Obsidian Nullifier).
// Design rationale: src/game/0-Design/CorridorGuard.md
//
// Wraps CreatureEventAI (instead of replacing it with ScriptedAI) so the existing
// phase-based ability rotation driven by creature_ai_events keeps working unchanged.
// Only affects the guid(s) listed in s_pilotGuards below; every other spawn of the
// same entry falls through to default CreatureEventAI behavior untouched.

#include "scriptPCH.h"
#include "CreatureEventAI.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace
{
    struct CorridorPoint { float x, y, z; };

    // Recorded via `.battlebot path`, guid=88064 (Anubisath Warder, entry 15311).
    std::vector<CorridorPoint> const s_path_88064 =
    {
        { -8902.0244f, 1856.1764f, -22.0328f }, { -8895.4980f, 1865.7058f, -22.1789f },
        { -8888.5947f, 1874.9630f, -22.8971f }, { -8882.5459f, 1882.5786f, -23.4431f },
        { -8877.6182f, 1888.7832f, -24.1981f }, { -8873.4258f, 1893.7389f, -24.7721f },
        { -8868.4502f, 1899.9052f, -26.5454f }, { -8853.6621f, 1917.6510f, -28.5720f },
        { -8846.2676f, 1926.5239f, -30.0553f }, { -8838.8730f, 1935.3969f, -29.8381f },
        { -8831.4795f, 1944.2698f, -29.2488f }, { -8824.0850f, 1953.1427f, -28.2337f },
        { -8816.6699f, 1961.9969f, -26.7746f }, { -8808.3477f, 1970.0027f, -24.8143f },
        { -8799.8594f, 1977.8351f, -22.2326f }, { -8790.9590f, 1985.1946f, -19.2119f },
        { -8781.9121f, 1992.3745f, -16.1168f }, { -8772.4551f, 1999.0038f, -12.8485f },
        { -8762.7363f, 2005.2354f, -9.9109f },  { -8752.3594f, 2010.2942f, -6.7250f },
        { -8741.6680f, 2014.6571f, -3.3205f },  { -8730.7979f, 2018.5558f, -0.1678f },
        { -8723.0957f, 2020.4058f, 1.4426f },   { -8715.9385f, 2021.4114f, 2.6158f },
        { -8704.4043f, 2021.9645f, 4.1523f },   { -8697.5459f, 2022.0972f, 5.2023f },
        { -8686.5303f, 2020.3956f, 8.4834f },   { -8680.1562f, 2017.8625f, 10.2160f },
        { -8669.7695f, 2012.8391f, 11.9957f },  { -8659.5107f, 2007.5334f, 12.9518f },
        { -8649.5840f, 2001.6484f, 13.0165f },  { -8640.0078f, 1995.2144f, 13.2277f },
        { -8630.1318f, 1986.8920f, 17.0047f },  { -8624.6279f, 1977.1768f, 18.6707f },
        { -8623.1719f, 1972.3336f, 19.1612f },  { -8621.0508f, 1960.9927f, 20.4930f },
        { -8618.4668f, 1952.7280f, 22.1337f },  { -8616.2646f, 1947.3917f, 23.4740f },
        { -8608.6152f, 1935.7327f, 27.4761f },  { -8601.2471f, 1934.1238f, 29.4580f },
        { -8591.7275f, 1929.4634f, 31.1760f },  { -8585.1768f, 1931.4518f, 32.3213f },
        { -8568.5928f, 1935.8694f, 35.1316f },  { -8559.1104f, 1942.4640f, 37.5969f },
        { -8550.0928f, 1949.6676f, 40.5643f },  { -8541.5205f, 1957.4016f, 43.7750f },
        { -8533.6592f, 1969.2509f, 48.0796f },  { -8524.8516f, 1970.9230f, 51.4189f },
        { -8511.1572f, 1982.8348f, 55.9356f },  { -8504.4932f, 1981.5723f, 57.1907f },
        { -8503.2520f, 1975.2280f, 57.8129f },  { -8497.5088f, 1969.2538f, 60.3407f },
        { -8494.7549f, 1957.0662f, 64.5823f },  { -8501.2031f, 1941.5968f, 67.0080f },
        { -8503.5527f, 1936.7040f, 67.8821f },  { -8514.4932f, 1933.0016f, 70.5178f },
        { -8525.4336f, 1929.2992f, 75.4295f },  { -8536.3740f, 1925.5969f, 80.1938f },
        { -8547.3154f, 1921.8945f, 84.3102f },  { -8558.3398f, 1918.4719f, 87.7685f },
        { -8572.3574f, 1917.5459f, 93.5647f },  { -8583.8955f, 1917.4241f, 97.6000f },
        { -8604.4727f, 1916.9061f, 104.3279f }, { -8611.4971f, 1915.3743f, 107.9103f },
    };

    // guid -> recorded corridor path. Any spawn of entry 15311/15312 not listed here
    // gets no override at all (falls through to plain CreatureEventAI behavior).
    std::unordered_map<uint32, std::vector<CorridorPoint> const*> const s_pilotGuards =
    {
        { 88064, &s_path_88064 },
    };

    float const CORRIDOR_TOLERANCE = 15.0f;   // yards; needs field-testing
    float const LEASH_ON_PATH = 999.0f;       // effectively "don't leash" while target tracks the corridor
    float const LEASH_DEFAULT = 120.0f;       // matches creature_template.leash_range (see Fix.sql)
    float const CHAIN_ATTACK_RADIUS = 40.0f;  // yards; needs field-testing

    float DistanceToSegment(float px, float py, float pz, float ax, float ay, float az, float bx, float by, float bz)
    {
        float abx = bx - ax, aby = by - ay, abz = bz - az;
        float apx = px - ax, apy = py - ay, apz = pz - az;
        float abLenSq = abx * abx + aby * aby + abz * abz;
        float t = abLenSq > 0.0f ? (apx * abx + apy * aby + apz * abz) / abLenSq : 0.0f;
        t = std::max(0.0f, std::min(1.0f, t));
        float cx = ax + t * abx, cy = ay + t * aby, cz = az + t * abz;
        float dx = px - cx, dy = py - cy, dz = pz - cz;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    float DistanceToPath(std::vector<CorridorPoint> const& path, float x, float y, float z)
    {
        float best = std::numeric_limits<float>::max();
        for (size_t i = 0; i + 1 < path.size(); ++i)
        {
            float d = DistanceToSegment(x, y, z, path[i].x, path[i].y, path[i].z, path[i + 1].x, path[i + 1].y, path[i + 1].z);
            if (d < best)
                best = d;
        }
        return best;
    }
}

struct aq40_corridor_guardAI : public CreatureEventAI
{
    std::vector<CorridorPoint> const* m_pPath;

    explicit aq40_corridor_guardAI(Creature* c) : CreatureEventAI(c)
    {
        auto itr = s_pilotGuards.find(c->GetGUIDLow());
        m_pPath = itr != s_pilotGuards.end() ? itr->second : nullptr;
    }

    void UpdateAI(uint32 const diff) override
    {
        CreatureEventAI::UpdateAI(diff);

        if (!m_pPath)
            return;

        if (Unit* pVictim = m_creature->GetVictim())
        {
            float dist = DistanceToPath(*m_pPath, pVictim->GetPositionX(), pVictim->GetPositionY(), pVictim->GetPositionZ());
            m_creature->SetLeashDistance(dist <= CORRIDOR_TOLERANCE ? LEASH_ON_PATH : LEASH_DEFAULT);
        }
    }

    void EnterEvadeMode() override
    {
        // Only a "ran out of living targets" evade (GetVictim()==nullptr) gets to look for
        // another nearby player before giving up the corridor; a leash-triggered evade
        // (still has a living victim, just wandered off the recorded path) always falls
        // through to the normal reset below, so the anti-pet-drag fix stays intact.
        if (m_pPath && !m_creature->GetVictim())
        {
            Player* pNext = nullptr;
            MaNGOS::NearestAlivePlayerCheck check(m_creature, CHAIN_ATTACK_RADIUS);
            MaNGOS::PlayerSearcher<MaNGOS::NearestAlivePlayerCheck> searcher(pNext, check);
            Cell::VisitWorldObjects(m_creature, searcher, CHAIN_ATTACK_RADIUS);

            if (pNext)
            {
                AttackStart(pNext);
                return;
            }
        }

        CreatureEventAI::EnterEvadeMode();
    }
};

CreatureAI* GetAI_aq40_corridor_guard(Creature* pCreature)
{
    return new aq40_corridor_guardAI(pCreature);
}

void AddSC_aq40_corridor_guard()
{
    Script* newscript = new Script;
    newscript->Name = "aq40_corridor_guard";
    newscript->GetAI = &GetAI_aq40_corridor_guard;
    newscript->RegisterSelf();
}
