/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "SocialMgr.h"
#include "Policies/SingletonImp.h"
#include "Database/DatabaseEnv.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "MasterPlayer.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "World.h"
#include "Util.h"

INSTANTIATE_SINGLETON_1(SocialMgr);

PlayerSocial::PlayerSocial(): m_playerLowGuid(0), m_masterPlayer(nullptr)
{
}

PlayerSocial::~PlayerSocial()
{
    m_playerSocialMap.clear();
}

uint32 PlayerSocial::GetNumberOfSocialsWithFlag(SocialFlag flag)
{
    uint32 counter = 0;
    for (const auto& itr : m_playerSocialMap)
    {
        if (itr.second.Flags & flag)
            ++counter;
    }
    return counter;
}

bool PlayerSocial::AddToSocialList(ObjectGuid friend_guid, bool ignore)
{
    // check client limits
    if (ignore)
    {
        if (GetNumberOfSocialsWithFlag(SOCIAL_FLAG_IGNORED) >= SOCIALMGR_IGNORE_LIMIT)
            return false;
    }
    else
    {
        if (GetNumberOfSocialsWithFlag(SOCIAL_FLAG_FRIEND) >= SOCIALMGR_FRIEND_LIMIT)
            return false;
    }

    uint32 flag = SOCIAL_FLAG_FRIEND;
    if (ignore)
        flag = SOCIAL_FLAG_IGNORED;

    PlayerSocialMap::const_iterator itr = m_playerSocialMap.find(friend_guid.GetCounter());
    if (itr != m_playerSocialMap.end())
    {
        CharacterDatabase.PExecute("UPDATE character_social SET flags = (flags | %u) WHERE guid = '%u' AND friend = '%u'", flag, m_playerLowGuid, friend_guid.GetCounter());
        m_playerSocialMap[friend_guid.GetCounter()].Flags |= flag;
    }
    else
    {
        CharacterDatabase.PExecute("INSERT INTO character_social (guid, friend, flags) VALUES ('%u', '%u', '%u')", m_playerLowGuid, friend_guid.GetCounter(), flag);
        FriendInfo fi;
        fi.Flags |= flag;
        m_playerSocialMap[friend_guid.GetCounter()] = fi;
    }
    return true;
}

void PlayerSocial::RemoveFromSocialList(ObjectGuid friend_guid, bool ignore)
{
    PlayerSocialMap::iterator itr = m_playerSocialMap.find(friend_guid.GetCounter());
    if (itr == m_playerSocialMap.end())                     // not exist
        return;

    uint32 flag = SOCIAL_FLAG_FRIEND;
    if (ignore)
        flag = SOCIAL_FLAG_IGNORED;

    itr->second.Flags &= ~flag;
    if (itr->second.Flags == 0)
    {
        CharacterDatabase.PExecute("DELETE FROM character_social WHERE guid = '%u' AND friend = '%u'", m_playerLowGuid, friend_guid.GetCounter());
        m_playerSocialMap.erase(itr);
    }
    else
        CharacterDatabase.PExecute("UPDATE character_social SET flags = (flags & ~%u) WHERE guid = '%u' AND friend = '%u'", flag, m_playerLowGuid, friend_guid.GetCounter());
}

void PlayerSocial::SendFriendList()
{
    MasterPlayer* plr = GetMasterPlayer();
    ASSERT(plr);

    uint32 size = GetNumberOfSocialsWithFlag(SOCIAL_FLAG_FRIEND);

    WorldPacket data(SMSG_FRIEND_LIST, (1 + size * 25)); // just can guess size
    data << uint8(size);                                   // friends count

    for (auto& itr : m_playerSocialMap)
    {
        if (itr.second.Flags & SOCIAL_FLAG_FRIEND)         // if IsFriend()
        {
            FriendInfo& friendInfo = itr.second;
            sSocialMgr.GetFriendInfo(plr, itr.first, friendInfo);

            data << ObjectGuid(HIGHGUID_PLAYER, itr.first);// player guid
            data << uint8(friendInfo.Status);              // online/offline/etc?
            if (friendInfo.Status)                         // if online
            {
                data << uint32(friendInfo.Area);           // player area
                data << uint32(friendInfo.Level);          // player level
                data << uint32(friendInfo.Class);          // player class
            }
        }
    }

    plr->GetSession()->SendPacket(&data);
}

