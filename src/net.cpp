#include "net.hpp"

#include <enet/enet.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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

std::string LocalIP()
{
    // "Connect" a UDP socket toward a public address. No packet is sent, but the
    // OS picks the interface it would route through, which getsockname then
    // reports — the machine's LAN address in practice. Winsock is already up
    // via enet_initialize on Windows.
#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return "";
#else
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return "";
#endif

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port   = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);

    std::string result;
    if (connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0)
    {
        sockaddr_in local{};
#if defined(_WIN32)
        int len = sizeof(local);
#else
        socklen_t len = sizeof(local);
#endif
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0)
        {
            char buf[INET_ADDRSTRLEN] = {0};
            if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf)) != nullptr)
                result = buf;
        }
    }

#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
    return result;
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
                // A vanished (crashed / force-quit) peer defaults to ~30s before
                // ENet declares it lost. On a LAN that should be a few seconds,
                // so a stranded player returns to the menu promptly.
                enet_peer_timeout(ev.peer, 32, 2000, 5000);
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
    // UNRELIABLE_FRAGMENT keeps the packet unreliable (stale ones are dropped,
    // which is what per-tick input and snapshots want) while still allowing it
    // to fragment when it exceeds the MTU — a busy wave's snapshot can run past
    // ~1400 bytes because every asteroid carries its polygon shape. A plain
    // unreliable packet over MTU would be silently dropped.
    ENetPacket* pkt = enet_packet_create(data, n, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
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
