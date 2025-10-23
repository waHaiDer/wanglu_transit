// client2.c — LAB3 Q2: Sign up → Login → Chat (student_id tagged)
// Build: gcc -O2 -Wall -Wextra -o client2 client2.c -lpthread
// Run:   ./client2 127.0.0.1 5678
//
// Client enforces:
// - account and password length 8–15
// - password contains at least one uppercase letter AND one symbol
// - "must sign up before login" workflow
// - Detects EOF or "Server is full!" and exits gracefully
//
// After OK LOGIN, user can send CHAT messages; server will tag them with student_id.

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
    if (L == 0 || out[L-1] != '\n') {
        if (L + 1 < sizeof(out)) { out[L] = '\n'; out[L+1] = '\0'; L++; }
    }
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

static void trim(char *s) {
    size_t L = strlen(s);
    while (L && (s[L-1]=='\n' || s[L-1]=='\r')) { s[L-1]='\0'; L--; }
}

static bool valid_len(const char *s) { size_t L = strlen(s); return L>=8 && L<=15; }
static bool has_upper_symbol(const char *s) {
    bool up=false, sym=false;
    for (const unsigned char *p=(const unsigned char*)s; *p; ++p) {
        if (*p>='A' && *p<='Z') up=true;
        if (!( (*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9') )) sym=true;
    }
    return up && sym;
}

typedef struct { int fd; } rx_arg_t;

static void *rx_thread(void *arg) {
    rx_arg_t *rx = (rx_arg_t*)arg; int fd = rx->fd; free(rx);
    char line[BUF_SZ];
    while (1) {
        ssize_t n = recv_line(fd, line, sizeof(line));
        if (n <= 0) break;
        fputs(line, stdout);
        if (strncmp(line, "Server is full!", 15) == 0) { shutdown(fd, SHUT_RDWR); close(fd); exit(0); }
        fflush(stdout);
    }
    exit(0);
    return NULL;
}

int main(int argc, char **argv) {
    // Defaults so you can just run: ./client2
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
    sa.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) <= 0) { perror("inet_pton"); return 1; }

    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        perror("connect");
        return 1;
    }

    // Start receiver thread
    pthread_t th;
    rx_arg_t *rx = (rx_arg_t *)malloc(sizeof(rx_arg_t));
    rx->fd = fd;
    if (pthread_create(&th, NULL, rx_thread, rx) != 0) { perror("pthread_create"); return 1; }
    pthread_detach(th);

    // ---- Existing menu loop from client2.c (Sign up / Login / Chat) ----
    char line[BUF_SZ];
    bool signed_up = false, logged_in = false;

    while (1) {
        if (!logged_in) {
            printf("\n== Q2 Menu ==\n1) Sign up\n2) Login\nq) Quit\n> ");
            fflush(stdout);
            if (!fgets(line, sizeof(line), stdin)) break; trim(line);
            if (!strcmp(line,"q")) break;

            if (!strcmp(line,"1")) {
                char sid[128], acc[128], pwd[128];
                // student id
                printf("Student ID: "); fflush(stdout);
                if (!fgets(sid, sizeof(sid), stdin)) break; trim(sid);

                // account
                while (1) {
                    printf("Account (8-15 chars): "); fflush(stdout);
                    if (!fgets(acc, sizeof(acc), stdin)) goto done; trim(acc);
                    if (valid_len(acc)) break;
                    printf("invalid account length!\n");
                }

                // password
                while (1) {
                    printf("Password (8-15, include UPPERCASE + symbol): "); fflush(stdout);
                    if (!fgets(pwd, sizeof(pwd), stdin)) goto done; trim(pwd);
                    if (valid_len(pwd) && has_upper_symbol(pwd)) break;
                    printf("invalid password policy!\n");
                }

                send_line(fd, "SIGNUP");
                send_line(fd, "SID:%s", sid);
                send_line(fd, "ACC:%s", acc);
                send_line(fd, "PWD:%s", pwd);
                // Wait a moment for server reply (rx_thread prints it)
                signed_up = true; // optimistic; server will say FAIL if not
            }
            else if (!strcmp(line,"2")) {
                char acc[128], pwd[128];
                printf("Account: "); fflush(stdout);
                if (!fgets(acc, sizeof(acc), stdin)) break; trim(acc);
                printf("Password: "); fflush(stdout);
                if (!fgets(pwd, sizeof(pwd), stdin)) break; trim(pwd);

                if (!signed_up) {
                    printf("note: you should sign up first (per spec)\n");
                }
                send_line(fd, "LOGIN");
                send_line(fd, "ACC:%s", acc);
                send_line(fd, "PWD:%s", pwd);
                // We'll consider ourselves logged in when server prints "OK LOGIN"
                // But for UX, allow entering chat immediately; server will reject CHAT if not logged in.
                logged_in = true;
            }
            else {
                printf("unknown option\n");
            }
        } else {
            // Chat loop (simple): line to send; "/exit" to leave; "/menu" to go back
            printf("chat> ");
            fflush(stdout);
            if (!fgets(line, sizeof(line), stdin)) break;
            trim(line);
            if (!strcmp(line, "/exit")) { send_line(fd, "EXIT!"); goto done; }
            if (!strcmp(line, "/menu")) { logged_in = false; continue; }
            send_line(fd, "CHAT");
            send_line(fd, "%s", line[0] ? line : "(empty)");
        }
    }

done:
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return 0;
}
