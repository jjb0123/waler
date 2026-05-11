#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "../../common/timing.h"
#include "../../common/stats.h"
#include "../../common/plot.h"

static const int    N_WARMUP  = 1000;
static const int    N_SAMPLES = 100000;
static const char  *PAYLOAD   = "ping";

int main(int argc, char *argv[]) {
    const char *server_ip = (argc > 1) ? argv[1] : "127.0.0.1";

    tsc_calibrate();

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(9000);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    /* connect() so we can use send/recv and the kernel filters replies
     * from other senders */
    if (connect(sockfd, reinterpret_cast<sockaddr*>(&server_addr),
                sizeof(server_addr)) < 0) {
        std::cerr << "Failed to connect\n";
        close(sockfd);
        return 1;
    }

    char buf[64];

    /* Warmup: let routes, ARP, and caches settle — don't record these */
    for (int i = 0; i < N_WARMUP; i++) {
        send(sockfd, PAYLOAD, strlen(PAYLOAD), 0);
        recv(sockfd, buf, sizeof(buf), 0);
    }

    /* Measurement loop */
    stats_t rtt;
    stats_init(&rtt, N_SAMPLES);

    for (int i = 0; i < N_SAMPLES; i++) {
        uint64_t t0 = tsc_read();
        send(sockfd, PAYLOAD, strlen(PAYLOAD), 0);
        recv(sockfd, buf, sizeof(buf), 0);
        uint64_t t1 = tsc_read();

        stats_record(&rtt, t1 - t0);
    }

    plot_generate(&rtt, "kernel UDP RTT", ".");

    stats_result_t result;
    stats_compute(&rtt, &result);
    stats_print(&result, stdout, "kernel UDP RTT");

    stats_free(&rtt);
    close(sockfd);
    return 0;
}
