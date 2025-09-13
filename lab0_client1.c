// client1.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#define PORT 12345
#define BUFSIZE 1024

int main() {
    int sockfd;
    struct sockaddr_in server_addr;
    char message[BUFSIZE] = "D1133813"; 

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int n = sendto(sockfd, message, strlen(message), 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
    printf("Student ID sent: %s\n", message);

    close(sockfd);
    return 0;
}
