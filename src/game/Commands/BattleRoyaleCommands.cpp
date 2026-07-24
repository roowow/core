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
#include "OO/WebChatMgr.h"

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
    sWebChatMgr.WriteBroadcast("[孤胆称雄] 江湖再起风云，论剑帖已张贴，速往令使处，赴一场生死豪局！");
    sWebChatMgr.NotifyWorldBroadcastToJianJia("[孤胆称雄] 江湖再起风云，论剑帖已张贴，速往令使处，赴一场生死豪局！");
    return true;
}

bool ChatHandler::HandleBRDisableCommand(char* /*args*/)
{
    sBattleRoyaleMgr.SetEnabled(false);
    sWorld.SendGlobalText("[孤胆称雄] 风波暂歇，论剑帖已收，江湖路远，来日方长。", nullptr);
    sWebChatMgr.WriteBroadcast("[孤胆称雄] 风波暂歇，论剑帖已收，江湖路远，来日方长。");
    sWebChatMgr.NotifyWorldBroadcastToJianJia("[孤胆称雄] 风波暂歇，论剑帖已收，江湖路远，来日方长。");
    return true;
}

// .br start [templateId|mapId|ab|av|ac] — force the *next* match to use a specific
// template instead of a random pick. Does not bypass the minimum-player/countdown
// gate: the match still only actually starts once enough real players are queued
// and the normal countdown (with its 60s-remaining bot-preload lock) reaches zero.
bool ChatHandler::HandleBRStartCommand(char* args)
{
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
                SendSysMessage("[孤胆称雄] 未识别的模板。可用：1/ab/529，2/av/30，3/ac/azshara/37，4（海加尔山，map 1），5（GM岛，map 1——跟海加尔山同地图，只能用模板id选，不能用map参数）。GM岛出生点未录制，暂未启用。");
                return true;
            }
            templateId = requestedTemplate->id;
        }
    }

    std::string error;
    if (!sBattleRoyaleMgr.ForceStartNow(templateId, &error))
    {
        PSendSysMessage("[孤胆称雄] 指定失败：%s", error.c_str());
        return true;
    }

    if (requestedTemplate)
        PSendSysMessage("[孤胆称雄] 已指定下一局使用模板 %u（map %u），候战人数满足后自动开局。",
                        requestedTemplate->id, requestedTemplate->mapId);
    else
        SendSysMessage("[孤胆称雄] 已取消模板指定，下一局恢复随机选图。");
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

// .br spawn add [templateId|mapId|ab/av/ac/...]  — record GM's current position as a
// player spawn point. With no argument, auto-detects the template by picking whichever
// template sharing the GM's current mapId has the closest centerX/Y to the GM's position
// — NOT just "first template with a matching mapId" (that used to silently misfile 海加尔山
// and GM岛 spawn points into each other, since both are mapId=1; see BattleRoyale.md「GM岛」).
bool ChatHandler::HandleBRSpawnAddCommand(char* args)
{
    Player* player = m_session->GetPlayer();
    uint32 const mapId = player->GetMapId();

    while (args && *args == ' ')
        ++args;

    BattleRoyaleTemplate const* tmpl = nullptr;
    if (args && *args)
    {
        tmpl = ResolveBattleRoyaleTemplateArg(args);
        if (!tmpl)
        {
            SendSysMessage("[BR] 未识别的模板参数。");
            return true;
        }
        if (tmpl->mapId != mapId)
        {
            PSendSysMessage("[BR] 模板 %u 对应地图 %u，跟你当前所在地图（%u）不一致，请确认。", tmpl->id, tmpl->mapId, mapId);
            return true;
        }
    }
    else
    {
        // Auto-detect: among templates sharing this mapId, pick the one whose
        // center is closest to the GM's current position.
        float const px = player->GetPositionX();
        float const py = player->GetPositionY();
        float bestDistSq = 0.0f;
        for (BattleRoyaleTemplate* t : GetAllBRTemplates())
        {
            if (t->mapId != mapId)
                continue;
            float const dx = px - t->centerX;
            float const dy = py - t->centerY;
            float const distSq = dx * dx + dy * dy;
            if (!tmpl || distSq < bestDistSq)
            {
                tmpl = t;
                bestDistSq = distSq;
            }
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

    // Auto-detect like .br spawn add: among templates sharing this mapId, pick the
    // one whose center is closest to the GM's current position (not just "first
    // match" — see HandleBRSpawnAddCommand's comment for why that was wrong).
    float const px = player->GetPositionX();
    float const py = player->GetPositionY();
    BattleRoyaleTemplate* tmpl = nullptr;
    float bestDistSq = 0.0f;
    for (BattleRoyaleTemplate* t : GetAllBRTemplates())
    {
        if (t->mapId != mapId)
            continue;
        float const dx = px - t->centerX;
        float const dy = py - t->centerY;
        float const distSq = dx * dx + dy * dy;
        if (!tmpl || distSq < bestDistSq)
        {
            tmpl = t;
            bestDistSq = distSq;
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
