// Two-thread rig: an "exchange" thread blasts simulated market data over
// UDP via kernel-bypass ef_vi TX, and a "strategy" thread picks packets off
// the NIC via ef_vi RX and measures receive timing. No parsing, no order
// book, no strategy logic yet -- this only exercises and times the
// send/receive path itself.
//
// Requires the two Solarflare X4 ports to be cabled together (loopback).

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <waler/EfviInterface.hpp>
#include <waler/EfviUdpSender.hpp>
#include <waler/PacketGenerator.hpp>
#include <waler/util.hpp>

#include "timing.h"
#include "stats.h"

namespace {

constexpr const char* kExchangeIfname = "enp1s0f0np0";
constexpr const char* kStrategyIfname = "enp1s0f1np1";
constexpr const char* kExchangeIp     = "10.10.10.1";
constexpr const char* kStrategyIp     = "10.10.10.2";
constexpr std::uint16_t kUdpPort      = 30303;
constexpr std::size_t   kDefaultCount = 200000;

std::atomic<bool> g_running{true};

void onSigint(int) {
  g_running.store(false, std::memory_order_relaxed);
}

void exchangeThreadMain(std::size_t count) {
  const auto dst_mac = waler::getInterfaceMac(kStrategyIfname);
  waler::EfviUdpSender sender(kExchangeIfname, kExchangeIp, kUdpPort,
                              kStrategyIp, kUdpPort, dst_mac);

  waler::PacketGenerator gen;
  std::byte buffer[1024];

  std::size_t sent = 0;
  while (g_running.load(std::memory_order_relaxed) && sent < count) {
    const ssize_t bytes = gen.nextPacket(reinterpret_cast<std::uint8_t*>(buffer), sizeof(buffer));
    if (bytes <= 0) {
      continue;
    }
    if (sender.send(buffer, static_cast<std::size_t>(bytes)) == 0) {
      ++sent;
    }
    sender.reapCompletions();
  }

  std::fprintf(stderr, "exchange: sent %zu packets\n", sent);
}

void strategyThreadMain(std::size_t count, stats_t* nic_gap, stats_t* tsc_gap, std::size_t* received) {
  waler::EfviInterface rx(kStrategyIfname, kStrategyIp, kUdpPort);

  bool have_prev = false;
  double prev_nic_ns = 0.0;
  std::uint64_t prev_tsc = 0;

  std::size_t n = 0;
  while (g_running.load(std::memory_order_relaxed) && n < count) {
    rx.poll([&](const waler::EfviFrame& frame) {
      if (n >= count) {
        return;
      }
      ++n;

      if (have_prev) {
        stats_record(tsc_gap, frame.tsc - prev_tsc);
      }
      prev_tsc = frame.tsc;

      if (frame.nic_ts_valid) {
        const double nic_ns = static_cast<double>(frame.nic_ts.tv_sec) * 1e9 +
                               static_cast<double>(frame.nic_ts.tv_nsec) +
                               static_cast<double>(frame.nic_ts.tv_nsec_frac) / 65536.0;
        if (have_prev) {
          // NIC hardware timestamps only, both from this same PHC: exact,
          // no TSC<->PHC correlation needed for a same-domain delta.
          stats_record(nic_gap, ns_to_tsc(nic_ns - prev_nic_ns));
        }
        prev_nic_ns = nic_ns;
      }

      have_prev = true;
    });
  }

  *received = n;
  std::fprintf(stderr, "strategy: received %zu packets (%zu discards)\n", n, rx.discard_count());
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t count = kDefaultCount;
  if (argc > 1) {
    count = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
  }

  std::signal(SIGINT, onSigint);

  tsc_calibrate();

  stats_t nic_gap;
  stats_t tsc_gap;
  stats_init(&nic_gap, count);
  stats_init(&tsc_gap, count);
  std::size_t received = 0;

  std::thread strategy(strategyThreadMain, count, &nic_gap, &tsc_gap, &received);
  // Give the RX filter a moment to be installed before the exchange thread
  // starts blasting.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::thread exchange(exchangeThreadMain, count);

  exchange.join();
  g_running.store(false, std::memory_order_relaxed);
  strategy.join();

  stats_result_t nic_result{};
  stats_result_t tsc_result{};
  stats_compute(&nic_gap, &nic_result);
  stats_compute(&tsc_gap, &tsc_result);

  std::fprintf(stdout, "\nreceived: %zu / %zu\n\n", received, count);
  stats_print(&nic_result, stdout, "inter-arrival gap, NIC hw RX timestamp domain (exact)");
  stats_print(&tsc_result, stdout, "inter-arrival gap, host TSC-after-poll domain (incl. scheduling)");

  stats_free(&nic_gap);
  stats_free(&tsc_gap);
  return 0;
}
