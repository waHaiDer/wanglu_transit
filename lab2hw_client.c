#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define SERVER_IP "127.0.0.1"
#define PORT      5678
#define MAXLINE   2048

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

static void chomp(char *s) {
    size_t L = strlen(s);
    while (L && (s[L-1] == '\n' || s[L-1] == '\r')) s[--L] = '\0';
}

static int valid_A(const char *s) {
    size_t L = strlen(s);
    return (L >= 5 && L <= 10);
}
static int valid_B(const char *s) {
    return (strlen(s) % 2 == 0);
}

int main(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); exit(1); }

    struct sockaddr_in srv; memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(PORT);
    if (inet_pton(AF_INET, SERVER_IP, &srv.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed\n"); close(s); exit(1);
    }
    if (connect(s, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("connect"); close(s); exit(1);
    }

    printf("Connected. Enter lines (Ctrl+D to quit)\n");

    char A[MAXLINE], B[MAXLINE], ID[MAXLINE], recvbuf[MAXLINE];

    for (;;) {
        // Read A
        printf("A: ");
        if (!fgets(A, sizeof(A), stdin)) break; // EOF
        chomp(A);
        if (!valid_A(A)) { puts("error"); continue; }

        // Read B
        printf("B: ");
        if (!fgets(B, sizeof(B), stdin)) break;
        chomp(B);
        if (!valid_B(B)) { puts("error"); continue; }

        // Read StudentID
        printf("StudentID: ");
        if (!fgets(ID, sizeof(ID), stdin)) break;
        chomp(ID);

        // Send three newline-terminated lines
        char line[MAXLINE];
        int n;

        n = snprintf(line, sizeof(line), "%s\n", A);
        if (n < 0 || send_all(s, line, (size_t)n) < 0) { perror("send A"); break; }

        n = snprintf(line, sizeof(line), "%s\n", B);
        if (n < 0 || send_all(s, line, (size_t)n) < 0) { perror("send B"); break; }

        n = snprintf(line, sizeof(line), "%s\n", ID);
        if (n < 0 || send_all(s, line, (size_t)n) < 0) { perror("send ID"); break; }

        // Receive and print reply
        ssize_t r = recv_line(s, recvbuf, sizeof(recvbuf));
        if (r < 0) { perror("recv"); break; }
        if (r == 0) { puts("Server closed."); break; }
        fputs(recvbuf, stdout);
    }

    printf("EOF detected. Closing connection.\n");
    close(s);
    return 0;
}
