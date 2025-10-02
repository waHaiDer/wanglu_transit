#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>

#define PORT     5678
#define BACKLOG  8
#define MAXLINE  2048

static ssize_t recv_line(int fd, char *buf, size_t cap) {
    size_t used = 0;
    while (used + 1 < cap) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0) return (used == 0) ? 0 : (ssize_t)used; // peer closed
        buf[used++] = c;
        if (c == '\n') break;
    }
    buf[used] = '\0';
    return (ssize_t)used;
}

static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void chomp(char *s) {                   // remove trailing CR/LF
    size_t L = strlen(s);
    while (L && (s[L-1] == '\n' || s[L-1] == '\r')) s[--L] = '\0';
}

int main(void) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(srv); exit(1);
    }
    if (listen(srv, BACKLOG) < 0) {
        perror("listen"); close(srv); exit(1);
    }

    printf("Server is listening on %d...\n", PORT);

    for (;;) { // accept clients forever
        struct sockaddr_in cli; socklen_t clen = sizeof(cli);
        int cfd = accept(srv, (struct sockaddr*)&cli, &clen);
        if (cfd < 0) { perror("accept"); continue; }

        // Handle one client: receive triplets repeatedly until client closes
        char A[MAXLINE], B[MAXLINE], ID[MAXLINE], reply[MAXLINE*2];

        for (;;) {
            ssize_t nA = recv_line(cfd, A, sizeof(A));
            if (nA <= 0) break;   // client closed or error
            chomp(A);

            ssize_t nB = recv_line(cfd, B, sizeof(B));
            if (nB <= 0) break;
            chomp(B);

            // Wait up to 5 seconds for StudentID using select()
            fd_set rfds; FD_ZERO(&rfds); FD_SET(cfd, &rfds);
            struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;

            int ready = select(cfd + 1, &rfds, NULL, NULL, &tv);
            if (ready < 0) { perror("select"); break; }

            if (ready > 0) {
                // try to read one ID line
                ssize_t nID = recv_line(cfd, ID, sizeof(ID));
                if (nID <= 0) break;
                chomp(ID);

                // Build and send "<A> <B>: [ <ID> ]\n"
                int m = snprintf(reply, sizeof(reply),
                                 "%s %s: [ %s ]\n", A, B, ID);
                if (m < 0) { perror("snprintf"); break; }
                // Optional: also print on server
                fputs(reply, stdout);
                if (send_all(cfd, reply, (size_t)m) < 0) { perror("send"); break; }
            } else {
                // timeout: no ID
                const char *msg = "Didn't receive student id\n";
                fputs(msg, stdout);
                if (send_all(cfd, msg, strlen(msg)) < 0) { perror("send"); break; }
            }
        }

        printf("Client disconnected.\n");
        close(cfd);
    }
    // close(srv);  // unreachable in this simple loop
    return 0;
}
