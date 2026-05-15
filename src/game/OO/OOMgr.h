#ifndef _OO_MGR_H
#define _OO_MGR_H

#include "Policies/Singleton.h"
#include "Platform/Define.h"
#include "GameObject.h"
#include "Chat.h"

#include <map>
#include <string>
#include <vector>

////////////////////
// Broadcast
// Load character_name
// Load character_pvp_text
///////////////////

struct BotNameTheme
{
    std::string name;
    std::vector<std::string> allianceNames;
    std::vector<std::string> hordeNames;
};

struct OOBroadCastEntry
{
    int32 stringId;
};

class OOGuildBank
{
    public:
        uint32 guid;
        uint32 guild_id;
        uint32 guild_rank;
        uint32 vendor_id;
        uint32 withdraw_item;
        uint32 withdraw_cod;
        uint32 withdraw_cod_total;
        std::string name;
        uint32 cod;
};

class OOMgr
{
    public :
        OOMgr();
        ~OOMgr();

        void Load();
        void Update(uint32 diff);

        std::array< std::string, 10000 > BattleBotNames;
        std::vector<BotNameTheme> BotNameThemes;
        // bgInstanceId -> {themeIndex, sidesFlipped}
        // sidesFlipped=false: Alliance=正方 Horde=反方; true: reversed
        std::map<uint32, std::pair<uint32, bool>> BgBotNameThemeMap;

        std::string GetBotName(uint32 instanceId, bool isAlliance);
        void RemoveBgTheme(uint32 instanceId);

        std::map< uint32, std::map< uint32, std::vector<std::string>> >  PVPTexts;

        std::map< uint32, uint32 > SnowBallObjects;

        std::map< uint32, OOGuildBank > OOGuildBanks; // character id, OOGuildBank
        std::map< uint32, OOGuildBank > OOGuildBankVendors; // vendor id, OOGuildBank

        std::map< uint32, std::map< uint32, uint32 > > OOPlayerGuildBankCount; // player_guid, < vendor_id, player withdraw count >
        std::map< uint32, std::map< uint32, uint32 > > OOPlayerGuildBankDepositCount; // player_guid, < vendor_id, player deposit count >
        std::map< uint32, std::map< uint32, uint32 > > OOPlayerGuildBankDepositItem; // player_guid, < item, 1 >
        std::map< uint32, std::map< uint32, uint32 > > OOPlayerGuildBankWithdrawItem; // player_guid, < item, 1 >
        std::map< uint32, std::map< uint32, uint32 > > OOGuildBankVendorItems; // vendor id, < item id, count >
        std::map< uint32, uint32 > OOGuildBankVendorLocks; // vendor id, timestamp
        std::map< uint32, std::map< uint32, uint32 > > OOGuildBankCreatureBots; // creature guid (npc summon bot), < creature entry (bot) , timestamp >

        std::map< std::string, uint32 > OOItems;

        std::vector<std::string > GetPVPText(uint32 prace, uint32 plass);

        void HandleBGKillAnnounce(Player* killer, Player* victim);

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

    private:
        void LoadBotNameThemes();
};

#define sOOMgr MaNGOS::Singleton<OOMgr>::Instance()
#endif
