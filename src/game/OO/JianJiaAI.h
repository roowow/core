#pragma once
#include "PlayerBots/PlayerBotAI.h"

// Persistent AI companion bot "蒹葭".
// Stays online as a customBot; whisper interception is handled in
// ChatHandler.cpp (no packet parsing needed here).
class JianJiaAI : public PlayerBotAI
{
public:
    explicit JianJiaAI(Player* p = nullptr) : PlayerBotAI(p) {}
    bool OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess) override;
    void OnPacketReceived(WorldPacket const* packet) override;
    void UpdateAI(uint32 diff) override {}
};
