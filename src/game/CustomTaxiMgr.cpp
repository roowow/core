/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include "CustomTaxiMgr.h"

#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>

namespace
{
uint32 const CUSTOM_TAXI_SAMPLE_INTERVAL = 500;
float const CUSTOM_TAXI_MIN_NODE_DISTANCE = 8.0f;
float const CUSTOM_TAXI_MIN_VERTICAL_DISTANCE = 4.0f;
float const CUSTOM_TAXI_PLAY_START_DISTANCE = 20.0f;
uint32 const CUSTOM_TAXI_MAX_NODES = 2000;

float NodeDistance(TaxiPathNodeEntry const& left, TaxiPathNodeEntry const& right)
{
    float const dx = left.x - right.x;
    float const dy = left.y - right.y;
    float const dz = left.z - right.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::string Trim(std::string value)
{
    auto const notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}
}

CustomTaxiMgr sCustomTaxiMgr;

void CustomTaxiMgr::LoadFromDB()
{
    m_paths.clear();

    std::unique_ptr<QueryResult> pathResult = WorldDatabase.Query(
        "SELECT `id`, `name`, `map_id`, `mount_display_id`, `created_by` "
        "FROM `custom_taxi_path` ORDER BY `id`");

    if (pathResult)
    {
        do
        {
            Field* fields = pathResult->Fetch();
            CustomTaxiPath& path = m_paths[fields[0].GetUInt32()];
            path.id = fields[0].GetUInt32();
            path.name = fields[1].GetCppString();
            path.mapId = fields[2].GetUInt32();
            path.mountDisplayId = fields[3].GetUInt32();
            path.createdBy = fields[4].GetCppString();
        }
        while (pathResult->NextRow());
    }

    std::unique_ptr<QueryResult> nodeResult = WorldDatabase.Query(
        "SELECT `path_id`, `point`, `map_id`, `position_x`, `position_y`, `position_z` "
        "FROM `custom_taxi_path_node` ORDER BY `path_id`, `point`");

    if (nodeResult)
    {
        do
        {
            Field* fields = nodeResult->Fetch();
            auto pathItr = m_paths.find(fields[0].GetUInt32());
            if (pathItr == m_paths.end())
            {
                sLog.Out(LOG_DBERROR, LOG_LVL_ERROR,
                    "Custom taxi node references missing path %u, skipped.", fields[0].GetUInt32());
                continue;
            }

            CustomTaxiPath& path = pathItr->second;
            TaxiPathNodeEntry node = {};
            node.path = 0;
            node.index = uint32(path.nodes.size());
            node.mapid = fields[2].GetUInt32();
            node.x = fields[3].GetFloat();
            node.y = fields[4].GetFloat();
            node.z = fields[5].GetFloat();
            path.nodes.push_back(node);
        }
        while (nodeResult->NextRow());
    }

    for (auto itr = m_paths.begin(); itr != m_paths.end();)
    {
        bool invalidMap = false;
        for (TaxiPathNodeEntry const& node : itr->second.nodes)
        {
            if (node.mapid != itr->second.mapId)
            {
                invalidMap = true;
                break;
            }
        }

        if (itr->second.nodes.size() < 2 || invalidMap)
        {
            sLog.Out(LOG_DBERROR, LOG_LVL_ERROR,
                "Custom taxi path %u (%s) has invalid nodes, skipped.",
                itr->second.id, itr->second.name.c_str());
            itr = m_paths.erase(itr);
        }
        else
            ++itr;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u custom taxi paths", uint32(m_paths.size()));
}

void CustomTaxiMgr::UpdateRecorder(Player* player, uint32 diff)
{
    if (!player || m_recorders.empty())
        return;

    auto itr = m_recorders.find(player->GetObjectGuid());
    if (itr == m_recorders.end() || !itr->second.recording)
        return;

    CustomTaxiRecorder& recorder = itr->second;
    if (recorder.sampleTimer > diff)
    {
        recorder.sampleTimer -= diff;
        return;
    }

    recorder.sampleTimer = CUSTOM_TAXI_SAMPLE_INTERVAL;

    std::string error;
    if (!TryAddNode(player, recorder, false, error))
    {
        recorder.recording = false;
        recorder.validationError = error;
        player->GetSession()->SendNotification("Custom taxi recording paused: %s", error.c_str());
    }
}

bool CustomTaxiMgr::StartRecording(Player* player, std::string const& requestedName, std::string& error)
{
    if (!player || !player->IsInWorld())
    {
        error = "Player is not in world.";
        return false;
    }

    if (player->IsTaxiFlying())
    {
        error = "Finish the current taxi flight before recording.";
        return false;
    }

    if (m_recorders.find(player->GetObjectGuid()) != m_recorders.end())
    {
        error = "A custom taxi recording already exists. Save or discard it first.";
        return false;
    }

    std::string const name = Trim(requestedName);
    if (name.empty())
    {
        error = "Route name is required.";
        return false;
    }

    if (name.size() > 120)
    {
        error = "Route name is too long. Maximum length is 120 bytes.";
        return false;
    }

    CustomTaxiRecorder recorder;
    recorder.name = name;
    recorder.mapId = player->GetMapId();
    recorder.sampleTimer = CUSTOM_TAXI_SAMPLE_INTERVAL;
    recorder.recording = true;

    if (!TryAddNode(player, recorder, true, error))
        return false;

    m_recorders[player->GetObjectGuid()] = recorder;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[CustomTaxi] recording started player %s route %s map %u.",
        player->GetName(), name.c_str(), recorder.mapId);
    return true;
}

bool CustomTaxiMgr::StopRecording(Player* player, std::string& error)
{
    if (!player)
    {
        error = "Player is not available.";
        return false;
    }

    auto itr = m_recorders.find(player->GetObjectGuid());
    if (itr == m_recorders.end())
    {
        error = "No custom taxi recording exists.";
        return false;
    }

    CustomTaxiRecorder& recorder = itr->second;
    if (recorder.recording)
    {
        if (!TryAddNode(player, recorder, true, error))
        {
            recorder.recording = false;
            recorder.validationError = error;
            return false;
        }
        recorder.recording = false;
    }

    if (!recorder.validationError.empty())
    {
        error = recorder.validationError;
        return false;
    }

    if (!ValidateRecorder(recorder, error))
    {
        recorder.validationError = error;
        return false;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[CustomTaxi] recording stopped player %s route %s nodes %u distance %.1f.",
        player->GetName(), recorder.name.c_str(), uint32(recorder.nodes.size()),
        CalculateDistance(recorder.nodes));
    return true;
}

bool CustomTaxiMgr::SaveRecording(Player* player, uint32& pathId, std::string& error)
{
    if (!player)
    {
        error = "Player is not available.";
        return false;
    }

    auto itr = m_recorders.find(player->GetObjectGuid());
    if (itr == m_recorders.end())
    {
        error = "No custom taxi recording exists.";
        return false;
    }

    CustomTaxiRecorder const& recorder = itr->second;
    if (recorder.recording)
    {
        error = "Stop the recording before saving.";
        return false;
    }

    if (!recorder.validationError.empty())
    {
        error = recorder.validationError;
        return false;
    }

    if (!ValidateRecorder(recorder, error))
        return false;

    std::string safeName = recorder.name;
    std::string safeCreator = player->GetName();
    WorldDatabase.escape_string(safeName);
    WorldDatabase.escape_string(safeCreator);

    std::unique_ptr<QueryResult> pathResult = WorldDatabase.PQuery(
        "SELECT `id` FROM `custom_taxi_path` WHERE `name` = '%s'", safeName.c_str());
    if (pathResult)
        pathId = (*pathResult)[0].GetUInt32();
    else
    {
        std::unique_ptr<QueryResult> maxResult = WorldDatabase.Query(
            "SELECT IFNULL(MAX(`id`), 0) + 1 FROM `custom_taxi_path`");
        pathId = maxResult ? (*maxResult)[0].GetUInt32() : 1;
    }

    WorldDatabase.BeginTransaction();
    WorldDatabase.PExecute("DELETE FROM `custom_taxi_path_node` WHERE `path_id` = %u", pathId);
    WorldDatabase.PExecute(
        "REPLACE INTO `custom_taxi_path` (`id`, `name`, `map_id`, `mount_display_id`, `created_by`) "
        "VALUES (%u, '%s', %u, 0, '%s')",
        pathId, safeName.c_str(), recorder.mapId, safeCreator.c_str());

    for (uint32 point = 0; point < recorder.nodes.size(); ++point)
    {
        TaxiPathNodeEntry const& node = recorder.nodes[point];
        WorldDatabase.PExecute(
            "INSERT INTO `custom_taxi_path_node` "
            "(`path_id`, `point`, `map_id`, `position_x`, `position_y`, `position_z`) "
            "VALUES (%u, %u, %u, %.6f, %.6f, %.6f)",
            pathId, point, node.mapid, node.x, node.y, node.z);
    }
    WorldDatabase.CommitTransaction();

    CustomTaxiPath& path = m_paths[pathId];
    path.id = pathId;
    path.name = recorder.name;
    path.mapId = recorder.mapId;
    path.mountDisplayId = 0;
    path.createdBy = player->GetName();
    path.nodes = recorder.nodes;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[CustomTaxi] route saved player %s route %u (%s) nodes %u distance %.1f.",
        player->GetName(), pathId, recorder.name.c_str(), uint32(recorder.nodes.size()),
        CalculateDistance(recorder.nodes));

