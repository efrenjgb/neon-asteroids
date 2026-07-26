#pragma once

// Thin wrapper over ENet (UDP) for the authoritative-host netcode. ENet's own
// headers are kept out of this interface (opaque void* for its structs) so the
// rest of the game never sees them — only net.cpp includes <enet/enet.h>.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace net
{

// ENet needs one global init/shutdown per process.
bool GlobalInit();
void GlobalShutdown();

enum class Role      { None, Host, Client };
enum class LinkState { Disconnected, Connecting, Connected };

struct Packet
{
    std::vector<uint8_t> data;
};

// A single point-to-point link — a listening host with one peer, or a client
// connected to one host. Two ENet channels are used: 0 reliable (lifecycle),
// 1 unreliable-sequenced (high-rate input/snapshots).
class NetLink
{
public:
    ~NetLink();

    bool StartHost(uint16_t port);
    bool StartClient(const char* ip, uint16_t port);

    // Drains ENet events into the incoming queue and flushes pending sends.
    // Call every frame. timeoutMs > 0 blocks up to that long waiting for the
    // first event (useful for a paced test loop); 0 is non-blocking.
    void Poll(uint32_t timeoutMs = 0);

    void SendReliable(const uint8_t* data, size_t n);
    void SendUnreliable(const uint8_t* data, size_t n);

    // Pops one received packet; returns false when the queue is empty.
    bool Receive(Packet& out);

    void Shutdown();

    Role      GetRole() const { return role_; }
    LinkState State()   const { return state_; }

private:
    void*     host_  = nullptr;   // ENetHost*
    void*     peer_  = nullptr;   // ENetPeer*
    Role      role_  = Role::None;
    LinkState state_ = LinkState::Disconnected;

    std::vector<Packet> incoming_;
};

}  // namespace net
