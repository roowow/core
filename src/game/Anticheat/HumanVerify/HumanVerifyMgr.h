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

#ifndef MANGOS_HUMANVERIFYMGR_H
#define MANGOS_HUMANVERIFYMGR_H

#include "Common.h"
#include "ObjectGuid.h"

#include <array>
#include <map>
#include <string>

class Player;

enum class HumanVerifyResult
{
    NotHandled,
    Passed,
    FailedAnswer,
    FailedTooManyAttempts,
    TimedOut,
};

struct HumanVerifyRequest
{
    std::string reason = "manual";
    uint32 timeoutMs = 120 * IN_MILLISECONDS;
    uint8 maxFailures = 3;
    bool allowGameMaster = false;
};

struct HumanVerifyState
{
    uint32 question = 0;
    uint32 correctAnswer = 0;
    uint32 expireTime = 0;
    uint8 failures = 0;
    uint8 maxFailures = 3;
    std::string reason;
    std::array<uint32, 4> options = {};
};

class HumanVerifyMgr
{
public:
    bool StartVerify(Player* player, HumanVerifyRequest const& request = HumanVerifyRequest());
    void CancelVerify(ObjectGuid guid);
    void Update(uint32 diff);

    bool IsPending(ObjectGuid guid) const;
    HumanVerifyResult HandleGossipSelect(Player* player, uint32 sender, uint32 action);

private:
    void BuildQuestion(HumanVerifyState& state) const;
    void SendGossip(Player* player, HumanVerifyState const& state) const;
    void Finish(Player* player, HumanVerifyState const& state, HumanVerifyResult result);

    std::map<ObjectGuid, HumanVerifyState> m_states;
    uint32 m_elapsedTime = 0;
    uint32 m_updateTimer = 0;
};

#endif
