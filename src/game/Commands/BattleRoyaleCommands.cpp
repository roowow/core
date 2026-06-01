#include "Common.h"
#include "Chat/Chat.h"
#include "Language.h"
#include "Player.h"
#include "BattleRoyale/BattleRoyaleMgr.h"
#include "BattleRoyale/BattleRoyale.h"

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

    SendSysMessage("========== BR 对局信息 ==========");
    PSendSysMessage("实例 ID: %-6u  状态: %s  运行: %02u:%02u",
                    instanceId, statusStr, mins, secs);
    PSendSysMessage("存活: %u/%u  待加入机器人: %u",
                    br->GetAliveCount(), br->GetTotalCount(), br->GetPendingBotCount());
    PSendSysMessage("毒圈: 阶段 %u  半径: %.1f 码  伤害: %u/秒",
                    zone.GetPhase(), zone.GetCurrentRadius(), zone.GetCurrentDamage());
    PSendSysMessage("圆心: (%.0f, %.0f)  队列等待: %u 人",
                    zone.GetCenterX(), zone.GetCenterY(), sBattleRoyaleMgr.GetQueueSize());

    SendSysMessage("----- 参与者 -----");
    auto const& players = br->GetPlayers();
    for (auto it = players.begin(); it != players.end(); ++it)
    {
        BattleRoyalePlayer const& p = it->second;
        char const* botTag  = p.bot        ? "[机器人]" : "";
        char const* zoneTag = p.outsideZone ? " 圈外"   : "";

        if (p.alive)
        {
            Player* target = player->GetMap() ? player->GetMap()->GetPlayer(it->first) : nullptr;
            char const* name = target ? target->GetName() : "(离线)";
            PSendSysMessage("  [存活] %s%s%s", name, botTag, zoneTag);
        }
        else
        {
            Player* target = sObjectMgr.GetPlayer(it->first);
            char const* name = target ? target->GetName() : "(已离开)";
            PSendSysMessage("  [第%2u] %s%s", p.placementRank, name, botTag);
        }
    }
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
