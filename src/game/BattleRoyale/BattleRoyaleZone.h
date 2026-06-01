#ifndef MANGOS_BATTLEROYALEZONE_H
#define MANGOS_BATTLEROYALEZONE_H

#include "BattleRoyaleTemplate.h"
#include "BattleRoyalePlayer.h"
#include "ObjectGuid.h"

#include <map>
#include <vector>
#include <cmath>

class Map;
class Player;

class BattleRoyaleZone
{
public:
    void Init(BattleRoyaleTemplate const* tmpl);
    void Update(uint32 diff, std::map<ObjectGuid, BattleRoyalePlayer>& players, Map* map);
    void Cleanup(Map* map);

    bool  IsInsideZone(float x, float y) const;
    float GetCurrentRadius()        const { return m_currentRadius; }
    float GetCurrentDamagePercent() const;
    uint32 GetPhase()               const { return m_phase; }

    // Spawn the initial marker ring (called when entering PREPARING state)
    void RefreshMarkers(Map* map) { UpdateMarkers(map); }
    // Drain pending spawn queue one batch at a time (call each tick during PREPARING)
    void DrainSpawnQueue(Map* map) { ProcessSpawnBatch(map); }

    float GetCenterX() const { return m_centerX; }
    float GetCenterY() const { return m_centerY; }

    // GM helpers
    void ForcePhase(uint32 phase);
    void ForceRadius(float r) { m_currentRadius = r; m_lastMarkerRadius = -1.0f; m_radiusForced = true; }

private:
    void  ApplyZoneDamage(Player* player);
    void  SendZoneWarning(Player* player);
    void  StartNextPhase();
    void  QueueRing(Map* map, float radius);   // computes positions into m_pendingSpawns
    void  ProcessSpawnBatch(Map* map);          // creates up to SPAWN_BATCH_SIZE GOs per tick
    void  UpdateMarkers(Map* map);
    void  RemoveMarkers(Map* map);

    // pending marker spawn queue
    struct PendingMarker { float x, y, z, angle; };
    std::vector<PendingMarker> m_pendingSpawns;
    uint32 m_pendingSpawnIdx = 0;

    float   m_centerX       = 0.0f;
    float   m_centerY       = 0.0f;
    float   m_currentRadius = 600.0f;
    float   m_startRadius   = 600.0f;
    float   m_targetRadius  = 600.0f;
    uint32  m_phase         = 0;
    uint32  m_phaseTimer    = 0;
    uint32  m_damageTimer   = 1000;
    bool    m_radiusForced  = false;

    float   m_lastMarkerRadius   = -1.0f;  // -1 = markers not yet spawned
    std::vector<ObjectGuid> m_markerGuids;

    BattleRoyaleTemplate const* m_tmpl = nullptr;
};

#endif // MANGOS_BATTLEROYALEZONE_H
