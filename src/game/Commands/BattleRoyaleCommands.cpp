#include "Common.h"
#include "Chat/Chat.h"
#include "Language.h"
#include "Player.h"
#include "BattleRoyale/BattleRoyaleMgr.h"
#include "BattleRoyale/BattleRoyale.h"
#include "BattleRoyale/BattleRoyaleTemplate.h"
#include "Battlegrounds/BattleGroundBR.h"
#include "Database/DatabaseEnv.h"
#include "World.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace
{
BattleRoyaleTemplate const* ResolveBattleRoyaleTemplateArg(char const* arg)
{
    if (!arg || !*arg)
        return nullptr;

    std::string token(arg);
    size_t const firstSpace = token.find_first_of(" \t\r\n");
    if (firstSpace != std::string::npos)
        token.resize(firstSpace);

    std::transform(token.begin(), token.end(), token.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });

    if (token == "ab" || token == "arathi")
        return &GetABTemplate();
    if (token == "av" || token == "alterac")
        return &GetAVTemplate();
    if (token == "ac" || token == "azshara" || token == "crater")
        return &GetAzsharaCraterTemplate();

    char* end = nullptr;
    uint32 const numeric = uint32(std::strtoul(token.c_str(), &end, 10));
    if (!end || *end)
        return nullptr;

    for (BattleRoyaleTemplate* tmpl : GetAllBRTemplates())
        if (tmpl->id == numeric || tmpl->mapId == numeric)
            return tmpl;

    return nullptr;
}
}

// .br enable / .br disable  — open or close the BR queue
bool ChatHandler::HandleBREnableCommand(char* /*args*/)
{
    sBattleRoyaleMgr.SetEnabled(true);
    sWorld.SendGlobalText("[孤胆称雄] 江湖再起风云，论剑帖已张贴，速往令使处，赴一场生死豪局！", nullptr);
    return true;
}

bool ChatHandler::HandleBRDisableCommand(char* /*args*/)
{
    sBattleRoyaleMgr.SetEnabled(false);
    sWorld.SendGlobalText("[孤胆称雄] 风波暂歇，论剑帖已收，江湖路远，来日方长。", nullptr);
    return true;
}

// .br start [templateId|mapId|ab|av|ac] — force start immediately with queued players
bool ChatHandler::HandleBRStartCommand(char* args)
{
    if (sBattleRoyaleMgr.GetQueueSize() == 0)
    {
        SendSysMessage("[孤胆称雄] 候战席无人，猎场暂不能开启。");
        return true;
    }

    uint32 templateId = 0;
    BattleRoyaleTemplate const* requestedTemplate = nullptr;
    if (args)
    {
        while (*args == ' ')
            ++args;

        if (*args)
        {
            requestedTemplate = ResolveBattleRoyaleTemplateArg(args);
            if (!requestedTemplate)
            {
                SendSysMessage("[孤胆称雄] 未识别的模板。可用：1/ab/529，2/av/30。3/ac/azshara/37 当前暂未开放。");
                return true;
            }
            templateId = requestedTemplate->id;
        }
    }

    std::string error;
    if (!sBattleRoyaleMgr.ForceStartNow(templateId, &error))
    {
        PSendSysMessage("[孤胆称雄] 开局失败：%s", error.c_str());
        return true;
    }

    if (requestedTemplate)
        PSendSysMessage("[孤胆称雄] 已强制敲响开局号角，指定模板 %u（map %u）。",
                        requestedTemplate->id, requestedTemplate->mapId);
    else
        SendSysMessage("[孤胆称雄] 已强制敲响开局号角。");
    return true;
}

// .br join    — GM joins the queue
bool ChatHandler::HandleBRJoinCommand(char* /*args*/)
{
    Player* player = m_session->GetPlayer();
    std::string err;
    if (sBattleRoyaleMgr.EnqueuePlayer(player, err))
        PSendSysMessage("[孤胆称雄] 报名确认，候战席当前 %u 人。", sBattleRoyaleMgr.GetQueueSize());
    else
        PSendSysMessage("[孤胆称雄] 暂不能入场：%s", err.c_str());
    return true;
}