void PlayerSocial::SendIgnoreList()
{
    MasterPlayer* plr = GetMasterPlayer();
    ASSERT(plr);

    uint32 size = GetNumberOfSocialsWithFlag(SOCIAL_FLAG_IGNORED);

    WorldPacket data(SMSG_IGNORE_LIST, (1 + size * 8));     // just can guess size
    data << uint8(size);                                    // friends count

    for (const auto& itr : m_playerSocialMap)
        if (itr.second.Flags & SOCIAL_FLAG_IGNORED)
            data << ObjectGuid(HIGHGUID_PLAYER, itr.first);// player guid


    plr->GetSession()->SendPacket(&data);
}

bool PlayerSocial::HasFriend(ObjectGuid friend_guid) const
{
    PlayerSocialMap::const_iterator itr = m_playerSocialMap.find(friend_guid.GetCounter());
    if (itr != m_playerSocialMap.end())
        return itr->second.Flags & SOCIAL_FLAG_FRIEND;
    return false;
}

bool PlayerSocial::HasIgnore(ObjectGuid ignore_guid) const
{
    PlayerSocialMap::const_iterator itr = m_playerSocialMap.find(ignore_guid.GetCounter());
    if (itr != m_playerSocialMap.end())
        return itr->second.Flags & SOCIAL_FLAG_IGNORED;
    return false;
}

SocialMgr::SocialMgr()
{

}

SocialMgr::~SocialMgr()
{

}

void SocialMgr::GetFriendInfo(MasterPlayer* player, uint32 friend_lowguid, FriendInfo &friendInfo)
{
    if (!player)
        return;

    // Self status broadcast - on login or logout
    if (player->GetObjectGuid().GetCounter() == friend_lowguid)
    {
        friendInfo.Status = FRIEND_STATUS_ONLINE;
        if (player->IsAFK())
            friendInfo.Status = FRIEND_STATUS_AFK;
        if (player->IsDND())
            friendInfo.Status = FRIEND_STATUS_DND;
        friendInfo.Area = player->GetZoneId();
        friendInfo.Level = player->GetLevel();
        friendInfo.Class = player->GetClass();
        return;
    }

    MasterPlayer* pFriend = ObjectAccessor::FindMasterPlayer(ObjectGuid(HIGHGUID_PLAYER, friend_lowguid));

    Team team = player->GetTeam();
    AccountTypes security = player->GetSession()->GetSecurity();
    bool allowTwoSideWhoList = sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_WHO_LIST);
    AccountTypes gmLevelInWhoList = AccountTypes(sWorld.getConfig(CONFIG_UINT32_GM_LEVEL_IN_WHO_LIST));

    PlayerSocialMap::const_iterator itr = player->GetSocial()->m_playerSocialMap.find(friend_lowguid);
    if (itr != player->GetSocial()->m_playerSocialMap.end())
    { 
        // PLAYER see his team only and PLAYER can't see MODERATOR, GAME MASTER, ADMINISTRATOR characters
        // MODERATOR, GAME MASTER, ADMINISTRATOR can see all
        if (pFriend && pFriend->GetName() &&
                (security > SEC_PLAYER ||
                 ((pFriend->GetTeam() == team || allowTwoSideWhoList) && (pFriend->GetSession()->GetSecurity() <= gmLevelInWhoList))) &&
                pFriend->IsVisibleGloballyFor(player))
        {
            friendInfo.Status = FRIEND_STATUS_ONLINE;
            if (pFriend->IsAFK())
                friendInfo.Status = FRIEND_STATUS_AFK;
            if (pFriend->IsDND())
                friendInfo.Status = FRIEND_STATUS_DND;
            friendInfo.Area = pFriend->GetZoneId();
            friendInfo.Level = pFriend->GetLevel();
            friendInfo.Class = pFriend->GetClass();
        }
        else
        {
            friendInfo.Status = FRIEND_STATUS_OFFLINE;
            friendInfo.Area = 0;
            friendInfo.Level = 0;
            friendInfo.Class = 0;
        }
    }
}

void SocialMgr::MakeFriendStatusPacket(FriendsResult result, uint32 guid, WorldPacket* data)
{
    data->Initialize(SMSG_FRIEND_STATUS, 5);
    *data << uint8(result);
    *data << ObjectGuid(HIGHGUID_PLAYER, guid);
}

