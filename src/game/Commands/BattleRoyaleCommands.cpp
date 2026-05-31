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