    m_recorders.erase(itr);
    return true;
}

bool CustomTaxiMgr::DiscardRecording(ObjectGuid guid)
{
    return m_recorders.erase(guid) != 0;
}

bool CustomTaxiMgr::Play(Player* player, uint32 pathId, std::string& error) const
{
    if (!player)
    {
        error = "Player is not available.";
        return false;
    }

    auto itr = m_paths.find(pathId);
    if (itr == m_paths.end())
    {
        error = "Custom taxi route does not exist.";
        return false;
    }

    if (player->IsTaxiFlying() || player->IsInCombat() ||
        player->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_REMOVE_CLIENT_CONTROL))
    {
        error = "You cannot start a custom taxi flight while busy.";
        return false;
    }

    CustomTaxiPath const& path = itr->second;
    TaxiPathNodeEntry const& firstNode = path.nodes.front();
    if (player->GetMapId() != firstNode.mapid)
    {
        error = "You must be on the route start map.";
        return false;
    }

    if (player->GetDistance3dToCenter(firstNode.x, firstNode.y, firstNode.z) > CUSTOM_TAXI_PLAY_START_DISTANCE)
    {
        error = "Move within 20 yards of the route start before playing it.";
        return false;
    }

    uint32 const sourceTaxiNode = player->GetTeam() == ALLIANCE ? 2 : 23;
    uint32 const mountDisplayId = path.mountDisplayId ?
        path.mountDisplayId : sObjectMgr.GetTaxiMountDisplayId(sourceTaxiNode, player->GetTeam(), true);
    if (!mountDisplayId)
    {
        error = "Unable to find a taxi mount display for your faction.";
        return false;
    }

    player->CombatStop();
    player->TradeCancel(true);
    player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
    if (player->IsInDisallowedMountForm())
    {
        player->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
        player->RemoveSpellsCausingAura(SPELL_AURA_TRANSFORM);
    }
    player->InterruptNonMeleeSpells(false);

    if (!player->GetTaxi().SetCustomTaxiPath(path.nodes))
    {
        error = "Unable to create the custom taxi flight.";
        return false;
    }

    player->GetSession()->SendDoFlight(mountDisplayId, 0);
    return true;
}

