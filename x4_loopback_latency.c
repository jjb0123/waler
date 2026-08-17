/*
 * x4_loopback_latency.c
 *
 * Decompose NIC latency on a Solarflare X4 with a loopback cable between
 * its two ports, using ef_vi + hardware timestamps + the x86 TSC.
 *
 * Measurement points per packet:
 *   T0     (TSC)  : immediately before the TX doorbell
 *   T1     (NIC)  : TX hardware timestamp (wire departure)
 *   T2     (NIC)  : RX hardware timestamp (wire arrival)
 *   T_mem  (TSC)  : last byte of payload observed in host memory (DMA landed)
 *   T3     (TSC)  : ef_eventq_poll() returns the RX event
 *
 * Derived quantities:
 *   wire            = T2 - T1                       (NIC clock only, exact)
 *   rtt_host        = T_mem - T0                    (TSC only, exact)
 *   egress+ingress  = rtt_host - wire               (no cross-domain sync)
 *   poll_delay      = T3 - T_mem                    (TSC only, exact)
 *   egress          = nic2tsc(T1) - T0              (needs clock correlation)
 *   ingress         = T_mem - nic2tsc(T2)           (needs clock correlation)
 *
 * Clock correlation: TSC <-> CLOCK_REALTIME (bracketed rdtsc pairs)
 * composed with CLOCK_REALTIME <-> NIC PHC (/dev/ptpN, PTP_SYS_OFFSET_PRECISE
 * with PTP_SYS_OFFSET fallback). Both fits are refreshed periodically.
 *
 * Build:
 *   gcc -O2 -Wall -o x4_loopback_latency x4_loopback_latency.c -lciul1 -lm
 *
 * Run (example, TX on enp1s0f0, RX on enp1s0f1, PHC of the NIC at ptp0):
 *   sudo taskset -c 3 ./x4_loopback_latency enp1s0f0 enp1s0f1 /dev/ptp0 100000
 *
 * Notes / things to verify on your firmware+ciul version:
 *   - TX VI needs EF_VI_TX_TIMESTAMPS, RX VI needs EF_VI_RX_TIMESTAMPS.
 *     If TX completions come back as plain EF_EVENT_TYPE_TX instead of
 *     EF_EVENT_TYPE_TX_WITH_TIMESTAMP, your firmware variant / license
 *     doesn't have TX timestamping enabled.
 *   - The exact accessor macros for TX timestamp events have shifted between
 *     ciul versions (EF_EVENT_TX_WITH_TIMESTAMP_SEC/_NSEC and a 16-bit
 *     fractional field _NSEC_FRAC16 on newer trees). Adjust if your headers
 *     differ; grep etherfabric/ef_vi.h.
 *   - Run on an isolated core, C-states off, invariant TSC assumed.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <net/if.h>
#include <linux/ptp_clock.h>
#include <x86intrin.h>

#include <etherfabric/vi.h>
#include <etherfabric/pd.h>
#include <etherfabric/pio.h>
#include <etherfabric/memreg.h>
#include <etherfabric/ef_vi.h>
#include <etherfabric/capabilities.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define PKT_LEN        128          /* frame length on the wire (>= 60) */
#define BUF_SIZE       2048
#define N_RX_BUFS      64
#define WARMUP_ITERS   2000
#define CAL_SAMPLES    64           /* samples per calibration pass */
#define CAL_PERIOD     4096         /* re-calibrate every N iterations */

