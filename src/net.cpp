#include "net.hpp"

#include <enet/enet.h>

namespace net
{

namespace
{
bool g_inited = false;
}

bool GlobalInit()
{
    if (g_inited) return true;
    if (enet_initialize() != 0) return false;
    g_inited = true;
    return true;
}

void GlobalShutdown()
{
    if (!g_inited) return;
    enet_deinitialize();
    g_inited = false;
}

NetLink::~NetLink()
{
    Shutdown();
}

bool NetLink::StartHost(uint16_t port)
{
    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port;

    ENetHost* h = enet_host_create(&addr, /*peers*/ 1, /*channels*/ 2, 0, 0);
    if (h == nullptr) return false;

    host_  = h;
    role_  = Role::Host;
    state_ = LinkState::Connecting;   // listening; Connected once a peer joins
    return true;
}

bool NetLink::StartClient(const char* ip, uint16_t port)
{
    ENetHost* h = enet_host_create(nullptr, /*peers*/ 1, /*channels*/ 2, 0, 0);
    if (h == nullptr) return false;

    ENetAddress addr;
    if (enet_address_set_host(&addr, ip) != 0)
    {
        enet_host_destroy(h);
        return false;
    }
    addr.port = port;

    ENetPeer* p = enet_host_connect(h, &addr, /*channels*/ 2, 0);
    if (p == nullptr)
    {
        enet_host_destroy(h);
        return false;
    }

    host_  = h;
    peer_  = p;
    role_  = Role::Client;
    state_ = LinkState::Connecting;
    return true;
}

void NetLink::Poll(uint32_t timeoutMs)
{
    if (host_ == nullptr) return;

    auto*     h  = static_cast<ENetHost*>(host_);
    ENetEvent ev;

    // The first service call may block up to timeoutMs; subsequent drains are
    // non-blocking so the whole queue empties in one Poll.
    while (enet_host_service(h, &ev, timeoutMs) > 0)
    {
        timeoutMs = 0;

        switch (ev.type)
        {
            case ENET_EVENT_TYPE_CONNECT:
                peer_  = ev.peer;
                state_ = LinkState::Connected;
                break;

            case ENET_EVENT_TYPE_RECEIVE:
            {
                Packet pkt;
                pkt.data.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
                incoming_.push_back(std::move(pkt));
                enet_packet_destroy(ev.packet);
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT:
                state_ = LinkState::Disconnected;
                peer_  = nullptr;
                break;

            default:
                break;
        }
    }
}

void NetLink::SendReliable(const uint8_t* data, size_t n)
{
    if (peer_ == nullptr) return;
    ENetPacket* pkt = enet_packet_create(data, n, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(static_cast<ENetPeer*>(peer_), /*channel*/ 0, pkt);
}

void NetLink::SendUnreliable(const uint8_t* data, size_t n)
{
    if (peer_ == nullptr) return;
    // Flag 0 = unreliable but sequenced: stale/out-of-order packets are dropped,
    // which is exactly what per-tick input and snapshots want.
    ENetPacket* pkt = enet_packet_create(data, n, 0);
    enet_peer_send(static_cast<ENetPeer*>(peer_), /*channel*/ 1, pkt);
}

bool NetLink::Receive(Packet& out)
{
    if (incoming_.empty()) return false;
    out = std::move(incoming_.front());
    incoming_.erase(incoming_.begin());
    return true;
}

void NetLink::Shutdown()
{
    if (peer_ != nullptr && host_ != nullptr)
    {
        // Ask for a graceful disconnect and pump the host briefly so the notice
        // actually goes out before we tear the socket down.
        enet_peer_disconnect(static_cast<ENetPeer*>(peer_), 0);

        auto*     h = static_cast<ENetHost*>(host_);
        ENetEvent ev;
        for (int i = 0; i < 3; i++)
        {
            while (enet_host_service(h, &ev, 30) > 0)
            {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
            }
        }
        peer_ = nullptr;
    }

    if (host_ != nullptr)
    {
        enet_host_destroy(static_cast<ENetHost*>(host_));
        host_ = nullptr;
    }

    role_  = Role::None;
    state_ = LinkState::Disconnected;
}

}  // namespace net
