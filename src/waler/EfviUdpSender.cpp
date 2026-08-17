#include <waler/EfviUdpSender.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>

#include <waler/util.hpp>

extern "C" {
  #include <etherfabric/base.h>
}

namespace waler {

namespace {

void checkOk(int rc, const char* what) {
  if (rc < 0) {
    std::fprintf(stderr, "EfviUdpSender: %s failed: rc=%d errno=%d (%s)\n",
                 what, rc, errno, std::strerror(errno));
    std::abort();
  }
}

// Standard 16-bit one's-complement checksum (used for the IPv4 header).
uint16_t ipChecksum(const void* data, std::size_t len) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::uint32_t sum = 0;
  while (len > 1) {
    sum += (static_cast<std::uint32_t>(p[0]) << 8) | p[1];
    p += 2;
    len -= 2;
  }
  if (len == 1) {
    sum += static_cast<std::uint32_t>(p[0]) << 8;
  }
  while (sum >> 16) {
    sum = (sum & 0xFFFFu) + (sum >> 16);
  }
  return static_cast<std::uint16_t>(~sum);
}

}  // namespace

EfviUdpSender::EfviUdpSender(const std::string& ifname,
                             const std::string& src_ip, uint16_t src_port,
                             const std::string& dst_ip, uint16_t dst_port,
                             const std::array<std::uint8_t, 6>& dst_mac) {
  const unsigned ifindex = if_nametoindex(ifname.c_str());
  if (ifindex == 0) {
    std::fprintf(stderr, "EfviUdpSender: no such interface: %s\n", ifname.c_str());
    std::abort();
  }

  checkOk(ef_driver_open(&dh_), "ef_driver_open");
  checkOk(ef_pd_alloc(&pd_, dh_, static_cast<int>(ifindex), EF_PD_DEFAULT), "ef_pd_alloc");
  checkOk(ef_vi_alloc_from_pd(&vi_, dh_, &pd_, dh_,
                              -1 /* evq */, -1 /* rxq */, -1 /* txq */,
                              nullptr, -1, EF_VI_TX_TIMESTAMPS),
          "ef_vi_alloc_from_pd");

  const size_t pool_bytes = kSlotLen * static_cast<size_t>(kTxSlots);
  void* pool_mem = nullptr;
  if (posix_memalign(&pool_mem, 4096, pool_bytes) != 0) {
    std::fprintf(stderr, "EfviUdpSender: posix_memalign(%zu) failed\n", pool_bytes);
    std::abort();
  }
  pool_ = static_cast<uint8_t*>(pool_mem);
  std::memset(pool_, 0, pool_bytes);

  checkOk(ef_memreg_alloc(&mr_, dh_, &pd_, dh_, pool_, pool_bytes), "ef_memreg_alloc");

  const std::array<std::uint8_t, 6> src_mac = getInterfaceMac(ifname);

  in_addr src_addr{};
  in_addr dst_addr{};
  if (inet_pton(AF_INET, src_ip.c_str(), &src_addr) != 1 ||
      inet_pton(AF_INET, dst_ip.c_str(), &dst_addr) != 1) {
    std::fprintf(stderr, "EfviUdpSender: bad src_ip/dst_ip\n");
    std::abort();
  }

  // Write the (mostly-fixed) header template into every slot up front;
  // send() only ever has to patch the length fields, IP checksum, and
  // payload bytes.
  for (unsigned i = 0; i < kTxSlots; ++i) {
    uint8_t* frame = slot(i);

    // -- Ethernet --
    std::memcpy(frame + 0, dst_mac.data(), 6);
    std::memcpy(frame + 6, src_mac.data(), 6);
    frame[12] = 0x08;
    frame[13] = 0x00;  // ethertype IPv4

    // -- IPv4 (no options) --
    uint8_t* ip = frame + kEthLen;
    ip[0] = 0x45;  // version 4, IHL 5 (20 bytes)
    ip[1] = 0x00;  // TOS
    // ip[2..3]  total length, patched per send
    // ip[4..5]  identification, patched per send
    ip[6] = 0x00;
    ip[7] = 0x00;  // flags/fragment offset: none
    ip[8] = 64;    // TTL
    ip[9] = IPPROTO_UDP;
    // ip[10..11] checksum, patched per send
    std::memcpy(ip + 12, &src_addr, 4);
    std::memcpy(ip + 16, &dst_addr, 4);

    // -- UDP --
    uint8_t* udp = ip + kIp4Len;
    const uint16_t src_port_be = htons(src_port);
    const uint16_t dst_port_be = htons(dst_port);
    std::memcpy(udp + 0, &src_port_be, 2);
    std::memcpy(udp + 2, &dst_port_be, 2);
    // udp[4..5] length, patched per send
    udp[6] = 0x00;
    udp[7] = 0x00;  // checksum: 0 = disabled, legal for IPv4
  }
}

EfviUdpSender::~EfviUdpSender() {
  ef_memreg_free(&mr_, dh_);
  ef_vi_free(&vi_, dh_);
  ef_pd_free(&pd_, dh_);
  ef_driver_close(dh_);
  std::free(pool_);
}

int EfviUdpSender::send(const void* payload, std::size_t payload_len) {
  if (payload_len > kMaxPayload) {
    return -1;
  }

  const unsigned slot_idx = next_slot_;
  next_slot_ = (next_slot_ + 1) % kTxSlots;

  uint8_t* frame = slot(slot_idx);
  uint8_t* ip    = frame + kEthLen;
  uint8_t* udp   = ip + kIp4Len;
  uint8_t* data  = udp + kUdpLen;

  std::memcpy(data, payload, payload_len);

  const std::size_t udp_len = kUdpLen + payload_len;
  const std::size_t ip_total_len = kIp4Len + udp_len;
  const std::size_t frame_len = kHeaderLen + payload_len < kMinFrameLen
                                     ? kMinFrameLen
                                     : kHeaderLen + payload_len;
  if (frame_len > kHeaderLen + payload_len) {
    std::memset(data + payload_len, 0, frame_len - (kHeaderLen + payload_len));
  }

  const uint16_t total_len_be = htons(static_cast<uint16_t>(ip_total_len));
  const uint16_t ip_id_be     = htons(ip_id_++);
  std::memcpy(ip + 2, &total_len_be, 2);
  std::memcpy(ip + 4, &ip_id_be, 2);
  ip[10] = 0;
  ip[11] = 0;
  const uint16_t ip_csum = htons(ipChecksum(ip, kIp4Len));
  std::memcpy(ip + 10, &ip_csum, 2);

  const uint16_t udp_len_be = htons(static_cast<uint16_t>(udp_len));
  std::memcpy(udp + 4, &udp_len_be, 2);

  const ef_addr dma = ef_memreg_dma_addr(&mr_, static_cast<size_t>(slot_idx) * kSlotLen);
  const int rc = ef_vi_transmit(&vi_, dma, static_cast<int>(frame_len), next_dma_id_++);
  return rc < 0 ? -1 : 0;
}

void EfviUdpSender::reapCompletions() {
  ef_event evs[16];
  const int n = ef_eventq_poll(&vi_, evs, 16);
  (void)n;  // TX completions are drained purely to keep the event queue
            // from filling up; timestamps aren't consumed yet (that's B0/B1,
            // out of scope for the receive-time measurement this is for).
}

}  // namespace waler
