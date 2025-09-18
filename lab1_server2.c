#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 5678
#define BUFSIZE 1024

int main(void) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(srv, 5) < 0) {
        perror("listen"); exit(1);
    }

    printf("TCP server2 listening on %d (Ctrl+C to stop)...\n", PORT);

    for (;;) {
        struct sockaddr_in cli; socklen_t clen = sizeof(cli);
        int cfd = accept(srv, (struct sockaddr*)&cli, &clen);
        if (cfd < 0) { perror("accept"); continue; }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        printf("Client connected: %s:%d\n", ip, ntohs(cli.sin_port));

        char buf[BUFSIZE];
        for (;;) {
            ssize_t n = recv(cfd, buf, sizeof(buf)-1, 0);
            if (n < 0) { perror("recv"); break; }
            if (n == 0) { printf("Client disconnected.\n"); break; }
            buf[n] = '\0';
            printf("Received: %s\n", buf);
        }

        close(cfd);
    }
    close(srv);
    return 0;
}
