#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
  #include <etherfabric/memreg.h>
  #include <etherfabric/pd.h>
  #include <etherfabric/vi.h>
}

namespace waler {

// One received packet, valid only until the next poll() call (the buffer it
// points into gets reposted to the NIC afterwards).
//
// `data`/`len` cover the raw Ethernet frame as delivered by the NIC (past
// the ef_vi rx prefix) -- for a UDP payload, skip 14 (eth) + 20 (ipv4, no
// options) + 8 (udp) bytes to reach it.
struct EfviFrame {
  const uint8_t *data;          // payload, past the ef_vi rx prefix
  size_t         len;
  ef_precisetime nic_ts;        // NIC hardware RX timestamp (wire arrival)
  bool           nic_ts_valid;  // flag to ensure that nic_ts is valid
  uint64_t       tsc;           // rdtscp taken right after ef_eventq_poll
};

// Raw ef_vi RX path for one NIC port, filtered to a single UDP port.
// Bypasses the kernel network stack: packets are DMA'd by the NIC directly
// into buffers this class owns, and poll() is a non-blocking check of the
// VI's private event queue.
//
// `local_ip` is the destination IPv4 address (dotted-quad) the hardware
// filter matches on -- ef_vi filters can't wildcard the host, and since
// this bypasses the kernel there's no requirement that `local_ip` be an
// address actually configured on `ifname`.
class EfviInterface {
public:
  EfviInterface(const std::string &ifname, const std::string &local_ip,
               uint16_t udp_port, unsigned rxq_capacity = 512,
               unsigned max_frame_len = 2048);
  ~EfviInterface();

  EfviInterface(const EfviInterface&) = delete; // delete copy operations because we don't want to objects believing they own the same ef_vi and memory pool.
  EfviInterface &operator=(const EfviInterface&) = delete;

  // Non-blocking. Invokes fn(const EfviFrame&) once per packet received
  // during this call. Returns the number of packets delivered (0 if none).
  template <typename Fn>
  int poll(Fn &&fn) {
    ef_event evs[kMaxEvents];
    int n_evs = ef_eventq_poll(&vi_, evs, kMaxEvents);
    if (n_evs <= 0) return 0;

    uint64_t b1 = tsc_read();
    int delivered = 0;

    for (int i = 0; i < n_evs; ++i) {
      if (EF_EVENT_TYPE(evs[i]) != EF_EVENT_TYPE_RX) {
        if (EF_EVENT_TYPE(evs[i]) == EF_EVENT_TYPE_RX_DISCARD)
          ++n_discards_;
        continue;
      }

      int      dma_id = EF_EVENT_RX_RQ_ID(evs[i]);
      uint16_t bytes  = EF_EVENT_RX_BYTES(evs[i]);
      uint8_t *buf    = slot(dma_id);

      EfviFrame p{};
      p.data = buf + prefix_len_;
      p.len  = bytes > prefix_len_ ? bytes - prefix_len_ : 0;
      p.tsc  = b1;

      ef_precisetime ts{};
      p.nic_ts_valid = (ef_vi_receive_get_precise_timestamp(&vi_, buf, &ts) == 0);
      p.nic_ts       = ts;

      fn(p);
      ++delivered;

      repost(dma_id, buf);
    }
    return delivered;
  }

  size_t discard_count() const { return n_discards_; }

private:
  static constexpr int kMaxEvents = 32;

  uint8_t *slot(int dma_id) const {
    return pool_ + static_cast<size_t>(dma_id) * slot_len_;
  }

  void repost(int dma_id, uint8_t* buf);
  static uint64_t tsc_read();

  ef_driver_handle dh_ = -1;
  ef_pd            pd_{};
  ef_vi            vi_{};
  ef_memreg        mr_{};
  ef_filter_cookie filter_cookie_{};

  uint8_t *pool_       = nullptr;
  size_t   slot_len_   = 0;
  unsigned n_slots_    = 0;
  int      prefix_len_ = 0;
  size_t   n_discards_ = 0;
};

} // namespace waler
