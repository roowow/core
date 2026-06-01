#include "BattleGroundBR.h"
#include "BattleRoyale/BattleRoyale.h"

#include "Player.h"
#include "BattleGroundMgr.h"
#include "Log.h"

BattleGroundBR::BattleGroundBR()
{
    SetTypeID(BATTLEGROUND_BR);
}

BattleGroundBR::~BattleGroundBR()
{
    // BattleRoyale instance is owned by BattleRoyaleMgr, not by us
    m_owner = nullptr;
}

void BattleGroundBR::Update(uint32 diff)
{
    // Do NOT call BattleGround::Update — it handles start/end via status which we don't use
    if (m_owner)
        m_owner->Update(diff);
}

void BattleGroundBR::AddPlayer(Player* player)
{
    // Do NOT call base class AddPlayer.
    // Base adds to faction BG groups which breaks FFA (same-faction players become friendly).
    // BR adds players via BattleRoyale::AddPlayer called from BattleRoyaleMgr.
    (void)player;
}

void BattleGroundBR::RemovePlayerAtLeave(ObjectGuid guid, bool /*transport*/, bool /*sendPacket*/)
{
    if (m_owner)
        m_owner->OnPlayerLeftMap(guid);
}

void BattleGroundBR::HandleKillPlayer(Player* victim, Player* killer)
{
    if (m_owner && victim)
        m_owner->OnPlayerDied(victim->GetObjectGuid());

    // Award the killer honor equal to one standard BG honorable kill.
    // Bots cannot use honor — only reward real players.
    if (killer && killer->GetSession() && !killer->GetSession()->GetBot())
    {
        uint32 honor = GetBonusHonorFromKill(1);
        if (honor)
            killer->GetHonorMgr().Add(honor, BONUS);
    }
}
