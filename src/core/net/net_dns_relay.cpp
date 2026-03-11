// NetBackend: DNS relay.
// The gateway (10.0.2.2) acts as the DNS server advertised via DHCP.
// Incoming DNS queries are forwarded to the host's current nameserver,
// so network changes on the host are transparent to the guest.

#include "core/net/net_compat.h"
#include "core/net/net_backend.h"
#include "core/net/net_packet.h"
#include "core/net/frame_builder.h"
#include "core/vmm/types.h"

#include <uv.h>
#include <cstring>

void NetBackend::HandleDnsQuery(const uint8_t* frame, uint32_t len,
                                 uint32_t ip_hdr_len, uint16_t src_port) {
    uint32_t udp_off = sizeof(EthHdr) + ip_hdr_len + sizeof(UdpHdr);
    if (len <= udp_off) return;
    uint32_t payload_len = len - udp_off;
    const uint8_t* dns_payload = frame + udp_off;

    uint32_t dns_server = GetHostDnsServer();

    SocketHandle s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == SOCK_INVALID) {
        LOG_ERROR("DNS relay: failed to create UDP socket");
        return;
    }
    SOCK_SETNONBLOCK(s);

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = htonl(dns_server);
    dest.sin_port = htons(53);

    sendto(s, SOCK_CCAST(dns_payload), static_cast<int>(payload_len), 0,
           reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

    auto session = std::make_unique<DnsSession>();
    session->backend = this;
    session->host_socket = static_cast<uintptr_t>(s);
    session->guest_src_port = src_port;
#ifdef _WIN32
    session->created_ms = GetTickCount64();
#else
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        session->created_ms = static_cast<uint64_t>(ts.tv_sec) * 1000 +
                              static_cast<uint64_t>(ts.tv_nsec) / 1000000;
    }
#endif

    session->poll.Init(&loop_, s);
    auto* sess_ptr = session.get();
    session->poll.Start(UV_READABLE, [](uv_poll_t* h, int status, int) {
        if (status < 0) return;
        auto* sess = static_cast<DnsSession*>(h->data);
        sess->backend->HandleDnsReadable(sess->host_socket, sess->guest_src_port);
        sess->poll.Close();
        SOCK_CLOSE(static_cast<SocketHandle>(sess->host_socket));
        sess->host_socket = ~(uintptr_t)0;
    }, sess_ptr);

    dns_sessions_.push_back(std::move(session));
}

void NetBackend::HandleDnsReadable(uintptr_t sock, uint16_t guest_src_port) {
    SocketHandle s = static_cast<SocketHandle>(sock);
    char buf[2048];
    struct sockaddr_in from{};
    socklen_t fromlen = sizeof(from);
    int n = recvfrom(s, buf, sizeof(buf), 0,
                     reinterpret_cast<sockaddr*>(&from), &fromlen);
    if (n <= 0) return;

    auto frame = frame::BuildUdpFrame(
        kGuestMac, kGatewayMac,
        kGatewayIp, kGuestIp,
        53, guest_src_port,
        buf, static_cast<uint32_t>(n));

    InjectFrame(frame.data(), static_cast<uint32_t>(frame.size()));
}
