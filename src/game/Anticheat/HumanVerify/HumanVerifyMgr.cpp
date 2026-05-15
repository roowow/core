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

#include "HumanVerifyMgr.h"
#include "GossipDef.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"

#include <algorithm>
#include <vector>

namespace
{
uint32 const HUMAN_VERIFY_UPDATE_INTERVAL = 1 * IN_MILLISECONDS;
uint32 const HUMAN_VERIFY_GOSSIP_SENDER = 0xA17C0001;
uint32 const HUMAN_VERIFY_GOSSIP_ACTION_BASE = 0xA17C0100;

char const* HumanVerifyResultName(HumanVerifyResult result)
{
    switch (result)
    {
        case HumanVerifyResult::Passed:
            return "passed";
        case HumanVerifyResult::FailedAnswer:
            return "failed_answer";
        case HumanVerifyResult::FailedTooManyAttempts:
            return "failed_too_many_attempts";
        case HumanVerifyResult::TimedOut:
            return "timed_out";
        default:
            return "not_handled";
    }
}
}

bool HumanVerifyMgr::StartVerify(Player* player, HumanVerifyRequest const& request)
{
    if (!player || player->IsBot() || (player->IsGameMaster() && !request.allowGameMaster))
        return false;

    ObjectGuid const guid = player->GetObjectGuid();
    HumanVerifyState& state = m_states[guid];
    state.expireTime = m_elapsedTime + request.timeoutMs;
    state.failures = 0;
    state.maxFailures = request.maxFailures ? request.maxFailures : 3;
    state.reason = request.reason;

    BuildQuestion(state);
    SendGossip(player, state);

    sLog.Out(LOG_ANTICHEAT, LOG_LVL_BASIC, "[HumanVerify] started player %s (%s) reason %s timeoutMs %u.",
        player->GetName(), guid.GetString().c_str(), state.reason.c_str(), request.timeoutMs);
    return true;
}

void HumanVerifyMgr::CancelVerify(ObjectGuid guid)
{
    m_states.erase(guid);
}

void HumanVerifyMgr::Update(uint32 diff)
{
    m_elapsedTime += diff;
    m_updateTimer += diff;
    if (m_updateTimer < HUMAN_VERIFY_UPDATE_INTERVAL)
        return;

    m_updateTimer = 0;

    for (auto itr = m_states.begin(); itr != m_states.end();)
    {
        if (m_elapsedTime < itr->second.expireTime)
        {
            ++itr;
            continue;
        }

        ObjectGuid guid = itr->first;
        HumanVerifyState state = itr->second;
        itr = m_states.erase(itr);

        if (Player* player = sObjectMgr.GetPlayer(guid))
            Finish(player, state, HumanVerifyResult::TimedOut);
        else
            sLog.Out(LOG_ANTICHEAT, LOG_LVL_BASIC, "[HumanVerify] timed out offline player %s reason %s.",
                guid.GetString().c_str(), state.reason.c_str());
    }
}

bool HumanVerifyMgr::IsPending(ObjectGuid guid) const
{
    return m_states.find(guid) != m_states.end();
}

HumanVerifyResult HumanVerifyMgr::HandleGossipSelect(Player* player, uint32 sender, uint32 action)
{
    if (!player || sender != HUMAN_VERIFY_GOSSIP_SENDER || action < HUMAN_VERIFY_GOSSIP_ACTION_BASE)
        return HumanVerifyResult::NotHandled;

    auto itr = m_states.find(player->GetObjectGuid());
    if (itr == m_states.end())
        return HumanVerifyResult::NotHandled;

    uint32 const selectedAnswer = action - HUMAN_VERIFY_GOSSIP_ACTION_BASE;
    HumanVerifyState& state = itr->second;
    if (selectedAnswer == state.correctAnswer)
    {
        HumanVerifyState finishedState = state;
        m_states.erase(itr);
        Finish(player, finishedState, HumanVerifyResult::Passed);
        return HumanVerifyResult::Passed;
    }

    ++state.failures;
    if (state.failures >= state.maxFailures)
    {
        HumanVerifyState finishedState = state;
        m_states.erase(itr);
        Finish(player, finishedState, HumanVerifyResult::FailedTooManyAttempts);
        return HumanVerifyResult::FailedTooManyAttempts;
    }

    BuildQuestion(state);
    SendGossip(player, state);
    sLog.Out(LOG_ANTICHEAT, LOG_LVL_BASIC, "[HumanVerify] wrong answer player %s (%s) failures %u/%u reason %s.",
        player->GetName(), player->GetObjectGuid().GetString().c_str(), uint32(state.failures), uint32(state.maxFailures), state.reason.c_str());
    return HumanVerifyResult::FailedAnswer;
}

void HumanVerifyMgr::BuildQuestion(HumanVerifyState& state) const
{
    state.question = urand(10, 99);
    state.correctAnswer = state.question;

    std::vector<uint32> options;
    options.reserve(state.options.size());
    options.push_back(state.correctAnswer);

    while (options.size() < state.options.size())
    {
        uint32 candidate = urand(10, 99);
        if (std::find(options.begin(), options.end(), candidate) == options.end())
            options.push_back(candidate);
    }

    for (uint8 i = 0; i < state.options.size(); ++i)
    {
        uint32 const swapIndex = urand(i, state.options.size() - 1);
        std::swap(options[i], options[swapIndex]);
        state.options[i] = options[i];
    }
}

void HumanVerifyMgr::SendGossip(Player* player, HumanVerifyState const& state) const
{
    if (!player || !player->PlayerTalkClass)
        return;

    PlayerMenu* menu = player->PlayerTalkClass;
    menu->ClearMenus();

    // Push question text into client NPC-text cache at id=0, then reference it via SendGossipMenu(0)
    std::string questionText = "请选择正确的数字：" + std::to_string(state.question);
    menu->SendTalking("人机验证", questionText.c_str());

    for (uint32 option : state.options)
    {
        std::string label = std::to_string(option);
        menu->GetGossipMenu().AddMenuItem(GOSSIP_ICON_CHAT, label.c_str(), HUMAN_VERIFY_GOSSIP_SENDER, HUMAN_VERIFY_GOSSIP_ACTION_BASE + option);
    }

    menu->SendGossipMenu(0, player->GetObjectGuid());
}

void HumanVerifyMgr::Finish(Player* player, HumanVerifyState const& state, HumanVerifyResult result)
{
    if (!player)
        return;

    if (player->PlayerTalkClass)
        player->PlayerTalkClass->CloseGossip();

    if (result == HumanVerifyResult::Passed)
        player->SendSysMessage("人机验证通过。");
    else if (result == HumanVerifyResult::TimedOut)
        player->SendSysMessage("人机验证超时，未在规定时间内作答。");
    else
        player->SendSysMessage("人机验证失败，答错次数过多。");

    sLog.Out(LOG_ANTICHEAT, LOG_LVL_BASIC, "[HumanVerify] finished player %s (%s) result %s failures %u/%u reason %s.",
        player->GetName(), player->GetObjectGuid().GetString().c_str(), HumanVerifyResultName(result),
        uint32(state.failures), uint32(state.maxFailures), state.reason.c_str());
}