// .br status  — show queue count and active instances
bool ChatHandler::HandleBRStatusCommand(char* /*args*/)
{
    PSendSysMessage("[孤胆称雄] 当前候战人数：%u。", sBattleRoyaleMgr.GetQueueSize());
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
        "等待", "倒计时", "空降", "运行中", "已结束", "已取消"
    };
    uint8 statusIdx = uint8(br->GetStatus());
    char const* statusStr = statusIdx < 6 ? statusNames[statusIdx] : "未知";

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

// .br spawn add  — record GM's current position as a player spawn point
bool ChatHandler::HandleBRSpawnAddCommand(char* /*args*/)
{
    Player* player = m_session->GetPlayer();
    uint32 const mapId = player->GetMapId();

    // Find the template that matches the GM's current map
    BattleRoyaleTemplate* tmpl = nullptr;
    for (BattleRoyaleTemplate* t : GetAllBRTemplates())
    {
        if (t->mapId == mapId)
        {
            tmpl = t;
            break;
        }
    }

    if (!tmpl)
    {
        PSendSysMessage("[BR] 当前地图（%u）没有对应的 BR 模板，无法录制出生点。", mapId);
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
        tmpl->id, x, y, z, o);

    sBattleRoyaleMgr.LoadSpawnPoints();

    PSendSysMessage("[BR] 已记录出生点 (%.2f, %.2f, %.2f)（模板 %u），当前共 %u / %u 个。",
                    x, y, z, tmpl->id,
                    uint32(tmpl->spawnPoints.size()),
                    tmpl->maxPlayers);
    return true;
}

// .br spawn list  — list recorded spawn points for the GM's current map template
bool ChatHandler::HandleBRSpawnListCommand(char* /*args*/)
{
    Player* player = m_session->GetPlayer();
    uint32 const mapId = player->GetMapId();

    BattleRoyaleTemplate* tmpl = nullptr;
    for (BattleRoyaleTemplate* t : GetAllBRTemplates())
    {
        if (t->mapId == mapId)
        {
            tmpl = t;
            break;
        }
    }

    if (!tmpl)
    {
        // Not on a BR map — show summary for all templates
        for (BattleRoyaleTemplate* t : GetAllBRTemplates())
            PSendSysMessage("[BR] 模板 %u (map %u): %u / %u 个出生点",
                            t->id, t->mapId, uint32(t->spawnPoints.size()), t->maxPlayers);
        return true;
    }

    uint32 count = uint32(tmpl->spawnPoints.size());
    PSendSysMessage("[BR] 模板 %u 出生点共 %u 个（需要 %u 个）：",
                    tmpl->id, count, tmpl->maxPlayers);
    for (uint32 i = 0; i < count; ++i)
    {
        BRSpawnPoint const& pt = tmpl->spawnPoints[i];
        PSendSysMessage("[BR]  #%-2u  (%.2f, %.2f, %.2f)", i + 1, pt.x, pt.y, pt.z);
    }
    if (count == 0)
        SendSysMessage("[BR] 暂无记录。用 .br spawn add 在地面上添加。");
    return true;
}

// .br list  — list all players in the BR instance the GM is in
bool ChatHandler::HandleBRListCommand(char* /*args*/)
{
    Player* player = m_session->GetPlayer();
    BattleRoyale* br = sBattleRoyaleMgr.GetInstanceForPlayer(player->GetObjectGuid());
    if (!br)
    {
        SendSysMessage("[BR] 你不在对局中。");
        return true;
    }

    BattleGroundBR* host = br->GetHost();
    Map* map = host ? host->GetBgMap() : nullptr;

    bool const allianceBroken = br->IsAllianceBroken();

    PSendSysMessage("[BR] 存活 %u / %u  联盟: %s",
                    br->GetAliveCount(), br->GetTotalCount(),
                    allianceBroken ? "已破裂" : "阶段1");

    auto const& players = br->GetPlayers();
    for (auto const& kv : players)
    {
        BattleRoyalePlayer const& bp = kv.second;
        if (bp.bot)
            continue;

        char const* tag = bp.isGM ? "[GM]" : "[玩]";
        char const* aliveStr = bp.alive ? "存活" : "阵亡";

        std::string name;
        if (map)
        {
            Player* p = map->GetPlayer(kv.first);
            name = p ? p->GetName() : "(离线)";
        }
        else
            name = "(无地图)";

        PSendSysMessage("[BR] %s %-16s  %s  击杀:%u",
                        tag, name.c_str(), aliveStr, bp.killCount);
    }
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
