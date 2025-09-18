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

    printf("TCP server1 listening on %d...\n", PORT);

    struct sockaddr_in cli; socklen_t clen = sizeof(cli);
    int cli_fd = accept(srv, (struct sockaddr*)&cli, &clen);
    if (cli_fd < 0) { perror("accept"); exit(1); }

    char buf[BUFSIZE];
    ssize_t n = recv(cli_fd, buf, sizeof(buf)-1, 0);
    if (n < 0) { perror("recv"); close(cli_fd); close(srv); exit(1); }
    buf[n] = '\0';

    printf("Received student ID: %s\n", buf);

    close(cli_fd);
    close(srv);
    return 0;
}
