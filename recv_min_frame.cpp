#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr uint16_t kEthertype = 0x88B5;
constexpr uint32_t kMagic = 0x4A425243; // "JBRC"

struct TestPayload {
    uint32_t magic;
    uint64_t seq;
    uint8_t padding[46 - sizeof(uint32_t) - sizeof(uint64_t)];
} __attribute__((packed));

int get_ifindex(int sockfd, const std::string& ifname) {
    struct ifreq ifr {};
    std::snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname.c_str());
    if (ioctl(sockfd, SIOCGIFINDEX, &ifr) < 0) {
        return -1;
    }
    return ifr.ifr_ifindex;
}

std::string mac_to_string(const uint8_t* mac) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(2) << static_cast<int>(mac[0]) << ":"
        << std::setw(2) << static_cast<int>(mac[1]) << ":"
        << std::setw(2) << static_cast<int>(mac[2]) << ":"
        << std::setw(2) << static_cast<int>(mac[3]) << ":"
        << std::setw(2) << static_cast<int>(mac[4]) << ":"
        << std::setw(2) << static_cast<int>(mac[5]);
    return oss.str();
}
}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <recv_interface>\n";
        return 1;
    }

    std::string ifname = argv[1];

    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(kEthertype));
    if (sockfd < 0) {
        std::perror("socket");
        return 1;
    }

    int ifindex = get_ifindex(sockfd, ifname);
    if (ifindex < 0) {
        std::perror("SIOCGIFINDEX");
        close(sockfd);
        return 1;
    }

    sockaddr_ll bind_addr {};
    bind_addr.sll_family = AF_PACKET;
    bind_addr.sll_protocol = htons(kEthertype);
    bind_addr.sll_ifindex = ifindex;

    if (bind(sockfd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
        std::perror("bind");
        close(sockfd);
        return 1;
    }

    std::cout << "Listening on interface " << ifname
              << " for EtherType 0x" << std::hex << kEthertype << std::dec << "...\n";

    std::vector<uint8_t> buf(2048);

    while (true) {
        ssize_t n = recvfrom(sockfd, buf.data(), buf.size(), 0, nullptr, nullptr);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("recvfrom");
            close(sockfd);
            return 1;
        }

        if (n < static_cast<ssize_t>(sizeof(ether_header) + sizeof(TestPayload))) {
            continue;
        }

        auto* eth = reinterpret_cast<ether_header*>(buf.data());
        if (ntohs(eth->ether_type) != kEthertype) {
            continue;
        }

        auto* payload = reinterpret_cast<TestPayload*>(buf.data() + sizeof(ether_header));
        if (ntohl(payload->magic) != kMagic) {
            std::cout << "Got matching EtherType but wrong magic; ignoring.\n";
            continue;
        }

        uint64_t seq = be64toh(payload->seq);

        std::cout << "Received test frame:\n";
        std::cout << "  dst MAC: " << mac_to_string(eth->ether_dhost) << "\n";
        std::cout << "  src MAC: " << mac_to_string(eth->ether_shost) << "\n";
        std::cout << "  seq    : " << seq << "\n";
        std::cout << "  bytes  : " << n << "\n";
        break;
    }

    close(sockfd);
    return 0;
}