#include <waler/EfviInterface.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <net/if.h>

extern "C" {
  #include <etherfabric/base.h>
}

namespace waler {

namespace {

void checkOk(int rc, const char* what) {
  if (rc < 0) {
    std::fprintf(stderr, "EfviInterface: %s failed: rc=%d errno=%d (%s)\n",
                 what, rc, errno, std::strerror(errno));
    std::abort();
  }
}

}  // namespace

EfviInterface::EfviInterface(const std::string& ifname, const std::string& local_ip,
                             uint16_t udp_port, unsigned rxq_capacity,
                             unsigned max_frame_len)
    : n_slots_(rxq_capacity) {
  const unsigned ifindex = if_nametoindex(ifname.c_str());
  if (ifindex == 0) {
    std::fprintf(stderr, "EfviInterface: no such interface: %s\n", ifname.c_str());
    std::abort();
  }

  checkOk(ef_driver_open(&dh_), "ef_driver_open");
  checkOk(ef_pd_alloc(&pd_, dh_, static_cast<int>(ifindex), EF_PD_DEFAULT), "ef_pd_alloc");
  checkOk(ef_vi_alloc_from_pd(&vi_, dh_, &pd_, dh_,
                              -1 /* evq */, -1 /* rxq */, -1 /* txq */,
                              nullptr, -1, EF_VI_RX_TIMESTAMPS),
          "ef_vi_alloc_from_pd");

  prefix_len_ = ef_vi_receive_prefix_len(&vi_);
  slot_len_   = max_frame_len;

  const size_t pool_bytes = slot_len_ * static_cast<size_t>(n_slots_);
  void* pool_mem = nullptr;
  if (posix_memalign(&pool_mem, 4096, pool_bytes) != 0) {
    std::fprintf(stderr, "EfviInterface: posix_memalign(%zu) failed\n", pool_bytes);
    std::abort();
  }
  pool_ = static_cast<uint8_t*>(pool_mem);
  std::memset(pool_, 0, pool_bytes);

  checkOk(ef_memreg_alloc(&mr_, dh_, &pd_, dh_, pool_, pool_bytes), "ef_memreg_alloc");

  in_addr local_addr{};
  if (inet_pton(AF_INET, local_ip.c_str(), &local_addr) != 1) {
    std::fprintf(stderr, "EfviInterface: bad local_ip: %s\n", local_ip.c_str());
    std::abort();
  }

  ef_filter_spec fs;
  ef_filter_spec_init(&fs, EF_FILTER_FLAG_NONE);
  checkOk(ef_filter_spec_set_ip4_local(&fs, IPPROTO_UDP, local_addr.s_addr, htons(udp_port)),
          "ef_filter_spec_set_ip4_local");
  checkOk(ef_vi_filter_add(&vi_, dh_, &fs, &filter_cookie_), "ef_vi_filter_add");

  for (unsigned i = 0; i < n_slots_; ++i) {
    const ef_addr dma = ef_memreg_dma_addr(&mr_, static_cast<size_t>(i) * slot_len_);
    checkOk(ef_vi_receive_post(&vi_, dma, static_cast<int>(i)), "ef_vi_receive_post");
  }
}

EfviInterface::~EfviInterface() {
  ef_vi_filter_del(&vi_, dh_, &filter_cookie_);
  ef_memreg_free(&mr_, dh_);
  ef_vi_free(&vi_, dh_);
  ef_pd_free(&pd_, dh_);
  ef_driver_close(dh_);
  std::free(pool_);
}

void EfviInterface::repost(int dma_id, uint8_t* /*buf*/) {
  const ef_addr dma = ef_memreg_dma_addr(&mr_, static_cast<size_t>(dma_id) * slot_len_);
  ef_vi_receive_post(&vi_, dma, dma_id);
}

uint64_t EfviInterface::tsc_read() {
  uint32_t lo = 0;
  uint32_t hi = 0;
  __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "rcx", "memory");
  return (static_cast<uint64_t>(hi) << 32) | lo;
}

}  // namespace waler
