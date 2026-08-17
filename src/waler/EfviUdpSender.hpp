#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
  #include <etherfabric/memreg.h>
  #include <etherfabric/pd.h>
  #include <etherfabric/vi.h>
}

namespace waler {

// Minimal ef_vi TX path: hand-crafts an Ethernet+IPv4+UDP frame template
// for one src/dst pair and rewrites the payload region in place for each
// send. Bypasses the kernel network stack entirely -- the peer does not
// need an OS-assigned IP, only a hardware UDP filter for the chosen
// dst ip:port (see EfviInterface).
//
// Uses a small ring of TX buffers so consecutive sends don't overwrite a
// frame the NIC hasn't finished DMA'ing out yet; reapCompletions() drains
// the TX completion queue so it doesn't back up over a long run.
class EfviUdpSender {
public:
  EfviUdpSender(const std::string &ifname,
               const std::string &src_ip, uint16_t src_port,
               const std::string &dst_ip, uint16_t dst_port,
               const std::array<std::uint8_t, 6> &dst_mac);
  ~EfviUdpSender();

  EfviUdpSender(const EfviUdpSender&) = delete;
  EfviUdpSender &operator=(const EfviUdpSender&) = delete;

  // Sends one UDP datagram containing `payload`. Non-blocking; does not
  // wait for the TX completion event. Returns 0 on success, -1 if
  // payload_len exceeds the frame's spare capacity or the TX ring is
  // momentarily full (call reapCompletions() and retry).
  int send(const void *payload, std::size_t payload_len);

  // Drains pending TX completion events. Call periodically (e.g. once per
  // loop iteration in the sending thread); not required per-send.
  void reapCompletions();

private:
  static constexpr std::size_t kEthLen    = 14;
  static constexpr std::size_t kIp4Len    = 20;
  static constexpr std::size_t kUdpLen    = 8;
  static constexpr std::size_t kHeaderLen = kEthLen + kIp4Len + kUdpLen;
  static constexpr std::size_t kMinFrameLen = 60;
  static constexpr std::size_t kMaxPayload  = 512;
  static constexpr std::size_t kSlotLen     = kHeaderLen + kMaxPayload;
  static constexpr unsigned    kTxSlots     = 16;

  uint8_t *slot(unsigned i) const { return pool_ + static_cast<size_t>(i) * kSlotLen; }

  ef_driver_handle dh_ = -1;
  ef_pd            pd_{};
  ef_vi            vi_{};
  ef_memreg        mr_{};

  uint8_t *pool_ = nullptr;      // kTxSlots reusable frame buffers
  unsigned next_slot_ = 0;
  uint32_t next_dma_id_ = 0;
  uint16_t ip_id_ = 0;
};

}  // namespace waler
