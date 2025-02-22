#ifndef _OO_MGR_H
#define _OO_MGR_H

#include "Policies/Singleton.h"
#include "Platform/Define.h"
#include "GameObject.h"
#include "Chat.h"

#include <vector>

////////////////////
// Broadcast
// Load character_name
// Load character_pvp_text 
///////////////////

struct OOBroadCastEntry
{
    int32 stringId;
};

class OOMgr
{
    public :
        OOMgr();
        ~OOMgr();

        void Load();
        void Update(uint32 diff);

        std::array< std::string, 10000 > BattleBotNames;
        std::map< uint32, std::map< uint32, std::vector<std::string>> >  PVPTexts;

        // player guid, object GameObject* pGo
        std::map< uint32, GameObject* > SnowBallObjects;

        std::vector<std::string > GetPVPText(uint32 prace, uint32 plass);

        std::map< uint32, uint32 > PartyQuestTexts {
            {32022, 22015},
            {32023, 22016},
            {32024, 22017},
            {32025, 22018},
            {32026, 22020},
            {32027, 22021},
            {32030, 22022},
            {32031, 22023},
            {32032, 22024},
            {32033, 22019}
        };

        char const* GetClassColor(int8 pclass) {
            std::map<int8, std::string> ClassColor;
            ClassColor[1] = "C79C6E";
            ClassColor[2] = "F58CBA";
            ClassColor[3] = "ABD473";
            ClassColor[4] = "FFF569";
            ClassColor[5] = "FFFFFF";
            ClassColor[7] = "0070DE";
            ClassColor[8] = "69CCF0";
            ClassColor[9] = "9482C9";
            ClassColor[11] = "FF7d0A";

            return ClassColor[pclass].c_str();
        }

    protected:
        std::vector<OOBroadCastEntry> entries;
        time_t _constInterval;
        time_t _current;
};

#define sOOMgr MaNGOS::Singleton<OOMgr>::Instance()
#endif
