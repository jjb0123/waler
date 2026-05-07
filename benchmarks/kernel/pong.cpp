#include <iostream>
#include <cstring> 
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

int main () {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0); // IPv4, datagram(udp), default protocol. though 0 could create some overehad
    if (sockfd < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET; // IPv4
    server_addr.sin_port = htons(9000);
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all interfaces 

    if (bind(sockfd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::cerr << "Failed to bind socket" << std::endl;
        close(sockfd);
        return 1;
    }
    
    std::cout << "UDP pong server listening on port 9000\n";

    char buffer[1024];

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_addr_len = sizeof(client_addr);

        ssize_t n = recvfrom(
            sockfd,
            buffer, 
            sizeof(buffer), 
            0, // no flags
            reinterpret_cast<sockaddr*>(&client_addr),
            &client_addr_len
        );

        if (n < 0) {
            std:cerr << "Failed to receive data" << std::endl;
            continue;
        }

        const char* pong = "pong";
        ssize_t sent = sendto(
            sockfd,
            pong,
            strlen(pong), // no need to send null terminator
            0, // no flags
            reinterpret_cast<sockaddr*>(&client_addr),
            client_addr_len
        );
        if (sent < 0) {
            std::cerr << "Failed to send data" << std::endl;
        }

    }
    close(sockfd);
    return 0;


}