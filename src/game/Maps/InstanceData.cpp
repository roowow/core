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

#include "InstanceData.h"
#include "Database/DatabaseEnv.h"
#include "Map.h"
#include "OO/InstanceDataCache.h"
#include "OO/DbWriteOutbox.h"

void InstanceData::SaveToDB()
{
    // no reason to save BGs/Arenas
    if (instance->IsBattleGround())
        return;

    if (!Save())
        return;

    std::string data = Save();

    // Write-through: keep the local Redis cache in sync so the next Map::CreateInstanceData
    // for this instance/world row reads the fresh value instead of a stale one. Must happen
    // with the raw value, before CharacterDatabase.escape_string() below mangles it for SQL.
    sInstanceDataCache.Set(MakeInstanceCacheKey(instance->Instanceable(),
        instance->Instanceable() ? instance->GetInstanceId() : instance->GetId()), data);

    CharacterDatabase.escape_string(data);

    // Phase3 (see HPHA.md) - routed through sCharactersOutbox. Absolute-assignment UPDATE, safe
    // to replay. std::string (not a fixed snprintf buffer) since the serialized blob size varies
    // a lot by encounter/raid complexity.
    if (instance->Instanceable())
        sCharactersOutbox.Enqueue("UPDATE `instance` SET `data` = '" + data + "' WHERE `id` = " + std::to_string(instance->GetInstanceId()));
    else
        sCharactersOutbox.Enqueue("UPDATE `world` SET `data` = '" + data + "' WHERE `map` = " + std::to_string(instance->GetId()));
}

bool InstanceData::CheckConditionCriteriaMeet(Player const* /*player*/, uint32 map_id, WorldObject const* source, uint32 instance_condition_id) const
{
    sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "Condition system call InstanceData::CheckConditionCriteriaMeet but instance script for map %u not have implementation for player condition criteria with internal id %u for map %u",
                  instance->GetId(), instance_condition_id, map_id);
    return false;
}
