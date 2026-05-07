#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <net/if.h>
#include <etherfabric/vi.h>
#include <etherfabric/pd.h>
#include <etherfabric/memreg.h>

#define PKT_BUF_SIZE    2048
#define N_BUFS          8

/* Macro to check ef_vi return codes */
#define TRY(x) do {                                             \
    int __rc = (x);                                             \
    if (__rc < 0) {                                             \
        fprintf(stderr, "ERROR: %s failed rc=%d errno=%d\n",   \
                #x, __rc, errno);                               \
        exit(1);                                                \
    }                                                           \
} while (0)

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <interface>\n", argv[0]);
        return 1;
    }
    const char *ifname = argv[1];

    ef_driver_handle  dh;
    ef_pd             pd;
    ef_vi             vi;
    ef_memreg         mr;

    /* 1. Open a handle to the ef_vi driver */
    TRY(ef_driver_open(&dh));

    /* 2. Allocate a Protection Domain on the given interface */
    int ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        fprintf(stderr, "Unknown interface: %s\n", ifname);
        return 1;
    }
    TRY(ef_pd_alloc(&pd, dh, ifindex, EF_PD_DEFAULT));

    /* 3. Allocate a Virtual Interface (VI) */
    TRY(ef_vi_alloc_from_pd(&vi, dh, &pd, dh,
                             -1,   /* evq_capacity: default */
                             -1,   /* rxq_capacity: default */
                             -1,   /* txq_capacity: default */
                             NULL, /* no event queue sharing */
                             -1,
                             0));

    /* 4. Allocate and pin DMA-capable packet buffers */
    void *bufs = NULL;
    size_t alloc_size = N_BUFS * PKT_BUF_SIZE;
    if (posix_memalign(&bufs, 4096, alloc_size) != 0) {
        perror("posix_memalign");
        return 1;
    }
    TRY(ef_memreg_alloc(&mr, dh, &pd, dh, bufs, alloc_size));

    /* 5. Build a raw Ethernet frame in buffer 0
     *    Layout: [dst MAC 6B][src MAC 6B][ethertype 2B][payload...]
     *    Here we send an oversized but valid frame with a dummy payload.
     */
    uint8_t *pkt = (uint8_t *)bufs;  /* use first buffer for TX */

    /* Destination MAC: broadcast */
    memset(pkt, 0xff, 6);
    /* Source MAC: the card's own MAC (hardcoded here; replace as needed) */
    pkt[6]  = 0xf4; pkt[7]  = 0xee; pkt[8]  = 0x08;
    pkt[9]  = 0x46; pkt[10] = 0x71; pkt[11] = 0x34;
    /* EtherType: 0x0800 = IPv4 (or use any custom ethertype) */
    pkt[12] = 0x08; pkt[13] = 0x00;
    /* Payload: 46 bytes of zeroes (minimum Ethernet payload) */
    memset(pkt + 14, 0, 46);

    int frame_len = 14 + 46;  /* 60 bytes total */

    /* 6. Get the DMA address of buffer 0 and transmit */
    ef_addr tx_dma = ef_memreg_dma_addr(&mr, 0);
    TRY(ef_vi_transmit(&vi, tx_dma, frame_len, 0 /* request id */));

    /* 7. Poll the event queue until we see the TX completion */
    ef_event evs[8];
    int done = 0;
    while (!done) {
        int n = ef_eventq_poll(&vi, evs, 8);
        for (int i = 0; i < n; i++) {
            switch (EF_EVENT_TYPE(evs[i])) {
            case EF_EVENT_TYPE_TX:
                printf("TX complete! rq_id=%d\n");
                done = 1;
                break;
            case EF_EVENT_TYPE_TX_ERROR:
                fprintf(stderr, "TX error!\n");
                done = 1;
                break;
            default:
                break;
            }
        }
    }

    /* 8. Cleanup */
    ef_memreg_free(&mr, dh);
    ef_vi_free(&vi, dh);
    ef_pd_free(&pd, dh);
    ef_driver_close(dh);

    return 0;
}
