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
#include <sstream>
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

bool get_if_mac(int sockfd, const std::string& ifname, uint8_t mac[6]) {
    struct ifreq ifr {};
    std::snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname.c_str());
    if (ioctl(sockfd, SIOCGIFHWADDR, &ifr) < 0) {
        return false;
    }
    std::memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return true;
}

bool parse_mac(const std::string& s, uint8_t mac[6]) {
    unsigned int values[6];
    if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x",
                    &values[0], &values[1], &values[2],
                    &values[3], &values[4], &values[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        mac[i] = static_cast<uint8_t>(values[i]);
    }
    return true;
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
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <send_interface> <dest_mac> <seq>\n";
        return 1;
    }

    std::string ifname = argv[1];
    std::string dest_mac_str = argv[2];
    uint64_t seq = std::stoull(argv[3]);

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

    uint8_t src_mac[6];
    if (!get_if_mac(sockfd, ifname, src_mac)) {
        std::perror("SIOCGIFHWADDR");
        close(sockfd);
        return 1;
    }

    uint8_t dst_mac[6];
    if (!parse_mac(dest_mac_str, dst_mac)) {
        std::cerr << "Failed to parse destination MAC: " << dest_mac_str << "\n";
        close(sockfd);
        return 1;
    }

    std::vector<uint8_t> frame(sizeof(ether_header) + sizeof(TestPayload), 0);

    auto* eth = reinterpret_cast<ether_header*>(frame.data());
    std::memcpy(eth->ether_dhost, dst_mac, 6);
    std::memcpy(eth->ether_shost, src_mac, 6);
    eth->ether_type = htons(kEthertype);

    auto* payload = reinterpret_cast<TestPayload*>(frame.data() + sizeof(ether_header));
    payload->magic = htonl(kMagic);
    payload->seq = htobe64(seq);
    std::memset(payload->padding, 0xAB, sizeof(payload->padding));

    sockaddr_ll addr {};
    addr.sll_family = AF_PACKET;
    addr.sll_ifindex = ifindex;
    addr.sll_halen = ETH_ALEN;
    std::memcpy(addr.sll_addr, dst_mac, 6);

    ssize_t sent = sendto(sockfd,
                          frame.data(),
                          frame.size(),
                          0,
                          reinterpret_cast<sockaddr*>(&addr),
                          sizeof(addr));
    if (sent < 0) {
        std::perror("sendto");
        close(sockfd);
        return 1;
    }

    std::cout << "Sent frame on " << ifname << "\n";
    std::cout << "  src MAC: " << mac_to_string(src_mac) << "\n";
    std::cout << "  dst MAC: " << mac_to_string(dst_mac) << "\n";
    std::cout << "  seq    : " << seq << "\n";
    std::cout << "  bytes  : " << sent << "\n";

    close(sockfd);
    return 0;
}