#include "Database/DatabaseEnv.h"
#include "World.h"
#include "Log.h"
#include "ProgressBar.h"
#include "Policies/SingletonImp.h"
#include "Util.h"

#include "OO/OOMgr.h"

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

    // for (auto & element : GetPVPText(2, 2)) {
    //     printf("dd %s \n", element.c_str());
    // }

    // Bot Name
    uint32 bindex = 0;
    std::unique_ptr<QueryResult> bresult(CharacterDatabase.Query("SELECT name FROM `character_name` where name not in (SELECT Name from characters)"));
    if (bresult)
    {
         do
        {
            Field* fields   = bresult->Fetch();
            BattleBotNames[bindex] = fields[0].GetString();
            bindex++;
        }
        while (bresult->NextRow());
    }

    /// Build Bank
    // SELECT `guild_id`, `guild_rank`, `withdraw_item`, `withdraw_cod` FROM `character_guild_bank`
    std::unique_ptr<QueryResult> gresult(CharacterDatabase.Query("SELECT guid, guild_id, vendor_id, guild_rank, withdraw_item, withdraw_cod, withdraw_cod_total, vendor_name FROM `character_guild_bank`"));
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

}

void OOMgr::Update(uint32 diff)
{
    // if (entries.empty())
    //     return;

    // _current += diff;

    // if (_current >= _constInterval)
    // {
    //     OOBroadCastEntry entry = SelectRandomContainerElement(entries);
    //     sWorld.SendWorldText(entry.stringId);
    //     _current = 0;
    // }
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
