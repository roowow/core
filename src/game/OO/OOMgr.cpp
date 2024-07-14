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
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u PVP texts", pcount);

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
    // for(auto const &ent1 : SnowBallObjects) {
    //     // ent1.first is the first key
    //     for(auto const &ent2 : ent1.second) {
    //         // ent2.first is the second key
    //         // ent2.second is the data
    //     }
    // }

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
