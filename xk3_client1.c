// client1.c — LAB3 Q1
// Build: gcc -O2 -Wall -Wextra -o client1 client1.c -lpthread
// Run:   ./client1 127.0.0.1 5678
//
// Client features per spec:
//  - User chooses: broadcast OR send to a specific SockID
//  - Broadcast must be 10~15 characters; otherwise print "invalid content!" and re-prompt
//  - Messages and server replies printed immediately
//  - If "Server is full!" is received at connect, print and exit
//
// Protocol to server (line based):
//   BROADCAST\n
//   <message>\n
// or
//   UNICAST <sockid>\n
//   <message>\n

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BUF_SZ 2048

static void safe_send(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("send");
            exit(1);
        }
        if (n == 0) break;
        sent += (size_t)n;
    }
}

static void send_line(int fd, const char *fmt, ...) {
    char msg[BUF_SZ];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    size_t L = strlen(msg);
    if (L == 0 || msg[L - 1] != '\n') {
        if (L + 1 < sizeof(msg)) {
            msg[L] = '\n';
            msg[L + 1] = '\0';
            L++;
        }
    }
    safe_send(fd, msg, L);
}

static ssize_t recv_line(int fd, char *out, size_t cap) {
    size_t pos = 0;
    while (pos + 1 < cap) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0) return 0; // server closed
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        out[pos++] = c;
        if (c == '\n') break;
    }
    out[pos] = '\0';
    return (ssize_t)pos;
}

typedef struct {
    int fd;
} rx_arg_t;

static void *rx_thread(void *arg) {
    rx_arg_t *rx = (rx_arg_t *)arg;
    int fd = rx->fd;
    free(rx);
    char line[BUF_SZ];
    while (1) {
        ssize_t n = recv_line(fd, line, sizeof(line));
        if (n <= 0) break;
        fputs(line, stdout);
        // Ensure a newline if server somehow missed it
        if (line[n-1] != '\n') fputc('\n', stdout);
        fflush(stdout);
        // Detect "Server is full!" immediately after connect
        if (strncmp(line, "Server is full!", 15) == 0) {
            // Close and exit
            shutdown(fd, SHUT_RDWR);
            close(fd);
            exit(0);
        }
    }
    // Server closed
    exit(0);
    return NULL;
}

static void trim_newline(char *s) {
    size_t L = strlen(s);
    while (L > 0 && (s[L-1] == '\n' || s[L-1] == '\r')) { s[L-1] = '\0'; L--; }
}

int main(int argc, char **argv) {
    const char *ip = "127.0.0.1";
    int port = 5678;

    if (argc >= 3) {
        ip = argv[1];
        port = atoi(argv[2]);
    } else {
        fprintf(stderr, "No args supplied. Using default %s:%d\n", ip, port);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) <= 0) { perror("inet_pton"); return 1; }

    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        perror("connect");
        return 1;
    }


    // Main loop: menu + input
    char line[BUF_SZ];
    while (1) {
        printf("\nChoose action:\n");
        printf("  1) Broadcast (10~15 chars)\n");
        printf("  2) Send to SockID\n");
        printf("  q) Quit\n");
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break; // EOF
        trim_newline(line);

        if (strcmp(line, "q") == 0) break;

        if (strcmp(line, "1") == 0) {
            // Broadcast flow
            char msg[BUF_SZ];
            while (1) {
                printf("Enter broadcast content (10~15 chars): ");
                fflush(stdout);
                if (!fgets(msg, sizeof(msg), stdin)) goto done;
                trim_newline(msg);
                size_t L = strlen(msg);
                if (L >= 10 && L <= 15) break;
                printf("invalid content!\n");
            }
            send_line(fd, "BROADCAST");
            send_line(fd, "%s", msg);
        } else if (strcmp(line, "2") == 0) {
            // Unicast flow
            printf("Enter target SockID (integer): ");
            fflush(stdout);
            if (!fgets(line, sizeof(line), stdin)) goto done;
            trim_newline(line);
            int target = atoi(line);

            char msg[BUF_SZ];
            printf("Enter message: ");
            fflush(stdout);
            if (!fgets(msg, sizeof(msg), stdin)) goto done;
            trim_newline(msg);

            char head[128];
            snprintf(head, sizeof(head), "UNICAST %d", target);
            send_line(fd, "%s", head);
            send_line(fd, "%s", msg);
        } else {
            printf("unknown option\n");
        }
    }

done:
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return 0;
}
