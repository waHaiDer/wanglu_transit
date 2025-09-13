// server1.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#define PORT 12345
#define BUFSIZE 1024

int main() {
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    char buffer[BUFSIZE];
    socklen_t addr_len = sizeof(client_addr);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(1);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(1);
    }

    printf("Server listening on port %d...\n", PORT);

    int n = recvfrom(sockfd, buffer, BUFSIZE, 0, (struct sockaddr*)&client_addr, &addr_len);
    if (n < 0) {
        perror("Receive failed");
        exit(1);
    }

    buffer[n] = '\0';
    printf("Received student ID: %s\n", buffer);

    close(sockfd);
    return 0;
}