bool CustomTaxiMgr::DeletePath(uint32 pathId, std::string& error)
{
    auto itr = m_paths.find(pathId);
    if (itr == m_paths.end())
    {
        error = "Custom taxi route does not exist.";
        return false;
    }

    WorldDatabase.BeginTransaction();
    WorldDatabase.PExecute("DELETE FROM `custom_taxi_path_node` WHERE `path_id` = %u", pathId);
    WorldDatabase.PExecute("DELETE FROM `custom_taxi_path` WHERE `id` = %u", pathId);
    WorldDatabase.CommitTransaction();

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[CustomTaxi] route deleted %u (%s).", pathId, itr->second.name.c_str());
    m_paths.erase(itr);
    return true;
}

CustomTaxiRecorder const* CustomTaxiMgr::GetRecorder(ObjectGuid guid) const
{
    auto itr = m_recorders.find(guid);
    return itr == m_recorders.end() ? nullptr : &itr->second;
}

float CustomTaxiMgr::CalculateDistance(std::vector<TaxiPathNodeEntry> const& nodes)
{
    float distance = 0.0f;
    for (size_t i = 1; i < nodes.size(); ++i)
        distance += NodeDistance(nodes[i - 1], nodes[i]);
    return distance;
}

bool CustomTaxiMgr::TryAddNode(Player* player, CustomTaxiRecorder& recorder, bool force, std::string& error) const
{
    if (!player || !player->IsInWorld())
    {
        error = "Player is not in world.";
        return false;
    }

    if (player->GetMapId() != recorder.mapId)
    {
        error = "Changing maps during recording is not supported.";
        return false;
    }

    TaxiPathNodeEntry node = {};
    node.path = 0;
    node.index = uint32(recorder.nodes.size());
    node.mapid = player->GetMapId();
    node.x = player->GetPositionX();
    node.y = player->GetPositionY();
    node.z = player->GetPositionZ();

    if (!recorder.nodes.empty())
    {
        TaxiPathNodeEntry const& previous = recorder.nodes.back();
        float const distance = NodeDistance(previous, node);
        if (distance < 0.25f)
            return true;

        if (!force && distance < CUSTOM_TAXI_MIN_NODE_DISTANCE &&
            std::fabs(previous.z - node.z) < CUSTOM_TAXI_MIN_VERTICAL_DISTANCE)
            return true;

        if (!player->GetMap()->isInLineOfSight(previous.x, previous.y, previous.z + 1.0f,
                                               node.x, node.y, node.z + 1.0f, true, false))
        {
            error = "The latest segment intersects map collision. Discard and record this route again.";
            return false;
        }
    }

    if (recorder.nodes.size() >= CUSTOM_TAXI_MAX_NODES)
    {
        error = "The route reached the 2000 node limit.";
        return false;
    }

    recorder.nodes.push_back(node);
    return true;
}

bool CustomTaxiMgr::ValidateRecorder(CustomTaxiRecorder const& recorder, std::string& error) const
{
    if (recorder.nodes.size() < 2)
    {
        error = "The route must contain at least 2 nodes.";
        return false;
    }

    for (TaxiPathNodeEntry const& node : recorder.nodes)
    {
        if (node.mapid != recorder.mapId)
        {
            error = "All route nodes must be on the same map.";
            return false;
        }
    }

    return true;
}
