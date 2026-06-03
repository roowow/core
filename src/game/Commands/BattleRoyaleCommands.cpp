#include "Common.h"
#include "Chat/Chat.h"
#include "Language.h"
#include "Player.h"
#include "BattleRoyale/BattleRoyaleMgr.h"
#include "BattleRoyale/BattleRoyale.h"
#include "BattleRoyale/BattleRoyaleTemplate.h"
#include "Battlegrounds/BattleGroundBR.h"
#include "Database/DatabaseEnv.h"
#include "ObjectMgr.h"

// .br start   — force start immediately with queued players
bool ChatHandler::HandleBRStartCommand(char* /*args*/)
{
    if (sBattleRoyaleMgr.GetQueueSize() == 0)
    {
        SendSysMessage("[BR] 队列为空，无法开始。");
        return true;
    }
    sBattleRoyaleMgr.ForceStartNow();
    SendSysMessage("[BR] 已强制开始对局。");
    return true;
}

// .br join    — GM joins the queue
bool ChatHandler::HandleBRJoinCommand(char* /*args*/)
{
    Player* player = m_session->GetPlayer();
    std::string err;
    if (sBattleRoyaleMgr.EnqueuePlayer(player, err))
        PSendSysMessage("[BR] 已加入队列（%u 人）。", sBattleRoyaleMgr.GetQueueSize());
    else
        PSendSysMessage("[BR] 无法加入：%s", err.c_str());
    return true;
}

// .br status  — show queue count and active instances
bool ChatHandler::HandleBRStatusCommand(char* /*args*/)
{
    PSendSysMessage("[BR] 队列人数：%u。", sBattleRoyaleMgr.GetQueueSize());
    return true;
}

// .br info  — detailed info about the BR instance the GM is currently in
bool ChatHandler::HandleBRInfoCommand(char* /*args*/)
{
    Player* player = m_session->GetPlayer();
    BattleRoyale* br = sBattleRoyaleMgr.GetInstanceForPlayer(player->GetObjectGuid());
    if (!br)
    {
        PSendSysMessage("[BR] 你不在对局中。队列人数：%u。", sBattleRoyaleMgr.GetQueueSize());
        return true;
    }

    BattleGroundBR* host = br->GetHost();
    uint32 instanceId = host ? host->GetInstanceID() : 0;

    static char const* const statusNames[] = {
        "等待", "倒计时", "空降", "准备期", "运行中", "已结束", "已取消"
    };
    uint8 statusIdx = uint8(br->GetStatus());
    char const* statusStr = statusIdx < 7 ? statusNames[statusIdx] : "未知";

    uint32 elapsed = br->GetRunningTimeSecs();
    uint32 mins = elapsed / 60, secs = elapsed % 60;

    BattleRoyaleZone const& zone = br->GetZone();

    PSendSysMessage("[BR] 实例 %-6u  状态: %s  运行: %02u:%02u",
                    instanceId, statusStr, mins, secs);
    PSendSysMessage("[BR] 存活: %u/%u  待加入机器人: %u  队列: %u",
                    br->GetAliveCount(), br->GetTotalCount(),
                    br->GetPendingBotCount(), sBattleRoyaleMgr.GetQueueSize());
    PSendSysMessage("[BR] 毒圈: 阶段 %u  半径: %.1f 码  伤害: %.1f%%/秒  圆心: (%.0f, %.0f)",
                    zone.GetPhase(), zone.GetCurrentRadius(), zone.GetCurrentDamagePercent(),
                    zone.GetCenterX(), zone.GetCenterY());
    return true;
}

// .br zone <radius>  — force set zone radius in current player's BR instance
bool ChatHandler::HandleBRZoneCommand(char* args)
{
    Player* player = m_session->GetPlayer();
    BattleRoyale* br = sBattleRoyaleMgr.GetInstanceForPlayer(player->GetObjectGuid());
    if (!br)
    {
        SendSysMessage("[BR] 你不在对局中。");
        return true;
    }

    float radius = 0.0f;
    if (args && *args)
        radius = float(atof(args));

    if (radius <= 0.0f)
    {
        PSendSysMessage("[BR] 当前毒圈半径：%.1f，阶段：%u。", br->GetZone().GetCurrentRadius(), br->GetZone().GetPhase());
        return true;
    }

    br->ForceSetRadius(radius);
    PSendSysMessage("[BR] 毒圈半径已设为 %.1f。", radius);
    return true;
}

// .br phase <n>  — force zone to phase N in current player's BR instance
bool ChatHandler::HandleBRPhaseCommand(char* args)
{
    Player* player = m_session->GetPlayer();
    BattleRoyale* br = sBattleRoyaleMgr.GetInstanceForPlayer(player->GetObjectGuid());
    if (!br)
    {
        SendSysMessage("[BR] 你不在对局中。");
        return true;
    }

    if (!args || !*args)
    {
        PSendSysMessage("[BR] 当前阶段：%u，当前半径：%.1f。", br->GetZone().GetPhase(), br->GetZone().GetCurrentRadius());
        return true;
    }

    uint32 phase = uint32(atoi(args));
    br->ForceSetPhase(phase);
    PSendSysMessage("[BR] 已切换到阶段 %u。", phase);
    return true;
}

