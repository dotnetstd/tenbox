// NetBackend: ICMP relay.
// Windows: uses IcmpSendEcho via a dedicated worker thread (no admin required).
// POSIX: uses SOCK_DGRAM/IPPROTO_ICMP raw socket with libuv poll.

#include "core/net/net_compat.h"
#include "core/net/net_backend.h"
#include "core/net/net_packet.h"
#include "core/net/frame_builder.h"
#include "core/vmm/types.h"

#include <uv.h>

#ifdef _WIN32
// ============================================================
// Windows: IcmpSendEcho worker thread
// ============================================================

#include <icmpapi.h>

void NetBackend::HandleIcmpOut(uint32_t src_ip, uint32_t dst_ip,
                                const uint8_t* icmp_data, uint32_t icmp_len) {
    if (icmp_len < 8) return;

    // Only relay echo requests (type 8)
    if (icmp_data[0] != 8) return;

    if (!icmp_running_) {
        icmp_handle_ = IcmpCreateFile();
        if (icmp_handle_ == INVALID_HANDLE_VALUE) {
            LOG_ERROR("IcmpCreateFile failed (error %lu)", GetLastError());
            icmp_handle_ = nullptr;
            return;
        }
        icmp_running_ = true;
        icmp_thread_ = std::thread(&NetBackend::IcmpWorkerThread, this);
    }

    IcmpRequest req;
    req.src_ip = src_ip;
    req.dst_ip = dst_ip;
    req.icmp_data.assign(icmp_data, icmp_data + icmp_len);

    {
        std::lock_guard<std::mutex> lk(icmp_mutex_);
        icmp_queue_.push_back(std::move(req));
    }
    icmp_cv_.notify_one();
}

void NetBackend::IcmpWorkerThread() {
    while (icmp_running_) {
        IcmpRequest req;
        {
            std::unique_lock<std::mutex> lk(icmp_mutex_);
            icmp_cv_.wait(lk, [this] { return !icmp_queue_.empty() || !icmp_running_; });
            if (!icmp_running_) break;
            req = std::move(icmp_queue_.front());
            icmp_queue_.erase(icmp_queue_.begin());
        }

        // ICMP echo request: type(1) code(1) checksum(2) id(2) seq(2) payload(...)
        uint16_t icmp_id = 0, icmp_seq = 0;
        if (req.icmp_data.size() >= 8) {
            icmp_id  = (req.icmp_data[4] << 8) | req.icmp_data[5];
            icmp_seq = (req.icmp_data[6] << 8) | req.icmp_data[7];
        }

        const uint8_t* echo_payload = req.icmp_data.data() + 8;
        uint32_t echo_payload_len = static_cast<uint32_t>(req.icmp_data.size()) - 8;

        uint32_t reply_size = sizeof(ICMP_ECHO_REPLY) + echo_payload_len + 8 + 16;
        std::vector<uint8_t> reply_buf(reply_size);

        DWORD ret = IcmpSendEcho(
            icmp_handle_,
            htonl(req.dst_ip),
            const_cast<uint8_t*>(echo_payload),
            static_cast<WORD>(echo_payload_len),
            nullptr,   // IP_OPTION_INFORMATION
            reply_buf.data(),
            static_cast<DWORD>(reply_buf.size()),
            4000       // 4 second timeout
        );

        if (ret == 0) continue;

        auto* reply = reinterpret_cast<ICMP_ECHO_REPLY*>(reply_buf.data());
        uint32_t from_ip = ntohl(reply->Address);

        // Build ICMP echo reply: type=0, code=0
        uint32_t reply_data_len = reply->DataSize;
        uint32_t icmp_total = 8 + reply_data_len;
        std::vector<uint8_t> icmp_reply(icmp_total);
        icmp_reply[0] = 0;  // type: echo reply
        icmp_reply[1] = 0;  // code
        icmp_reply[2] = 0;  // checksum (computed below)
        icmp_reply[3] = 0;
        icmp_reply[4] = static_cast<uint8_t>(icmp_id >> 8);
        icmp_reply[5] = static_cast<uint8_t>(icmp_id & 0xFF);
        icmp_reply[6] = static_cast<uint8_t>(icmp_seq >> 8);
        icmp_reply[7] = static_cast<uint8_t>(icmp_seq & 0xFF);

        if (reply_data_len > 0 && reply->Data)
            memcpy(icmp_reply.data() + 8, reply->Data, reply_data_len);

        // Compute ICMP checksum
        uint32_t sum = ChecksumPartial(icmp_reply.data(), static_cast<int>(icmp_total));
        uint16_t cksum = ChecksumFold(sum);
        icmp_reply[2] = static_cast<uint8_t>(cksum >> 8);
        icmp_reply[3] = static_cast<uint8_t>(cksum & 0xFF);

        auto frame = frame::BuildIpFrame(
            kGuestMac, kGatewayMac,
            from_ip, kGuestIp, IPPROTO_ICMP,
            icmp_reply.data(), static_cast<uint32_t>(icmp_reply.size()));

        InjectFrame(frame.data(), static_cast<uint32_t>(frame.size()));
    }
}

#else
// ============================================================
// POSIX: SOCK_DGRAM/IPPROTO_ICMP with libuv poll
// ============================================================

void NetBackend::HandleIcmpOut(uint32_t src_ip, uint32_t dst_ip,
                                const uint8_t* icmp_data, uint32_t icmp_len) {
    if (icmp_socket_ == ~(uintptr_t)0) {
        SocketHandle s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
        if (s == SOCK_INVALID) {
            LOG_ERROR("Failed to create ICMP socket (need admin?)");
            return;
        }
        SOCK_SETNONBLOCK(s);
        icmp_socket_ = static_cast<uintptr_t>(s);
        uv_poll_init_socket(&loop_, &icmp_poll_, s);
        icmp_poll_.data = this;
        uv_poll_start(&icmp_poll_, UV_READABLE, [](uv_poll_t* h, int status, int) {
            if (status < 0) return;
            static_cast<NetBackend*>(h->data)->HandleIcmpReadable();
        });
        icmp_poll_active_ = true;
    }

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = htonl(dst_ip);
    sendto(static_cast<SocketHandle>(icmp_socket_),
           SOCK_CCAST(icmp_data),
           static_cast<int>(icmp_len), 0,
           reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
}

void NetBackend::HandleIcmpReadable() {
    if (icmp_socket_ == ~(uintptr_t)0) return;
    SocketHandle s = static_cast<SocketHandle>(icmp_socket_);

    for (;;) {
        char buf[2048];
        struct sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n <= 0) break;
        if (n < 20) continue;
        auto* recv_ip = reinterpret_cast<IpHdr*>(buf);
        uint32_t recv_ip_hdr_len = (recv_ip->ver_ihl & 0xF) * 4;
        if (static_cast<uint32_t>(n) < recv_ip_hdr_len + 8) continue;

        const uint8_t* icmp_payload = reinterpret_cast<const uint8_t*>(buf) + recv_ip_hdr_len;
        uint32_t icmp_len = n - recv_ip_hdr_len;
        uint32_t from_ip = ntohl(from.sin_addr.s_addr);

        auto frame = frame::BuildIpFrame(
            kGuestMac, kGatewayMac,
            from_ip, kGuestIp, IPPROTO_ICMP,
            icmp_payload, icmp_len);

        InjectFrame(frame.data(), static_cast<uint32_t>(frame.size()));
    }
}

#endif
