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

#ifndef MANGOS_CUSTOMTAXIMGR_H
#define MANGOS_CUSTOMTAXIMGR_H

#include "Common.h"
#include "DBCStructure.h"
#include "ObjectGuid.h"

#include <map>
#include <string>
#include <vector>

class Player;

struct CustomTaxiPath
{
    uint32 id = 0;
    std::string name;
    uint32 mapId = 0;
    uint32 mountDisplayId = 0;
    std::string createdBy;
    std::vector<TaxiPathNodeEntry> nodes;
};

struct CustomTaxiRecorder
{
    std::string name;
    uint32 mapId = 0;
    uint32 sampleTimer = 0;
    bool recording = false;
    std::string validationError;
    std::vector<TaxiPathNodeEntry> nodes;
};

class CustomTaxiMgr
{
public:
    void LoadFromDB();
    void UpdateRecorder(Player* player, uint32 diff);

    bool StartRecording(Player* player, std::string const& name, std::string& error);
    bool StopRecording(Player* player, std::string& error);
    bool SaveRecording(Player* player, uint32& pathId, std::string& error);
    bool DiscardRecording(ObjectGuid guid);

    bool Play(Player* player, uint32 pathId, std::string& error) const;
    bool DeletePath(uint32 pathId, std::string& error);

    CustomTaxiRecorder const* GetRecorder(ObjectGuid guid) const;
    std::map<uint32, CustomTaxiPath> const& GetPaths() const { return m_paths; }

    static float CalculateDistance(std::vector<TaxiPathNodeEntry> const& nodes);

private:
    bool TryAddNode(Player* player, CustomTaxiRecorder& recorder, bool force, std::string& error) const;
    bool ValidateRecorder(CustomTaxiRecorder const& recorder, std::string& error) const;

    std::map<uint32, CustomTaxiPath> m_paths;
    std::map<ObjectGuid, CustomTaxiRecorder> m_recorders;
};

extern CustomTaxiMgr sCustomTaxiMgr;

#endif