// .br spawn add  — record GM's current position as a player spawn point
bool ChatHandler::HandleBRSpawnAddCommand(char* /*args*/)
{
    Player* player = m_session->GetPlayer();

    if (player->GetMapId() != 529)
    {
        SendSysMessage("[BR] 此命令只能在阿拉希盆地（地图 529）内使用。");
        return true;
    }

    float x = player->GetPositionX();
    float y = player->GetPositionY();
    float z = player->GetPositionZ();
    float o = player->GetOrientation();

    WorldDatabase.PExecute(
        "INSERT INTO `battle_royale_spawn_point` "
        "(`template_id`, `position_x`, `position_y`, `position_z`, `orientation`) "
        "VALUES (%u, %f, %f, %f, %f)",
        1u, x, y, z, o);

    sBattleRoyaleMgr.LoadSpawnPoints();

    PSendSysMessage("[BR] 已记录出生点 (%.2f, %.2f, %.2f)，当前共 %u / %u 个。",
                    x, y, z,
                    uint32(GetABTemplate().spawnPoints.size()),
                    GetABTemplate().maxPlayers);
    return true;
}

// .br spawn list  — list all recorded spawn points
bool ChatHandler::HandleBRSpawnListCommand(char* /*args*/)
{
    BattleRoyaleTemplate const& tmpl = GetABTemplate();
    uint32 count = uint32(tmpl.spawnPoints.size());
    PSendSysMessage("[BR] 出生点共 %u 个（需要 %u 个）：", count, tmpl.maxPlayers);
    for (uint32 i = 0; i < count; ++i)
    {
        BRSpawnPoint const& pt = tmpl.spawnPoints[i];
        PSendSysMessage("[BR]  #%-2u  (%.2f, %.2f, %.2f)", i + 1, pt.x, pt.y, pt.z);
    }
    if (count == 0)
        SendSysMessage("[BR] 暂无记录。用 .br spawn add 在地面上添加。");
    return true;
}

// .br chest add  — record GM's current position as a common chest spawn point
// Must be used standing on the ground inside Arathi Basin (mapId 529).
bool ChatHandler::HandleBRChestAddCommand(char* /*args*/)
{
    Player* player = m_session->GetPlayer();

    if (player->GetMapId() != 529)
    {
        SendSysMessage("[BR] 此命令只能在阿拉希盆地（地图 529）内使用。");
        return true;
    }

    float x = player->GetPositionX();
    float y = player->GetPositionY();
    float z = player->GetPositionZ();
    float o = player->GetOrientation();
    uint32 const templateId = 1;

    WorldDatabase.PExecute(
        "INSERT INTO `battle_royale_chest_point` "
        "(`template_id`, `chest_type`, `position_x`, `position_y`, `position_z`, `orientation`) "
        "VALUES (%u, 0, %f, %f, %f, %f)",
        templateId, x, y, z, o);

    sBattleRoyaleMgr.LoadChestPoints();

    PSendSysMessage("[BR] 已记录普通箱位置 (%.2f, %.2f, %.2f)，当前共 %u 个。",
                    x, y, z, uint32(GetABTemplate().commonChestPoints.size()));
    return true;
}

// .br chest list  — list all recorded common chest positions
bool ChatHandler::HandleBRChestListCommand(char* /*args*/)
{
    BattleRoyaleTemplate const& tmpl = GetABTemplate();
    uint32 count = uint32(tmpl.commonChestPoints.size());
    PSendSysMessage("[BR] 普通箱位置共 %u 个：", count);
    for (uint32 i = 0; i < count; ++i)
    {
        BRSpawnPoint const& pt = tmpl.commonChestPoints[i];
        PSendSysMessage("[BR]  #%-2u  (%.2f, %.2f, %.2f)", i + 1, pt.x, pt.y, pt.z);
    }
    if (count == 0)
        SendSysMessage("[BR] 暂无记录。用 .br chest add 在地面上添加。");
    return true;
}

// .br cancel  — cancel the BR instance the GM is in
bool ChatHandler::HandleBRCancelCommand(char* /*args*/)
{
    Player* player = m_session->GetPlayer();
    BattleRoyale* br = sBattleRoyaleMgr.GetInstanceForPlayer(player->GetObjectGuid());
    if (!br)
    {
        SendSysMessage("[BR] 你不在对局中。");
        return true;
    }
    br->Cancel();
    SendSysMessage("[BR] 对局已取消。");
    return true;
}
