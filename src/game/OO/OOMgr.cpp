#include "Database/DatabaseEnv.h"
#include "World.h"
#include "Log.h"
#include "ProgressBar.h"
#include "Policies/SingletonImp.h"
#include "Util.h"
#include "IO/Filesystem/FileSystem.h"
#include "ObjectAccessor.h"

#include "OO/OOMgr.h"
#include "Player.h"
#include "BattleGround.h"
#include "WorldPacket.h"
#include "CreatureDefines.h"
#include "SQLStorages.h"
#include "WorldSession.h"

#include "Config/Config.h"
#include "Utilities/Random.h"
#include <fstream>
#include <iomanip>
#include <sstream>

INSTANTIATE_SINGLETON_1(OOMgr);

OOMgr::OOMgr()
{
    // _constInterval = sWorld.getConfig(CONFIG_UINT32_AUTOBROADCAST_INTERVAL);
    // _current = 0;
}

OOMgr::~OOMgr()
{
    // entries.clear();
}

void OOMgr::Load()
{
    // Delete all snowballs

    // PVP Text
    std::unique_ptr<QueryResult> presult(CharacterDatabase.Query("SELECT `Race`,`Class`,`Text` FROM `character_pvp_text` ORDER BY `character_pvp_text`.`ID` ASC"));
    uint32 pcount = 0;
    if (presult)
    {
         do
        {
            Field* fields   = presult->Fetch();
            PVPTexts[fields[0].GetInt32()][fields[1].GetInt32()].push_back(fields[2].GetString());
            ++pcount;
        }
        while (presult->NextRow());
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u OO PVP texts", pcount);

    // Bot Name Themes (file-based, per-faction)
    LoadBotNameThemes();

    // for (auto & element : GetPVPText(2, 2)) {
    //     printf("dd %s \n", element.c_str());
    // }

    // Bot Name
    // uint32 bindex = 0;
    // std::unique_ptr<QueryResult> bresult(CharacterDatabase.Query("SELECT name FROM `character_name` where name not in (SELECT Name from characters)"));
    // if (bresult)
    // {
    //      do
    //     {
    //         Field* fields   = bresult->Fetch();
    //         BattleBotNames[bindex] = fields[0].GetString();
    //         bindex++;
    //     }
    //     while (bresult->NextRow());
    // }

    /// Build Bank
    // SELECT `guild_id`, `guild_rank`, `withdraw_item`, `withdraw_cod` FROM `character_guild_bank`
    std::unique_ptr<QueryResult> gresult(CharacterDatabase.Query("SELECT guid, guild_id, vendor_id, guild_rank, withdraw_item, withdraw_cod, withdraw_cod_total, vendor_name FROM `character_guild_bank` ORDER BY `guild_id`, `weight` ASC"));
    if (gresult)
    {
        do
        {
            Field* fields = gresult->Fetch();

            OOGuildBank OOGuildBank;
            OOGuildBank.guid                = fields[0].GetUInt32();
            OOGuildBank.guild_id            = fields[1].GetUInt32();
            OOGuildBank.vendor_id           = fields[2].GetUInt32();
            OOGuildBank.guild_rank          = fields[3].GetUInt32();
            OOGuildBank.withdraw_item       = fields[4].GetUInt32();
            OOGuildBank.withdraw_cod        = fields[5].GetUInt32();
            OOGuildBank.withdraw_cod_total  = fields[6].GetUInt32();
            OOGuildBank.name                = fields[7].GetString();

            OOGuildBanks[fields[0].GetUInt32()]       = OOGuildBank;
            OOGuildBankVendors[fields[2].GetUInt32()] = OOGuildBank;
        }
        while (gresult->NextRow());
    }

    std::unique_ptr<QueryResult> vresult(WorldDatabase.PQuery("SELECT `entry`, `item`, `maxcount`, `incrtime`, `itemflags`, `condition_id` FROM npc_vendor"));
    pcount = 0;
    if (vresult)
    {
        do
        {
            Field* fields = vresult->Fetch();

            uint32 entry        = fields[0].GetUInt32();
            uint32 item_id      = fields[1].GetUInt32();
            uint32 maxcount     = fields[2].GetUInt32();
            uint32 incrtime     = fields[3].GetUInt32();
            uint32 itemflags    = fields[4].GetUInt32();
            uint32 conditionId  = fields[5].GetUInt32();

            OOGuildBankVendorItems[entry][item_id] = maxcount; // guild bank, vendor entry / item entry / count
            OOGuildBankVendorLocks[entry] = 0; // vendor id, timestamp
            ++pcount;
        }
        while (vresult->NextRow());
    }
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u OO Guild Bank Items", pcount);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "");

    /// Items
    std::unique_ptr<QueryResult> iresult(WorldDatabase.Query("SELECT l.entry, l.name_loc4 FROM `locales_item` l join item_template i on i.entry = l.entry where i.max_count = 0 and i.bonding in (0,2) and i.flags != 2 and l.name_loc4 is not null and TRIM(l.name_loc4) <> ''"));
    if (iresult)
    {
        do
        {
            Field* fields = iresult->Fetch();
            OOItems[fields[1].GetString()] = fields[0].GetUInt32();
        }
        while (iresult->NextRow());
    }

    // entries.clear();
    // std::unique_ptr<QueryResult> result(WorldDatabase.Query("SELECT `string_id` FROM `autobroadcast`"));

    // if (!result)
    // {
    //     BarGoLink bar(1);
    //     bar.step();

    //     sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "");
    //     sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded 0 OOAutoBroadCast message");
    //     return;
    // }

    // uint32 count = 0;
    // BarGoLink bar(result->GetRowCount());

    // Field* fields;
    // do
    // {
    //     bar.step();
    //     OOBroadCastEntry e;
    //     fields = result->Fetch();

    //     e.stringId = fields[0].GetInt32();

    //     entries.push_back(e);
    //     ++count;
    // }
    // while (result->NextRow());

    // sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "");
    // sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u OOAutoBroadCast messages", count);

    BuildBannedTransformDisplayIds();
}

// 派对入场券随机变身（8067 Party Time!）/手动指定变身，两个入口共用的黑名单。
// 之所以不直接从 creature_display_info_addon 里删行，是因为这张表同时也是
// 真实生物（比如下面的 Furbolg/Ogre）的碰撞体积/移动速度数据来源，删掉会影响
// 这些生物本身在世界里正常生成时的表现——黑名单只在"随机挑变身"/"手动指定变身"
// 这两处生效，不影响其他任何读取这张表的地方。
// 依赖 creature_display_info_addon 已经加载完毕（World.cpp 里 sObjectMgr.LoadCreatureDisplayInfoAddon()
// 在 sOOMgr.Load() 之前执行），.reload creature_display_info_addon 后也要记得重新调用一次本函数。
void OOMgr::BuildBannedTransformDisplayIds()
{
    m_bannedTransformDisplayIds.clear();

    // 勇敢者/天选者满级奖励专属变身，不允许在派对入场券里出现，保持这两个模型的专属感
    // 905462 达托尔的变形棒 -> creature_template 3925 -> display_id1 6823（熊怪）
    // 918258 戈多克食人魔套装 -> creature_template 14353 -> display_id1 14406（食人魔）
    m_bannedTransformDisplayIds.insert(6823);
    m_bannedTransformDisplayIds.insert(14406);

    // 欢乐制造器 PX-238 Winter Wondervolt 系列变身（spell 26157/26272/26273/26274 各自的巨大化冬日精灵）
    static uint32 const PX238_DISPLAY_IDS[] = { 15687, 15803, 15806, 15807, 15799, 15800, 15795, 15796 };
    for (uint32 displayId : PX238_DISPLAY_IDS)
        m_bannedTransformDisplayIds.insert(displayId);

    // 过滤大体型boss/特效模型。碰撞半径(bounding_radius)只能抓到"胖"的模型（Colossus这类），
    // 抓不到奥妮克希亚(8570)这类细长的龙——它bounding_radius只有1.8，但combat_reach高达23.4。
    // 实测数据：combat_reach>5 这个阈值只命中113条记录，抽样全是团本/世界boss/稀有（拉格纳罗斯、
    // 萨菲隆、四条绿龙、Onyxia、Patchwerk等），没有误伤正常野兽/人形模型，所以两个条件任一超标就过滤。
    static float const LARGE_BOSS_BOUNDING_RADIUS = 2.5f;
    static float const LARGE_BOSS_COMBAT_REACH = 5.0f;
    for (uint32 i = 1; i < sCreatureDisplayInfoAddonStorage.GetMaxEntry(); ++i)
    {
        if (CreatureDisplayInfoAddon const* minfo = sCreatureDisplayInfoAddonStorage.LookupEntry<CreatureDisplayInfoAddon>(i))
            if (minfo->bounding_radius > LARGE_BOSS_BOUNDING_RADIUS || minfo->combat_reach > LARGE_BOSS_COMBAT_REACH)
                m_bannedTransformDisplayIds.insert(minfo->display_id);
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u banned transform display ids", uint32(m_bannedTransformDisplayIds.size()));
}

// 勇敢者/天选者满60级音效：全服所有在线玩家按各自阵营先听3次钟响（1秒1次），
// 然后再听一次欢呼——用 m_level60Timer/m_level60BellsLeft 这对状态在 Update() 里分步播放，
// 不能在同一帧里连续排3个PlaySound包，那样客户端会把3声钟响糊在一起同时播放。
static uint32 const LEVEL60_BELL_ALLIANCE   = 6594; // BellTollAlliance
static uint32 const LEVEL60_BELL_HORDE      = 6595; // BellTollHorde
static uint32 const LEVEL60_CHEER_ALLIANCE  = 8571; // CrowdCheerAlliance1
static uint32 const LEVEL60_CHEER_HORDE     = 8573; // CrowdCheerHorde1
static uint32 const LEVEL60_BELL_RING_COUNT = 3;
static uint32 const LEVEL60_STEP_DELAY_MS   = 1000; // 钟声间隔，以及最后一声钟响到欢呼的间隔

static void PlaySoundToAllOnline(uint32 allianceSoundId, uint32 hordeSoundId)
{
    for (auto const& itr : sWorld.GetAllSessions())
    {
        WorldSession* session = itr.second;
        if (!session)
            continue;
        Player* player = session->GetPlayer();
        if (!player || !player->IsInWorld())
            continue;
        player->PlayDirectSound(player->GetTeam() == ALLIANCE ? allianceSoundId : hordeSoundId, player);
    }
}

void OOMgr::Update(uint32 diff)
{
    // Path recording: poll the position of each recording GM every 200ms.
    if (!m_pathRecordings.empty())
    {
        m_pathRecordPollTimer += diff;
        if (m_pathRecordPollTimer >= 200)
        {
            m_pathRecordPollTimer = 0;
            for (auto& kv : m_pathRecordings)
            {
                Player* player = ObjectAccessor::FindPlayer(kv.first);
                if (!player || !player->IsInWorld())
                    continue;
                Position const cur = player->GetPosition();
                float const dx = cur.x - kv.second.lastPoint.x;
                float const dy = cur.y - kv.second.lastPoint.y;
                if (dx * dx + dy * dy >= 25.0f) // 5 yards threshold (5² = 25)
                {
                    kv.second.points.push_back(cur);
                    kv.second.lastPoint = cur;
                }
            }
        }
    }

    // 勇敢者/天选者满60级音效：分步播放剩余的钟响，最后补一次欢呼
    if (m_level60Timer)
    {
        if (diff >= m_level60Timer)
        {
            if (m_level60BellsLeft > 0)
            {
                PlaySoundToAllOnline(LEVEL60_BELL_ALLIANCE, LEVEL60_BELL_HORDE);
                --m_level60BellsLeft;
                m_level60Timer = LEVEL60_STEP_DELAY_MS;
            }
            else
            {
                PlaySoundToAllOnline(LEVEL60_CHEER_ALLIANCE, LEVEL60_CHEER_HORDE);
                m_level60Timer = 0;
            }
        }
        else
            m_level60Timer -= diff;
    }
}

void OOMgr::AnnounceLevel60Fanfare()
{
    // 立即播放第一声钟响，剩下 (LEVEL60_BELL_RING_COUNT - 1) 声由 Update() 按1秒间隔接力播放，
    // 全部播完后再自动补一次欢呼。
    PlaySoundToAllOnline(LEVEL60_BELL_ALLIANCE, LEVEL60_BELL_HORDE);
    m_level60BellsLeft = LEVEL60_BELL_RING_COUNT - 1;
    m_level60Timer = LEVEL60_STEP_DELAY_MS;
}

// ---------------------------------------------------------------------------
// Path recording implementation
// ---------------------------------------------------------------------------

void OOMgr::StartPathRecording(ObjectGuid playerGuid, std::string const& name)
{
    PathRecordingSession& session = m_pathRecordings[playerGuid];
    session.pathName  = name;
    session.points.clear();

    // Record the starting position immediately
    if (Player* player = ObjectAccessor::FindPlayer(playerGuid))
    {
        session.lastPoint = player->GetPosition();
        session.points.push_back(session.lastPoint);
    }
}

void OOMgr::StopPathRecording(ObjectGuid playerGuid, std::string& outMessage)
{
    auto it = m_pathRecordings.find(playerGuid);
    if (it == m_pathRecordings.end())
    {
        outMessage = "No active recording.";
        return;
    }

    PathRecordingSession const& session = it->second;

    if (session.points.empty())
    {
        m_pathRecordings.erase(it);
        outMessage = "Recording stopped but no points were captured.";
        return;
    }

    // Build output file path
    time_t const now = time(nullptr);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y%m%d_%H%M%S", localtime(&now));
    std::string logsDir = sConfig.GetStringDefault("LogsDir", "./logs");
    if (!logsDir.empty() && logsDir.back() != '/' && logsDir.back() != '\\')
        logsDir += "/";
    std::string const filename = logsDir + "botpath_" + session.pathName + "_" + timebuf + ".cpp";

    std::ofstream out(filename);
    if (!out.is_open())
    {
        m_pathRecordings.erase(it);
        outMessage = "ERROR: Could not write file " + filename;
        return;
    }

    out << "// Recorded: " << timebuf << "  Points: " << session.points.size() << "\n";
    out << "BattleBotPath vPath_" << session.pathName << " =\n{\n";
    out << std::fixed << std::setprecision(4);
    for (Position const& p : session.points)
        out << "    { " << p.x << "f, " << p.y << "f, " << p.z << "f, nullptr },\n";
    out << "};\n";
    out.close();

    uint32 const count = (uint32)session.points.size();
    m_pathRecordings.erase(it);
    outMessage = "Saved " + std::to_string(count) + " points -> " + filename;
}

bool OOMgr::IsRecording(ObjectGuid playerGuid) const
{
    return m_pathRecordings.count(playerGuid) > 0;
}

uint32 OOMgr::GetRecordedPointCount(ObjectGuid playerGuid) const
{
    auto it = m_pathRecordings.find(playerGuid);
    return (it != m_pathRecordings.end()) ? (uint32)it->second.points.size() : 0;
}

void OOMgr::LoadBotNameThemes()
{
    std::string const dirPath = "./BattleBotNames/";
    std::vector<std::string> files = IO::Filesystem::GetAllFilesInFolder(dirPath, IO::Filesystem::OutputFilePath::FullFilePath);

    uint32 count = 0;
    for (std::string const& filePath : files)
    {
        // only .txt files
        if (filePath.size() < 4 || filePath.substr(filePath.size() - 4) != ".txt")
            continue;

        std::ifstream file(filePath);
        if (!file.is_open())
            continue;

        BotNameTheme theme;
        // derive theme name from filename (strip path and extension)
        size_t slash = filePath.find_last_of("/\\");
        std::string filename = (slash != std::string::npos) ? filePath.substr(slash + 1) : filePath;
        theme.name = filename.substr(0, filename.size() - 4);

        bool inAlliance = false, inHorde = false;
        std::string line;
        while (std::getline(file, line))
        {
            // trim trailing whitespace/CR
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;

            if (line == "[正方]") { inAlliance = true;  inHorde = false; continue; }
            if (line == "[反方]") { inAlliance = false; inHorde = true;  continue; }

            if (inAlliance)
                theme.allianceNames.push_back(line);
            else if (inHorde)
                theme.hordeNames.push_back(line);
        }

        if (!theme.allianceNames.empty() || !theme.hordeNames.empty())
        {
            BotNameThemes.push_back(std::move(theme));
            ++count;
        }
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u BotName themes from %s", count, dirPath.c_str());
}

std::string OOMgr::GetBotName(uint32 instanceId, bool isAlliance)
{
    if (BotNameThemes.empty())
        return "";

    // Assign a theme to this key on first access, picking one not already in use
    // by another active BG so each concurrent battleground gets its own theme.
    auto it = BgBotNameThemeMap.find(instanceId);
    if (it == BgBotNameThemeMap.end())
    {
        // Collect indices currently locked by other active BGs
        std::set<uint32> usedIndices;
        for (auto const& kv : BgBotNameThemeMap)
            usedIndices.insert(kv.second.first);

        // Pick from themes not in use; fall back to any random theme if all are taken
        std::vector<uint32> available;
        for (uint32 i = 0; i < (uint32)BotNameThemes.size(); ++i)
            if (usedIndices.find(i) == usedIndices.end())
                available.push_back(i);

        uint32 themeIndex = available.empty()
            ? urand(0, (uint32)BotNameThemes.size() - 1)
            : available[urand(0, (uint32)available.size() - 1)];

        bool sidesFlipped = urand(0, 1) == 1;
        BgBotNameThemeMap[instanceId] = {themeIndex, sidesFlipped};
        it = BgBotNameThemeMap.find(instanceId);
    }

    BotNameTheme const& theme = BotNameThemes[it->second.first];
    bool useFirstSide = (isAlliance != it->second.second);
    std::vector<std::string> const& names = useFirstSide ? theme.allianceNames : theme.hordeNames;
    if (names.empty())
        return "";

    return names[urand(0, (uint32)names.size() - 1)];
}

void OOMgr::RemoveBgTheme(uint32 instanceId)
{
    BgBotNameThemeMap.erase(instanceId);
}

std::vector<std::string> OOMgr::GetPVPText(uint32 prace, uint32 plass) {
    std::vector<std::string> tmpTexts;

    for (auto & element : PVPTexts[0][0]) {
        tmpTexts.push_back(element);
    }

    for (auto & element : PVPTexts[0][plass]) {
        tmpTexts.push_back(element);
    }

    for (auto & element : PVPTexts[prace][plass]) {
        tmpTexts.push_back(element);
    }

    return tmpTexts;
}

// ---------------------------------------------------------------------------
// BG kill announcement helpers
// ---------------------------------------------------------------------------

namespace
{

static std::string GetClassColorStr(uint8 pClass)
{
    switch (pClass)
    {
        case CLASS_WARRIOR: return "C79C6E";
        case CLASS_PALADIN: return "F58CBA";
        case CLASS_HUNTER:  return "ABD473";
        case CLASS_ROGUE:   return "FFF569";
        case CLASS_PRIEST:  return "FFFFFF";
        case CLASS_SHAMAN:  return "0070DE";
        case CLASS_MAGE:    return "69CCF0";
        case CLASS_WARLOCK: return "9482C9";
        case CLASS_DRUID:   return "FF7D0A";
        default:            return "FFFFFF";
    }
}

static char const* GetClassNameZH(uint8 pClass)
{
    switch (pClass)
    {
        case CLASS_WARRIOR: return "战士";
        case CLASS_PALADIN: return "圣骑士";
        case CLASS_HUNTER:  return "猎人";
        case CLASS_ROGUE:   return "盗贼";
        case CLASS_PRIEST:  return "牧师";
        case CLASS_SHAMAN:  return "萨满";
        case CLASS_MAGE:    return "法师";
        case CLASS_WARLOCK: return "术士";
        case CLASS_DRUID:   return "德鲁伊";
        default:            return "英雄";
    }
}

template<std::size_t N>
static char const* PickRand(char const* const (&arr)[N])
{
    return arr[urand(0, N - 1)];
}

static char const* GetClassKillPrefix(uint8 pClass)
{
    static char const* const sWarrior[] = { "怒斩千军！", "铁拳破敌！", "横刀立马！", "战意沸腾！", "不可阻挡！" };
    static char const* const sPaladin[] = { "圣光审判！", "以光之名！", "圣战临身！", "神圣降临！", "圣光常护！" };
    static char const* const sHunter[]  = { "箭如雨下！", "猎物已至！", "穿云利箭！", "无处遁形！", "野性追踪！" };
    static char const* const sRogue[]   = { "暗影突袭！", "背刺奏效！", "毒刃出鞘！", "神出鬼没！", "一击毙命！" };
    static char const* const sPriest[]  = { "黑暗降临！", "魂归冥府！", "影法显威！", "毒奶奉上！", "圣言审判！" };
    static char const* const sShaman[]  = { "雷霆万钧！", "元素之怒！", "图腾显威！", "大地的愤怒！", "雷霆觉醒！" };
    static char const* const sMage[]    = { "法术席卷！", "奥术爆发！", "冰封千里！", "炎爆四起！", "魔法无敌！" };
    static char const* const sWarlock[] = { "灵魂已夺！", "黑暗诅咒！", "恶魔附身！", "痛苦永恒！", "绝望深渊！" };
    static char const* const sDruid[]   = { "自然觉醒！", "月焰天击！", "狂暴突袭！", "大地之怒！", "万物皆服！" };
    static char const* const sGeneric[] = { "英雄显威！", "以一敌众！", "所向披靡！", "铁骨铮铮！", "势不可当！" };

    switch (pClass)
    {
        case CLASS_WARRIOR: return PickRand(sWarrior);
        case CLASS_PALADIN: return PickRand(sPaladin);
        case CLASS_HUNTER:  return PickRand(sHunter);
        case CLASS_ROGUE:   return PickRand(sRogue);
        case CLASS_PRIEST:  return PickRand(sPriest);
        case CLASS_SHAMAN:  return PickRand(sShaman);
        case CLASS_MAGE:    return PickRand(sMage);
        case CLASS_WARLOCK: return PickRand(sWarlock);
        case CLASS_DRUID:   return PickRand(sDruid);
        default:            return PickRand(sGeneric);
    }
}

static char const* GetBGKillVerb(BattleGroundTypeId bgType)
{
    static char const* const sAV[] = {
        "在阿尔特拉克山谷的烽火中击杀了",
        "于冰雪山谷的血战中斩落了",
        "在旷日持久的攻城战中击败了",
        "以钢铁之志在战场上征服了",
        "于两军决战的前线击倒了",
    };
    static char const* const sWSG[] = {
        "在战歌峡谷的旗帜争夺中击倒了",
        "于护旗阵地截杀了",
        "以闪电般的速度击败了",
        "在旗帜争夺战中斩落了",
        "于峡谷丛林中伏击了",
    };
    static char const* const sAB[] = {
        "在阿拉希盆地的据点争夺中击败了",
        "于资源要冲处斩杀了",
        "以一夫当关之势击倒了",
        "在攻城略地中击败了",
        "守卫据点，斩落了",
    };
    static char const* const sDefault[] = {
        "击杀了", "斩落了", "击败了", "终结了", "打败了",
    };

    switch (bgType)
    {
        case BATTLEGROUND_AV: return PickRand(sAV);
        case BATTLEGROUND_WS: return PickRand(sWSG);
        case BATTLEGROUND_AB: return PickRand(sAB);
        default:              return PickRand(sDefault);
    }
}

} // anonymous namespace

void OOMgr::HandleBGKillAnnounce(Player* killer, Player* victim)
{
    if (!killer || !victim)
        return;
    if (killer->IsGameMaster() || killer->IsBot() || victim->IsBot())
        return;
    if (killer->GetTeam() == victim->GetTeam())
        return;
    if (!killer->InBattleGround())
        return;
    BattleGround* bg = killer->GetBattleGround();
    if (!bg)
        return;

    char const* prefix = GetClassKillPrefix(killer->GetClass());
    char const* verb   = GetBGKillVerb(bg->GetTypeID());

    std::string message = std::string(prefix)
        + " |cFF" + GetClassColorStr(killer->GetClass()) + killer->GetName()
        + "|r（" + std::to_string(killer->GetLevel()) + "级" + GetClassNameZH(killer->GetClass()) + "） "
        + verb
        + " |cFF" + GetClassColorStr(victim->GetClass()) + victim->GetName()
        + "|r（" + std::to_string(victim->GetLevel()) + "级" + GetClassNameZH(victim->GetClass()) + "）";

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_BG_SYSTEM_NEUTRAL, message.c_str());
    bg->SendPacketToAll(&data);
}
