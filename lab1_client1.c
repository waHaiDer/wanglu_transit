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
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); exit(1); }

    struct sockaddr_in srv; memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);

    if (connect(s, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("connect"); close(s); exit(1);
    }

    const char *id = "D0765432"; 
    ssize_t left = (ssize_t)strlen(id), sent = 0;
    while (left > 0) {
        ssize_t n = send(s, id + sent, left, 0);
        if (n < 0) { perror("send"); close(s); exit(1); }
        sent += n; left -= n;
    }

    printf("Sent student ID: %s\n", id);
    close(s);
    return 0;
}
