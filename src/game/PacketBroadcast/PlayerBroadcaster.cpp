#include "PlayerBroadcaster.h"

#include "MovementBroadcaster.h"
#include "WorldPacket.h"
#include "WorldSocket.h"
#include "Player.h"

uint32 PlayerBroadcaster::num_bcaster_created = 0;
uint32 PlayerBroadcaster::num_bcaster_deleted = 0;

PlayerBroadcaster::PlayerBroadcaster(std::shared_ptr<WorldSocket> socket, ObjectGuid const& self, std::size_t max_queue)
    : MAX_QUEUE_SIZE(max_queue), m_socket(std::move(socket)), m_self(self), instanceId(0), lastUpdatePackets(0)
{
    m_queue.reserve(max_queue);
    ++num_bcaster_created;
}

void PlayerBroadcaster::ChangeSocket(std::shared_ptr<WorldSocket> const& new_socket)
{
    m_socket = new_socket;
}

void PlayerBroadcaster::AddListener(Player const* player)
{
    ASSERT(player);
    if (player->GetObjectGuid() == m_self)
        return;

    std::lock_guard<std::mutex> guard(m_listeners_lock);
    m_listeners[player->GetObjectGuid()] = player->m_broadcaster;
}

void PlayerBroadcaster::RemoveListener(Player const* player)
{
    ASSERT(player);
    std::lock_guard<std::mutex> guard(m_listeners_lock);
    m_listeners.erase(player->GetObjectGuid());
}

void PlayerBroadcaster::ClearListeners()
{
    std::lock_guard<std::mutex> guard(m_listeners_lock);
    m_listeners.clear();
}

void PlayerBroadcaster::SendPacket(WorldPacket const& packet)
{
    if (m_socket)
        m_socket->SendPacket(packet);
}

void PlayerBroadcaster::ProcessQueue(uint32& num_packets)
{
    if (m_queue.empty())
        return;

    std::unique_lock<std::mutex> q_g(m_queue_lock), v_g(m_listeners_lock);
    auto queue = std::move(m_queue);
    q_g.unlock();

    // selfOnly entries (QueueSelfOnlyPacket()) always cost exactly 1 send, not one per listener -
    // count them separately so this estimate (used for slow-instance detection) stays meaningful
    // now that combat log traffic can share this same queue with movement.
    std::size_t selfOnlyCount = 0;
    for (auto const& data : queue)
        if (data.selfOnly)
            ++selfOnlyCount;
    lastUpdatePackets = uint32(selfOnlyCount + (queue.size() - selfOnlyCount) * m_listeners.size());
    num_packets += lastUpdatePackets;

    for (auto& data : queue)
    {
        // Combat log entry queued for this player alone (QueueSelfOnlyPacket()) - deliver to
        // self only, never fan out to m_listeners (that list means "watching my movement", not
        // "wants a copy of every bystander event I happen to also receive").
        if (data.selfOnly)
        {
            SendPacket(data.packet);
            continue;
        }

        // Send to self?
        if (data.sendToSelf && data.except != GetGUID())
            SendPacket(data.packet);

        for (auto it = m_listeners.begin(); it != m_listeners.end(); ++it)
        {
            if (it->first == data.except)
                continue;

            it->second->SendPacket(data.packet);
        }
    }
}

void PlayerBroadcaster::QueuePacket(WorldPacket packet, bool self, ObjectGuid except)
{
    BroadcastData data;
    data.packet = std::move(packet);
    data.sendToSelf = self;
    data.except = except;

    std::unique_lock<std::mutex> guard(m_queue_lock, std::defer_lock);

    guard.lock();

    // We need to drop a packet here - if possible
    if (m_queue.size() >= MAX_QUEUE_SIZE)
    {
        BroadcastData& last_in_queue = m_queue[m_queue.size() - 1];
        // Phase "网络带宽优化" 序3 (see HPHA.md) - the queue is now shared with combat log
        // entries (QueueSelfOnlyPacket()), whose opcodes CanSkipPacket() was never taught to
        // recognize (it's a movement-opcode-range heuristic). Must check selfOnly/allowDrop
        // first, or a movement packet could silently evict a raid combat log entry that was
        // explicitly marked non-droppable (allowDrop=false, 序5) - the exact "never drop"
        // guarantee this whole design exists to protect.
        bool const lastSkippable = last_in_queue.selfOnly ? last_in_queue.allowDrop
                                                            : CanSkipPacket(last_in_queue.packet.GetOpcode());
        if (lastSkippable && CanSkipPacket(packet.GetOpcode()))
        {
            m_queue[m_queue.size() - 1] = std::move(data);
            guard.unlock();
            return;
        }
    }
    m_queue.emplace_back(std::move(data));

    guard.unlock();
}

void PlayerBroadcaster::QueueSelfOnlyPacket(WorldPacket packet, bool allowDrop)
{
    BroadcastData data;
    data.sendToSelf = true;
    data.selfOnly = true;
    data.allowDrop = allowDrop;
    data.packet = std::move(packet);

    std::unique_lock<std::mutex> guard(m_queue_lock, std::defer_lock);
    guard.lock();

    if (m_queue.size() >= MAX_QUEUE_SIZE)
    {
        BroadcastData& last_in_queue = m_queue[m_queue.size() - 1];
        bool const lastSkippable = last_in_queue.selfOnly ? last_in_queue.allowDrop
                                                            : CanSkipPacket(last_in_queue.packet.GetOpcode());
        if (lastSkippable && allowDrop)
        {
            m_queue[m_queue.size() - 1] = std::move(data);
            guard.unlock();
            return;
        }
        // Not droppable (raid) - fall through and grow the queue past MAX_QUEUE_SIZE rather than
        // lose a stats-relevant entry. Same "soft cap, hard for non-skippable content" behavior
        // QueuePacket() already has for movement's own protected opcodes, just reused here.
    }
    m_queue.emplace_back(std::move(data));

    guard.unlock();
}

ObjectGuid PlayerBroadcaster::GetGUID() const
{
    return m_self;
}

void PlayerBroadcaster::FreeAtLogout()
{
    m_socket = nullptr;
    std::unique_lock<std::mutex> q_g(m_queue_lock), v_g(m_listeners_lock);
    m_queue.clear();
    m_listeners.clear();
}

PlayerBroadcaster::~PlayerBroadcaster()
{
    m_socket = nullptr;
    ++num_bcaster_deleted;
}
