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
            // Each side sends a distinct message type on its channel.
            net::ByteWriter w;
            if (isHost)
            {
                net::StartGameMsg m;
                m.seed = 0x1000u + static_cast<uint32_t>(tick);
                m.write(w);
                link.SendReliable(w.buf.data(), w.buf.size());
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
                if (recv <= 3)
                {
                    net::ByteReader r(pkt.data.data(), pkt.data.size());
                    const uint8_t type = r.u8();
                    std::printf("[tick %d] recv type=%u (%zu bytes)\n",
                                tick, type, pkt.data.size());
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
