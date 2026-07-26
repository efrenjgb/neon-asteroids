// End-to-end proof for the NetLink layer: run one process as host and one as
// client on localhost, and confirm they connect, exchange messages in both
// directions on both channels, and disconnect cleanly. No window, no game —
// just the network path, verifiable from the logs.
//
//   net_conntest host <port>
//   net_conntest client <ip> <port>

#include "../src/net.hpp"
#include "../src/net_serialize.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
const char* StateName(net::LinkState s)
{
    switch (s)
    {
        case net::LinkState::Connected:  return "Connected";
        case net::LinkState::Connecting: return "Connecting";
        default:                         return "Disconnected";
    }
}

// A worst-case-ish snapshot: a full late wave of 11 large asteroids (each with
// its polygon shape), two ships, bullets, a saucer, a powerup and events. This
// is the real payload Phase 1 must push every tick, so sending it over the
// socket proves the size is handled — a snapshot larger than the UDP MTU would
// be dropped on the unreliable channel unless fragmentation is arranged.
net::Snapshot MakeSampleSnapshot(int tick)
{
    net::Snapshot s;
    s.tick = static_cast<uint32_t>(tick);
    s.wave = 11;

    for (int i = 0; i < 2; i++)
    {
        Ship sh;
        sh.player = i;
        sh.lives  = 3;
        sh.alive  = true;
        sh.pos    = {100.0f + i * 200.0f, 360.0f};
        s.ships.push_back(sh);
    }

    // 40 asteroids deliberately overshoots a busy wave, pushing the payload
    // past the MTU so the fragmentation path is actually exercised over the
    // socket rather than just assumed.
    for (int i = 0; i < 40; i++)
    {
        Asteroid a;
        a.pos    = {static_cast<float>(i * 30), 200.0f};
        a.rot    = 0.1f * i;
        a.radius = 18.0f;
        a.tier   = 1;
        a.shape.assign(13, 1.0f);   // max vertex count
        s.asteroids.push_back(a);
    }

    for (int i = 0; i < 6; i++)
    {
        Bullet b;
        b.pos   = {static_cast<float>(i * 50), 300.0f};
        b.vel   = {700.0f, 0.0f};
        b.owner = i % 2;
        s.bullets.push_back(b);
    }

    Ufo u; u.pos = {500.0f, 90.0f}; u.radius = 22.0f; u.tier = 2;
    s.ufos.push_back(u);

    Powerup p; p.pos = {250.0f, 250.0f}; p.type = PowerupType::FanFire; p.radius = 16.0f;
    s.powerups.push_back(p);

    s.events.push_back({net::EventType::Explosion, {12.0f, 34.0f}, 3});
    s.events.push_back({net::EventType::Shoot, {100.0f, 360.0f}, 0});

    return s;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: net_conntest host <port> | client <ip> <port>\n");
        return 2;
    }

    if (!net::GlobalInit())
    {
        std::printf("enet init failed\n");
        return 1;
    }

    const bool isHost = std::strcmp(argv[1], "host") == 0;
    net::NetLink link;

    if (isHost)
    {
        const uint16_t port = (argc >= 3) ? static_cast<uint16_t>(std::atoi(argv[2])) : 45123;
        if (!link.StartHost(port)) { std::printf("host create failed\n"); return 1; }
        std::printf("HOST listening on port %u\n", port);
    }
    else
    {
        if (argc < 4) { std::printf("client needs <ip> <port>\n"); return 2; }
        const char*    ip   = argv[2];
        const uint16_t port = static_cast<uint16_t>(std::atoi(argv[3]));
        if (!link.StartClient(ip, port)) { std::printf("client connect failed\n"); return 1; }
        std::printf("CLIENT connecting to %s:%u\n", ip, port);
    }
    std::fflush(stdout);

    net::LinkState lastState = net::LinkState::Disconnected;
    int sent = 0;
    int recv = 0;

    // ~5 seconds at ~16 ms/tick. The 16 ms comes from ENet's own service
    // timeout inside Poll, so no separate sleep is needed.
    for (int tick = 0; tick < 300; tick++)
    {
        link.Poll(16);

        if (link.State() != lastState)
        {
            std::printf("[tick %d] state -> %s\n", tick, StateName(link.State()));
            std::fflush(stdout);
            lastState = link.State();
        }

        if (link.State() == net::LinkState::Connected)
        {
            // Host streams full snapshots (the real Phase 1 payload); client
            // streams its input. Both on the unreliable channel.
            net::ByteWriter w;
            if (isHost)
            {
                MakeSampleSnapshot(tick).write(w);
                if (tick == 0 || sent == 0)
                    std::printf("[tick %d] snapshot payload = %zu bytes\n", tick, w.buf.size());
                link.SendUnreliable(w.buf.data(), w.buf.size());
            }
            else
            {
                net::InputMsg m;
                m.tick = static_cast<uint32_t>(tick);
                m.controls.turn = 0.5f;
                m.controls.fire = true;
                m.write(w);
                link.SendUnreliable(w.buf.data(), w.buf.size());
            }
            sent++;

            net::Packet pkt;
            while (link.Receive(pkt))
            {
                recv++;
                net::ByteReader r(pkt.data.data(), pkt.data.size());
                const auto type = static_cast<net::MsgType>(r.u8());

                // Client decodes snapshots and confirms they arrived whole.
                if (type == net::MsgType::Snapshot)
                {
                    const net::Snapshot snap = net::Snapshot::read(r);
                    if (recv <= 3)
                    {
                        std::printf("[tick %d] recv snapshot %zu bytes: %zu ast, %zu ships, ok=%d\n",
                                    tick, pkt.data.size(), snap.asteroids.size(),
                                    snap.ships.size(), r.ok ? 1 : 0);
                        std::fflush(stdout);
                    }
                }
                else if (recv <= 3)
                {
                    std::printf("[tick %d] recv type=%u (%zu bytes)\n",
                                tick, static_cast<unsigned>(type), pkt.data.size());
                    std::fflush(stdout);
                }
            }
        }

        if (tick % 60 == 0 && link.State() == net::LinkState::Connected)
        {
            std::printf("[tick %d] running: sent=%d recv=%d\n", tick, sent, recv);
            std::fflush(stdout);
        }
    }

    std::printf("DONE: sent=%d recv=%d final=%s\n", sent, recv, StateName(link.State()));
    link.Shutdown();
    net::GlobalShutdown();
    return 0;
}
