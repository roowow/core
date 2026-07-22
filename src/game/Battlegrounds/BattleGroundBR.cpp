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
    // 开放世界模式下没有 BattleGroundMap（m_map 一直是空）。~BattleGround()
    // 基类析构函数里会调用 sBattleGroundMgr.RemoveBattleGround(GetInstanceID(), ...)
    // 来清理注册表，但执行到基类那一层时虚函数表已经回退成基类版本，GetInstanceID()
    // 拿到的是 m_map==nullptr 对应的 0，跟创建时用真实合成ID（m_openWorldInstanceId）
    // 注册的条目对不上——不主动处理的话，注册表里会留下一条永远清理不掉的僵尸记录。
    // 这里趁虚函数表还没回退，先用正确的ID清理一次；基类析构函数里那次用0做的
    // RemoveBattleGround 就变成了对一个本来就不存在的0号条目做erase，是安全的空操作。
    if (m_openWorldMap)
        sBattleGroundMgr.RemoveBattleGround(GetInstanceID(), GetTypeID());

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
        m_owner->OnPlayerDied(victim->GetObjectGuid(), killer ? killer->GetObjectGuid() : ObjectGuid());
    // No honor reward in BR -- kills are rewarded via season score instead.
}
