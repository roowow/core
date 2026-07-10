#pragma once
#include "Common.h"
#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>

struct redisContext;
class BattleGround;
class Player;

class WebChatMgr
{
public:
    static WebChatMgr& instance();

    void Initialize(char const* socketPath, uint32 realmId);
    void Shutdown();
    void Update(); // drain pending web→game messages; call from World::Update()

    // game→web: serialize to JSON and publish to Redis
    // contextId: group_id for party/raid, guild_id for guild, 0 otherwise
    void WriteWebChat(std::string const& channel,
                      std::string const& charName,
                      uint32 faction,
                      uint32 classId,
                      std::string const& recipient,
                      std::string const& msg,
                      uint32 contextId = 0);

    void WriteBroadcast(std::string const& msg);

    // web presence — returns true if charName is currently viewing webchat
    bool IsWebOnline(std::string const& charName) const;

    // 蒹葭 AI companion
    bool        IsJianJiaName(std::string const& name) const;
    bool        IsJianJiaActive() const { return !m_jianJiaName.empty() && m_pubCtx != nullptr; }
    void        ForwardWhisperToJianJia(std::string const& senderName, std::string const& message,
                    uint8 level = 0, uint8 cls = 0, uint8 race = 0,
                    std::string const& zone = "");
    void        ForwardGroupChatToJianJia(std::string const& senderName, std::string const& message,
                    char const* chatContext, uint32 groupId = 0);  // chatContext: "party"/"raid"/"bg"
    bool        NotifyBgAfkViaJianJia(Player* player, BattleGround* bg, uint8 stage, uint8 afkLevel,
                    char const* noticeType);   // returns false → caller should use fallback
    void        SpeakAsJianJia(std::string const& targetName, std::string const& message, ChatMsg groupType = CHAT_MSG_WHISPER);
    void        SpeakInBgAsJianJia(std::string const& targetName, std::string const& message);
    void        UpdateJianJia(); // drain pending AI replies; call from main thread
    void        SetJianJiaName(std::string const& name) { m_jianJiaName = name; }

private:
    void SubscribeThread();
    void DispatchWebMessage(std::string const& json); // main thread only

    void Publish(std::string const& json);
    void ReconnectPub();

    static std::string BuildJson(std::string const& source,
                                 std::string const& channel,
                                 std::string const& charName,
                                 uint32 faction,
                                 uint32 classId,
                                 std::string const& recipient,
                                 std::string const& msg,
                                 uint32 contextId = 0);
    static std::string EscapeJson(std::string const& s);
    static std::string StripColorCodes(std::string const& s);
    static std::string JsonGetStr(std::string const& json, char const* key);
    static uint32      JsonGetU32(std::string const& json, char const* key);

    std::string   m_socketPath;
    std::string   m_keyLive;        // web_chat:live:<realmId>
    std::string   m_keyHistory;     // web_chat:history:<realmId>
    std::string   m_keyJianJiaIn;   // web_chat:jianjia_in:<realmId>   (C++ → Python)
    std::string   m_keyJianJiaOut;  // web_chat:jianjia_out:<realmId>  (Python → C++)
    std::string   m_keyWebOnline;   // web_chat:web_online:<realmId>   prefix for presence keys
    uint32        m_realmId = 0;
    std::string   m_jianJiaName;  // in-game character name
    redisContext* m_pubCtx = nullptr;
    redisContext* m_subCtx = nullptr;

    std::thread       m_subThread;
    std::atomic<bool> m_stop{false};
    std::atomic<int>  m_subFd{-1};   // fd of the active subscribe socket; -1 when not connected

    mutable std::mutex      m_pubMutex;        // guards m_pubCtx and all Redis pub commands
    std::mutex              m_queueMutex;
    std::queue<std::string> m_pending;        // web chat messages (web→game)
    std::queue<std::string> m_jianJiaPending; // AI companion replies from Python
};

#define sWebChatMgr WebChatMgr::instance()
