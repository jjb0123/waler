#include <iostream>
#include <cstring> 
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

int main () {
    int sock = socket(AF_INET, SOCK_DGRAM, 0); // IPv4, datagram(udp), default protocol. though 0 could create some overehad
    if (sock < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }

    std::cout << "Socket created successfully" << std::endl;
    close(sockfd);
    return 0;
}