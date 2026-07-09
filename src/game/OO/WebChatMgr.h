#pragma once
#include "Common.h"
#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>

struct redisContext;

class WebChatMgr
{
public:
    static WebChatMgr& instance();

    void Initialize(char const* socketPath, uint32 realmId);
    void Shutdown();
    void Update(); // drain pending web→game messages; call from World::Update()

    // game→web: serialize to JSON and publish to Redis
    void WriteWebChat(std::string const& channel,
                      std::string const& charName,
                      uint32 faction,
                      uint32 classId,
                      std::string const& recipient,
                      std::string const& msg);

    void WriteBroadcast(std::string const& msg);

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
                                 std::string const& msg);
    static std::string EscapeJson(std::string const& s);
    static std::string StripColorCodes(std::string const& s);
    static std::string JsonGetStr(std::string const& json, char const* key);
    static uint32      JsonGetU32(std::string const& json, char const* key);

    std::string   m_socketPath;
    std::string   m_keyLive;    // web_chat:live:<realmId>
    std::string   m_keyHistory; // web_chat:history:<realmId>
    redisContext* m_pubCtx = nullptr;
    redisContext* m_subCtx = nullptr;

    std::thread       m_subThread;
    std::atomic<bool> m_stop{false};

    std::mutex              m_queueMutex;
    std::queue<std::string> m_pending;
};

#define sWebChatMgr WebChatMgr::instance()