#define TRY(x)                                                          \
  do {                                                                  \
    int __rc = (x);                                                     \
    if (__rc < 0) {                                                     \
      fprintf(stderr, "FAIL %s:%d %s rc=%d errno=%d (%s)\n",            \
              __FILE__, __LINE__, #x, __rc, errno, strerror(errno));    \
      exit(1);                                                          \
    }                                                                   \
  } while (0)

/* ------------------------------------------------------------------ */
/* TSC helpers                                                         */
/* ------------------------------------------------------------------ */

static inline uint64_t tsc_now(void) {
  /* lfence keeps earlier loads/stores from drifting past the read and
   * keeps rdtsc from executing early. Cheaper than cpuid serialization
   * and sufficient for this use. */
  _mm_lfence();
  uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
}

/* Linear map: y = a*x + b, fit by least squares over recent samples. */
typedef struct { double a, b; } linfit_t;

static linfit_t fit(const double* x, const double* y, int n) {
  /* Center on the first sample: raw values are epoch-scale (1e12 tsc
   * ticks, 1e18 phc ns) and squaring them destroys all precision in
   * doubles. Fit in the centered frame, translate the intercept back. */
  double x0 = x[0], y0 = y[0];
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (int i = 0; i < n; i++) {
    double dx = x[i] - x0, dy = y[i] - y0;
    sx += dx; sy += dy; sxx += dx*dx; sxy += dx*dy;
  }
  double d = n * sxx - sx * sx;
  linfit_t f;
  f.a = (n * sxy - sx * sy) / d;
  f.b = (sy - f.a * sx) / n + y0 - f.a * x0;
  return f;
}

static inline double lin_apply(linfit_t f, double x) { return f.a * x + f.b; }

static inline double ts_to_ns(struct timespec ts) {
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* TSC -> CLOCK_REALTIME (ns). Bracket clock_gettime with rdtsc, keep the
 * tightest brackets, fit tsc -> realtime_ns. Also yields tsc frequency. */
static linfit_t calibrate_tsc_realtime(void) {
  double xs[CAL_SAMPLES], ys[CAL_SAMPLES];
  int n = 0;
  for (int i = 0; i < CAL_SAMPLES * 4 && n < CAL_SAMPLES; i++) {
    uint64_t t0 = tsc_now();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t1 = tsc_now();
    if (t1 - t0 > 400)            /* discard wide brackets (preemption etc) */
      continue;
    xs[n] = (double)((t0 + t1) / 2);
    ys[n] = ts_to_ns(ts);
    n++;
    usleep(300);   /* spread samples: ~20 ms total baseline, so a ~50 ns
                    * bracket error resolves the slope to a few ppm */
  }
  if (n < 8) { fprintf(stderr, "tsc calibration failed\n"); exit(1); }
  return fit(xs, ys, n);
}

/* CLOCK_REALTIME (ns) -> PHC (ns). Prefers PTP_SYS_OFFSET_PRECISE
 * (cross-timestamped in hardware/driver), falls back to PTP_SYS_OFFSET
 * with min-bracket filtering. */
static linfit_t calibrate_realtime_phc(int phc_fd) {
  double xs[CAL_SAMPLES], ys[CAL_SAMPLES];
  int n = 0;

#ifdef PTP_SYS_OFFSET_PRECISE
  for (int i = 0; i < CAL_SAMPLES; i++) {
    struct ptp_sys_offset_precise p;
    memset(&p, 0, sizeof(p));
    if (ioctl(phc_fd, PTP_SYS_OFFSET_PRECISE, &p) == 0) {
      xs[n] = (double)p.sys_realtime.sec * 1e9 + p.sys_realtime.nsec;
      ys[n] = (double)p.device.sec * 1e9 + p.device.nsec;
      n++;
      usleep(300);   /* spread the baseline for slope resolution */
    } else {
      break;  /* not supported by this driver; fall back below */
    }
  }
#endif

  if (n < 8) {
    n = 0;
    for (int i = 0; i < CAL_SAMPLES && n < CAL_SAMPLES; i++) {
      struct ptp_sys_offset p;
      memset(&p, 0, sizeof(p));
      p.n_samples = PTP_MAX_SAMPLES;
      if (ioctl(phc_fd, PTP_SYS_OFFSET, &p) < 0) {
        perror("PTP_SYS_OFFSET");
        exit(1);
      }
      /* samples: sys[0], phc[0], sys[1], phc[1], ... sys[n]; pick the
       * tightest sys bracket around each phc reading */
      double best_gap = 1e18; int best = -1;
      for (unsigned j = 0; j < p.n_samples; j++) {
        double s0 = (double)p.ts[2*j].sec * 1e9 + p.ts[2*j].nsec;
        double s1 = (double)p.ts[2*j+2].sec * 1e9 + p.ts[2*j+2].nsec;
        if (s1 - s0 < best_gap) { best_gap = s1 - s0; best = (int)j; }
      }
      if (best >= 0) {
        double s0 = (double)p.ts[2*best].sec * 1e9 + p.ts[2*best].nsec;
        double s1 = (double)p.ts[2*best+2].sec * 1e9 + p.ts[2*best+2].nsec;
        double d  = (double)p.ts[2*best+1].sec * 1e9 + p.ts[2*best+1].nsec;
        xs[n] = (s0 + s1) / 2;
        ys[n] = d;
        n++;
        usleep(300);
      }
    }
  }

  if (n < 8) { fprintf(stderr, "phc calibration failed\n"); exit(1); }
  return fit(xs, ys, n);
}

/* Composite: NIC-clock ns -> TSC-domain ns.
 * realtime = a1*tsc_ns + b1  (approximately; we fit tsc ticks directly)
 * phc      = a2*realtime + b2
 * so tsc_ns(phc) inverts both. We keep the two fits and invert on demand. */
typedef struct {
  linfit_t tsc2rt;   /* tsc ticks   -> realtime ns */
  linfit_t rt2phc;   /* realtime ns -> phc ns      */
} clockmap_t;

static double phc_to_tscns(const clockmap_t* m, double phc_ns,
                           double tsc_hz) {
  double rt  = (phc_ns - m->rt2phc.b) / m->rt2phc.a;
  double tsc = (rt - m->tsc2rt.b) / m->tsc2rt.a;    /* tsc ticks */
  return tsc * 1e9 / tsc_hz;                        /* -> ns */
}

static inline double tsc_ticks_to_ns(uint64_t ticks, double tsc_hz) {
  return (double)ticks * 1e9 / tsc_hz;
}

/* ------------------------------------------------------------------ */
/* Interface MAC lookup                                                */
/* ------------------------------------------------------------------ */

#include <sys/socket.h>

static void get_mac(const char* intf, uint8_t mac[6]) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  snprintf(ifr.ifr_name, IFNAMSIZ, "%s", intf);
  TRY(ioctl(s, SIOCGIFHWADDR, &ifr));
  memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
  close(s);
}

/* ------------------------------------------------------------------ */
/* ef_vi state (one side = one port)                                   */
/* ------------------------------------------------------------------ */

typedef struct {
  ef_driver_handle dh;
  ef_pd            pd;
  ef_vi            vi;
  ef_memreg        mr;
  void*            buf_base;
  size_t           buf_bytes;
  ef_addr          dma_base;
} port_t;

static void port_open(port_t* p, const char* intf, unsigned vi_flags) {
  int ifindex = if_nametoindex(intf);
  if (ifindex == 0) {
    fprintf(stderr, "no such interface: %s\n", intf);
    exit(1);
  }
  TRY(ef_driver_open(&p->dh));
  TRY(ef_pd_alloc(&p->pd, p->dh, ifindex, EF_PD_DEFAULT));
  TRY(ef_vi_alloc_from_pd(&p->vi, p->dh, &p->pd, p->dh,
                          -1 /* evq */, -1 /* rxq */, -1 /* txq */,
                          NULL, -1, vi_flags));

  p->buf_bytes = BUF_SIZE * (N_RX_BUFS + 1);
  TRY(posix_memalign(&p->buf_base, 4096, p->buf_bytes) == 0 ? 0 : -1);
  memset(p->buf_base, 0, p->buf_bytes);
  TRY(ef_memreg_alloc(&p->mr, p->dh, &p->pd, p->dh,
                      p->buf_base, p->buf_bytes));
  p->dma_base = ef_memreg_dma_addr(&p->mr, 0);
}

/* ------------------------------------------------------------------ */
/* Stats                                                               */
/* ------------------------------------------------------------------ */

static int cmp_dbl(const void* a, const void* b) {
  double d = *(const double*)a - *(const double*)b;
  return d < 0 ? -1 : d > 0 ? 1 : 0;
}

static void report(const char* name, double* v, int n) {
  qsort(v, n, sizeof(double), cmp_dbl);
  double sum = 0;
  for (int i = 0; i < n; i++) sum += v[i];
  printf("%-18s n=%-7d min=%8.1f p50=%8.1f p90=%8.1f p99=%8.1f "
         "p99.9=%8.1f max=%9.1f mean=%8.1f  (ns)\n",
         name, n,
         v[0], v[n/2], v[(int)(n*0.90)], v[(int)(n*0.99)],
         v[(int)(n*0.999)], v[n-1], sum / n);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char** argv) {
  if (argc < 5) {
    fprintf(stderr,
      "usage: %s <tx-intf> <rx-intf> </dev/ptpN> <iters>\n", argv[0]);
    return 1;
  }
  const char* tx_intf = argv[1];
  const char* rx_intf = argv[2];
  const char* phc_dev = argv[3];
  int iters = atoi(argv[4]);

  int phc_fd = open(phc_dev, O_RDONLY);
  if (phc_fd < 0) { perror(phc_dev); return 1; }

  /* --- clock calibration ------------------------------------------ */
  clockmap_t cm;
  cm.tsc2rt = calibrate_tsc_realtime();
  cm.rt2phc = calibrate_realtime_phc(phc_fd);
  /* slope a is realtime-ns per tsc tick, so frequency = 1e9 / a */
  double tsc_hz = 1e9 / cm.tsc2rt.a;
  printf("# tsc ~ %.6f GHz, phc skew vs realtime: %+0.3f ppm\n",
         tsc_hz / 1e9, (cm.rt2phc.a - 1.0) * 1e6);

  /* --- NIC setup --------------------------------------------------- */
  port_t tx, rx;
  port_open(&tx, tx_intf, EF_VI_TX_TIMESTAMPS);
  port_open(&rx, rx_intf, EF_VI_RX_TIMESTAMPS);

  /* steer frames addressed to the RX port's MAC to our VI */
  uint8_t rx_mac[6];
  get_mac(rx_intf, rx_mac);
  printf("# rx mac = %02x:%02x:%02x:%02x:%02x:%02x\n",
         rx_mac[0], rx_mac[1], rx_mac[2], rx_mac[3], rx_mac[4], rx_mac[5]);
  ef_filter_spec fs;
  ef_filter_spec_init(&fs, EF_FILTER_FLAG_NONE);
  TRY(ef_filter_spec_set_eth_local(&fs, EF_FILTER_VLAN_ID_ANY, rx_mac));
  TRY(ef_vi_filter_add(&rx.vi, rx.dh, &fs, NULL));

  unsigned rx_prefix = ef_vi_receive_prefix_len(&rx.vi);
  printf("# rx prefix = %u bytes\n", rx_prefix);

  /* post RX buffers */
  for (int i = 0; i < N_RX_BUFS; i++)
    TRY(ef_vi_receive_post(&rx.vi, rx.dma_base + (ef_addr)i * BUF_SIZE, i));

  /* build the TX frame in the last buffer slot of the TX registration */
  uint8_t* txf = (uint8_t*)tx.buf_base + (size_t)N_RX_BUFS * BUF_SIZE;
  ef_addr  txf_dma = tx.dma_base + (ef_addr)N_RX_BUFS * BUF_SIZE;
  memset(txf, 0, PKT_LEN);
  memcpy(txf + 0, rx_mac, 6);               /* dst: the RX port's MAC */
  memset(txf + 6, 0x02, 6);                 /* src: locally administered */
  txf[12] = 0x88; txf[13] = 0xb5;           /* experimental ethertype */
  for (int i = 14; i < PKT_LEN; i++) txf[i] = (uint8_t)i;
  txf[PKT_LEN - 1] = 0xa5;                  /* nonzero sentinel value */

  /* allocate result arrays */
  double* wire    = malloc(iters * sizeof(double));
  double* rtt     = malloc(iters * sizeof(double));
  double* eg_in   = malloc(iters * sizeof(double));
  double* egress  = malloc(iters * sizeof(double));
  double* ingress = malloc(iters * sizeof(double));
  double* polld   = malloc(iters * sizeof(double));
  int n_ok = 0;

  ef_event evs[16];
  ef_request_id ids[EF_VI_TRANSMIT_BATCH];
  int rx_next = 0;   /* which RX buffer id we expect next (FIFO order) */

  for (int it = 0; it < iters + WARMUP_ITERS; it++) {
    if ((it % CAL_PERIOD) == 0 && it > 0) {
      cm.tsc2rt = calibrate_tsc_realtime();
      cm.rt2phc = calibrate_realtime_phc(phc_fd);
      tsc_hz = 1e9 / cm.tsc2rt.a;
    }

    int slot = rx_next % N_RX_BUFS;
    uint8_t* rxf = (uint8_t*)rx.buf_base + (size_t)slot * BUF_SIZE;
    volatile uint8_t* sentinel = rxf + rx_prefix + PKT_LEN - 1;
    *sentinel = 0;                           /* arm the sentinel */
    _mm_mfence();

    /* ---- T0 + transmit ---- */
    uint64_t t0 = tsc_now();
    TRY(ef_vi_transmit(&tx.vi, txf_dma, PKT_LEN, it));

    /* ---- spin on payload byte for T_mem ---- */
    uint64_t t_mem;
    for (;;) {
      if (*sentinel == 0xa5) { t_mem = tsc_now(); break; }
    }

    /* ---- poll RX event queue for T3 + T2 ---- */
    double t2_phc_ns = -1;
    uint64_t t3 = 0;
    int got_rx = 0;
    while (!got_rx) {
      int n = ef_eventq_poll(&rx.vi, evs, 16);
      if (n == 0) continue;
      uint64_t t3_candidate = tsc_now();
      for (int i = 0; i < n; i++) {
        switch (EF_EVENT_TYPE(evs[i])) {
        case EF_EVENT_TYPE_RX: {
          t3 = t3_candidate;
          ef_precisetime pt;
          TRY(ef_vi_receive_get_precise_timestamp(&rx.vi, rxf, &pt));
          t2_phc_ns = (double)pt.tv_sec * 1e9 + (double)pt.tv_nsec
                      + (double)pt.tv_nsec_frac / 65536.0;
          /* repost the buffer */
          TRY(ef_vi_receive_post(&rx.vi,
                rx.dma_base + (ef_addr)slot * BUF_SIZE, slot));
          rx_next++;
          got_rx = 1;
          break;
        }
        case EF_EVENT_TYPE_RX_DISCARD:
          fprintf(stderr, "rx discard type=%d\n",
                  (int)EF_EVENT_RX_DISCARD_TYPE(evs[i]));
          exit(1);
        default:
          break;
        }
      }
    }

    /* ---- reap TX completion for T1 ---- */
    double t1_phc_ns = -1;
    int got_tx = 0;
    while (!got_tx) {
      int n = ef_eventq_poll(&tx.vi, evs, 16);
      for (int i = 0; i < n; i++) {
        switch (EF_EVENT_TYPE(evs[i])) {
        case EF_EVENT_TYPE_TX_WITH_TIMESTAMP:
          /* One completion per send in this benchmark. Newer ciul also
           * exposes a 16-bit fractional ns field; add it if present:
           *   + EF_EVENT_TX_WITH_TIMESTAMP_NSEC_FRAC16(evs[i]) / 65536.0 */
          t1_phc_ns =
            (double)EF_EVENT_TX_WITH_TIMESTAMP_SEC(evs[i]) * 1e9 +
            (double)EF_EVENT_TX_WITH_TIMESTAMP_NSEC(evs[i]);
          got_tx = 1;
          break;
        case EF_EVENT_TYPE_TX:
          fprintf(stderr,
            "got plain TX completion: TX timestamping not active "
            "(firmware variant/license?)\n");
          exit(1);
        default:
          break;
        }
      }
    }

    if (it < WARMUP_ITERS)
      continue;

    /* ---- derive ---- */
    double t0_ns    = tsc_ticks_to_ns(t0, tsc_hz);
    double tmem_ns  = tsc_ticks_to_ns(t_mem, tsc_hz);
    double t3_ns    = tsc_ticks_to_ns(t3, tsc_hz);
    double t1_ns    = phc_to_tscns(&cm, t1_phc_ns, tsc_hz);
    double t2_ns    = phc_to_tscns(&cm, t2_phc_ns, tsc_hz);

    wire[n_ok]    = t2_phc_ns - t1_phc_ns;      /* same clock: exact   */
    rtt[n_ok]     = tmem_ns - t0_ns;            /* same clock: exact   */
    eg_in[n_ok]   = rtt[n_ok] - wire[n_ok];     /* still sync-free     */
    polld[n_ok]   = t3_ns - tmem_ns;            /* same clock: exact   */
    egress[n_ok]  = t1_ns - t0_ns;              /* cross-domain        */
    ingress[n_ok] = tmem_ns - t2_ns;            /* cross-domain        */
    n_ok++;
  }

  printf("\n# exact (single clock domain):\n");
  report("wire (T2-T1)", wire, n_ok);
  report("rtt_host", rtt, n_ok);
  report("egress+ingress", eg_in, n_ok);
  report("poll_delay", polld, n_ok);
  printf("\n# cross-domain (subject to clock-fit error, ~10s of ns):\n");
  report("egress", egress, n_ok);
  report("ingress", ingress, n_ok);

  return 0;
}