void SocialMgr::SendFriendStatus(MasterPlayer* player, FriendsResult result, ObjectGuid friend_guid, bool broadcast)
{
    uint32 friend_lowguid = friend_guid.GetCounter();

    FriendInfo fi;

    WorldPacket data;
    MakeFriendStatusPacket(result, friend_lowguid, &data);
    GetFriendInfo(player, friend_lowguid, fi);

    switch (result)
    {
        case FRIEND_ADDED_ONLINE:
        case FRIEND_ONLINE:
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_8_4
            data << uint8(fi.Status);
#endif
            data << uint32(fi.Area);
            data << uint32(fi.Level);
            data << uint32(fi.Class);
            break;
        default:
            break;
    }

    if (broadcast)
        BroadcastToFriendListers(player, &data);
    else
        player->GetSession()->SendPacket(&data);
}

void SocialMgr::BroadcastToFriendListers(MasterPlayer const* player, WorldPacket const* packet)
{
    if (!player)
        return;

    Team team = player->GetTeam();
    AccountTypes security = player->GetSession()->GetSecurity();
    uint32 guid     = player->GetGUIDLow();
    AccountTypes gmLevelInWhoList = AccountTypes(sWorld.getConfig(CONFIG_UINT32_GM_LEVEL_IN_WHO_LIST));
    bool allowTwoSideWhoList = sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_WHO_LIST);

    std::shared_lock<std::shared_timed_mutex> guard(_socialMapLock);
    for (const auto& itr : m_socialMap)
    {
        PlayerSocialMap::const_iterator itr2 = itr.second.m_playerSocialMap.find(guid);
        if (itr2 != itr.second.m_playerSocialMap.end() && (itr2->second.Flags & SOCIAL_FLAG_FRIEND))
        {
            MasterPlayer* pFriend = ObjectAccessor::FindMasterPlayer(ObjectGuid(HIGHGUID_PLAYER, itr.first));

            // PLAYER see his team only and PLAYER can't see MODERATOR, GAME MASTER, ADMINISTRATOR characters
            // MODERATOR, GAME MASTER, ADMINISTRATOR can see all
            if (pFriend &&
                    (pFriend->GetSession()->GetSecurity() > SEC_PLAYER ||
                     ((pFriend->GetTeam() == team || allowTwoSideWhoList) && security <= gmLevelInWhoList)) &&
                    player->IsVisibleGloballyFor(pFriend))
                pFriend->GetSession()->SendPacket(packet);
        }
    }
}

PlayerSocial* SocialMgr::LoadFromDB(std::unique_ptr<QueryResult> result, ObjectGuid guid)
{
    std::unique_lock<std::shared_timed_mutex> guard(_socialMapLock);
    PlayerSocial *social = &m_socialMap[guid.GetCounter()];
    social->SetPlayerGuid(guid);

    if (!result)
        return social;

    uint32 friendLowGuid = 0;
    uint32 flags = 0;

    // used to speed up check below. Using GetNumberOfSocialsWithFlag will cause unneeded iteration
    uint32 friendCounter = 0, ignoreCounter = 0;

    do
    {
        Field* fields  = result->Fetch();

        friendLowGuid = fields[0].GetUInt32();
        flags = fields[1].GetUInt32();

        if ((flags & SOCIAL_FLAG_IGNORED) && ignoreCounter >= SOCIALMGR_IGNORE_LIMIT)
            continue;
        if ((flags & SOCIAL_FLAG_FRIEND) && friendCounter >= SOCIALMGR_FRIEND_LIMIT)
            continue;

        // character deleted from account, don't load it until it's restored
        if (!sObjectMgr.GetPlayerAccountIdByGUID(ObjectGuid(HIGHGUID_PLAYER, friendLowGuid)))
            continue;

        social->m_playerSocialMap[friendLowGuid] = FriendInfo(flags);

        if (flags & SOCIAL_FLAG_IGNORED)
            ignoreCounter++;
        else
            friendCounter++;
    }
    while (result->NextRow());
    return social;
}

void SocialMgr::RemovePlayerSocial(uint32 guid)
{
    std::unique_lock<std::shared_timed_mutex> guard(_socialMapLock);
    m_socialMap.erase(guid);
}
