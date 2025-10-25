#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BUF_SZ 4096

static void safe_send(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0) { if (errno == EINTR) continue; perror("send"); exit(1); }
        if (n == 0) break;
        off += (size_t)n;
    }
}
static void send_line(int fd, const char *fmt, ...) {
    char out[BUF_SZ];
    va_list ap; va_start(ap, fmt);
    vsnprintf(out, sizeof(out), fmt, ap);
    va_end(ap);
    size_t L = strlen(out);
    if (L == 0 || out[L-1] != '\n') { if (L + 1 < sizeof(out)) { out[L] = '\n'; out[L+1] = '\0'; L++; } }
    safe_send(fd, out, L);
}
static ssize_t recv_line(int fd, char *out, size_t cap) {
    size_t pos = 0;
    while (pos + 1 < cap) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0) return 0;
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        out[pos++] = c;
        if (c == '\n') break;
    }
    out[pos] = '\0';
    return (ssize_t)pos;
}
static void trim(char *s) { size_t L = strlen(s); while (L && (s[L-1]=='\n'||s[L-1]=='\r')) { s[L-1]='\0'; --L; } }
static bool pass_ok(const char *p) { size_t L = strlen(p); return L >= 6 && L <= 15; }

typedef struct { int fd; } rx_arg_t;

static void *rx_thread(void *arg) {
    rx_arg_t *rx = (rx_arg_t*)arg; int fd = rx->fd; free(rx);
    char line[BUF_SZ];
    while (1) {
        ssize_t n = recv_line(fd, line, sizeof(line));
        if (n <= 0) break;
        fputs(line, stdout);
        fflush(stdout);
    }
    exit(0);
    return NULL;
}

int main(int argc, char **argv) {
    const char *ip = "127.0.0.1";
    int port = 5678;
    if (argc >= 3) { ip = argv[1]; port = atoi(argv[2]); }
    else fprintf(stderr, "No args supplied. Using default %s:%d\n", ip, port);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) <= 0) { perror("inet_pton"); return 1; }
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) { perror("connect"); return 1; }

    // receiver thread
    pthread_t th; rx_arg_t *rx = (rx_arg_t*)malloc(sizeof(rx_arg_t)); rx->fd = fd;
    if (pthread_create(&th, NULL, rx_thread, rx) != 0) { perror("pthread_create"); return 1; }
    pthread_detach(th);

    // 1) SIGNUP
    char sid[128], acc[128], pwd[128], line[BUF_SZ];
    printf("== SIGN UP ==\n");
    printf("Student ID : "); fflush(stdout); if (!fgets(sid, sizeof(sid), stdin)) goto done; trim(sid);

    while (1) {
        printf("New Account (any): "); fflush(stdout);
        if (!fgets(acc, sizeof(acc), stdin)) goto done; trim(acc);
        if (strlen(acc) > 0) break;
        printf("Account cannot be empty.\n");
    }
    while (1) {
        printf("New Password (6..15): "); fflush(stdout);
        if (!fgets(pwd, sizeof(pwd), stdin)) goto done; trim(pwd);
        if (pass_ok(pwd)) break;
        printf("Invalid length. Please try again.\n");
    }
    send_line(fd, "SIGNUP");
    send_line(fd, "SID:%s", sid);
    send_line(fd, "ACC:%s", acc);
    send_line(fd, "PWD:%s", pwd);

    // 2) LOGIN (loop until success)
    printf("\n== LOGIN ==\n");
    while (1) {
        char acc2[128], pwd2[128];
        printf("Account : "); fflush(stdout); if (!fgets(acc2, sizeof(acc2), stdin)) goto done; trim(acc2);
        printf("Password: "); fflush(stdout); if (!fgets(pwd2, sizeof(pwd2), stdin)) goto done; trim(pwd2);

        send_line(fd, "LOGIN");
        send_line(fd, "ACC:%s", acc2);
        send_line(fd, "PWD:%s", pwd2);

        // Give server a moment to respond (rx thread prints messages)
        // User can retry immediately if server said Wrong ID/Password.
        printf("(If you saw \"OK LOGIN\", you may start chatting.)\n");

        // Ask if proceed to chat:
        printf("Proceed to chat? (y/n): "); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) goto done; trim(line);
        if (line[0]=='y' || line[0]=='Y') break;
    }

    // 3) CHAT
    printf("\n== CHAT (type /exit to leave) ==\n");
    while (1) {
        if (!fgets(line, sizeof(line), stdin)) break; trim(line);
        if (!strcmp(line, "/exit")) { send_line(fd, "EXIT!"); break; }
        send_line(fd, "CHAT");
        send_line(fd, "%s", line[0] ? line : "(empty)");
    }

done:
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return 0;
}
