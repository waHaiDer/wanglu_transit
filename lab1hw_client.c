#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define SERVER_IP "127.0.0.1"
#define PORT 5678
#define MAXLINE 2048

static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static ssize_t recv_line(int fd, char *buf, size_t cap) {
    size_t used = 0;
    while (used + 1 < cap) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0) return -1;
        if (n == 0) return (used == 0) ? 0 : (ssize_t)used;
        buf[used++] = c;
        if (c == '\n') break;
    }
    buf[used] = '\0';
    return (ssize_t)used;
}

static void transform_input(const char *in, char *out, size_t out_cap) {
    size_t L = 0; out[0] = '\0';
    for (size_t i = 0; in[i]; ++i) {
        unsigned char ch = (unsigned char)in[i];
        if (!isalnum(ch)) continue;
        char t = isalpha(ch) ? (char)toupper(ch) : (char)ch;
        if (L && L + 1 < out_cap) out[L++] = ' ';
        if (L + 1 < out_cap) out[L++] = t;
        out[L] = '\0';
    }
    if (L + 1 < out_cap) { out[L++] = '\n'; out[L] = '\0'; }
}

int main(void) {
    // 1) create and connect
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); exit(1); }

    struct sockaddr_in srv; memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(PORT);
    if (inet_pton(AF_INET, SERVER_IP, &srv.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed for %s\n", SERVER_IP);
        close(s); exit(1);
    }
    if (connect(s, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("connect"); close(s); exit(1);
    }

    printf("Connected to server:\n");

    char inbuf[MAXLINE], sendbuf[MAXLINE], recvbuf[MAXLINE];

    while (fgets(inbuf, sizeof(inbuf), stdin) != NULL) {
        inbuf[strcspn(inbuf, "\n")] = '\0';
        printf("%s\n", inbuf);

        transform_input(inbuf, sendbuf, sizeof(sendbuf));
        {
            char show[MAXLINE];
            strncpy(show, sendbuf, sizeof(show));
            show[sizeof(show)-1] = '\0';
            show[strcspn(show, "\n")] = '\0';
            printf("Sent: %s\n", show);
        }

        if (send_all(s, sendbuf, strlen(sendbuf)) < 0) {
            perror("send"); close(s); exit(1);
        }

        ssize_t n = recv_line(s, recvbuf, sizeof(recvbuf));
        if (n < 0) { perror("recv"); close(s); exit(1); }
        if (n == 0) { printf("Server closed connection.\n"); close(s); return 0; }

        fputs(recvbuf, stdout); 
    }
    printf("EOF detected. Closing connection.\n");
    close(s);
    return 0;
}
