// client3.c — LAB3 Q3 (fixed)
// Build: gcc -O2 -Wall -Wextra -o client3 client3.c -lpthread
// Run  : ./client3                 (defaults to 127.0.0.1:5678)
//        ./client3 <ip> [port]
//
// Menu:
//   1) Sign up
//   2) Login
//   3) Chat (continuous; /menu to return; Exit to disconnect)
//   4) Create private room
//   5) Leave room
//   q) Exit
//
// Extra commands at menu prompt:
//   /accept <TOKEN>     — accept invite
//   /reject <TOKEN>     — reject invite
//
// Protocol (what this client now sends):
//   SIGNUP + SID/ACC/PWD lines
//   LOGIN  + ACC/PWD lines
//   CHAT <message>                 <-- single-line chat
//   CREATEPRV <sock1> [<sock2>]    <-- no count
//   INVITE_RESP <TOKEN> YES|NO
//   LEAVE
//   EXIT!

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

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define BUF_SZ 4096

static int g_debug = 1;  // set to 0 to silence ">>" send logs

static void safe_send(int fd, const char *buf, size_t len){
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("send"); exit(1);
        }
        if (n == 0) break;
        off += (size_t)n;
    }
}

static void send_line(int fd, const char *fmt, ...){
    char out[BUF_SZ];
    va_list ap; va_start(ap, fmt);
    vsnprintf(out, sizeof(out), fmt, ap);
    va_end(ap);
    size_t L = strlen(out);
    if (L == 0 || out[L-1] != '\n') {
        if (L + 1 < sizeof(out)) { out[L] = '\n'; out[L+1] = '\0'; L++; }
    }
    if (g_debug) {
        // Print without the trailing newline for clarity
        char shown[BUF_SZ];
        strncpy(shown, out, sizeof(shown)-1); shown[sizeof(shown)-1] = '\0';
        size_t SL = strlen(shown);
        if (SL && shown[SL-1]=='\n') shown[SL-1]='\0';
        fprintf(stderr, ">> %s\n", shown);
    }
    safe_send(fd, out, L);
}

static ssize_t recv_line(int fd, char *out, size_t cap){
    size_t pos = 0;
    while (pos + 1 < cap) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0) return 0;          // peer closed
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        out[pos++] = c;
        if (c == '\n') break;
    }
    out[pos] = '\0';
    return (ssize_t)pos;
}

static void trim(char *s){
    size_t L = strlen(s);
    while (L && (s[L-1] == '\n' || s[L-1] == '\r')) { s[L-1] = '\0'; L--; }
}

static bool valid_len(const char *s){ size_t L = strlen(s); return L >= 8 && L <= 15; }
static bool has_upper_symbol(const char *s){
    bool up=false, sym=false;
    for (const unsigned char *p=(const unsigned char*)s; *p; ++p){
        if (*p>='A' && *p<='Z') up=true;
        if (!( (*p>='A'&&*p<='Z') || (*p>='a'&&*p<='z') || (*p>='0'&&*p<='9') )) sym=true;
    }
    return up && sym;
}

typedef struct { int fd; volatile int logged_in; } rx_arg_t;

static void *rx_thread(void *arg){
    rx_arg_t *rx = (rx_arg_t*)arg;
    int fd = rx->fd;
    char line[BUF_SZ];

    while (1) {
        ssize_t n = recv_line(fd, line, sizeof(line));
        if (n <= 0) break;

        fputs(line, stdout);

        if (strstr(line, "OK LOGIN") || strstr(line, "LOGIN OK") || strstr(line, "LOGIN_SUCCESS"))
            rx->logged_in = 1;

        if (!strncmp(line, "Server is full!", 15)) {
            fprintf(stderr, "Disconnected: server reports full capacity.\n");
            shutdown(fd, SHUT_RDWR); close(fd); exit(0);
        }

        // Show how to respond to invites, but do NOT read stdin here
        if (!strncmp(line, "INVITE ", 7)) {
            char token[128]={0};
            if (sscanf(line, "INVITE %127s", token) == 1) {
                printf("Type '/accept %s' to accept, or '/reject %s' to decline.\n", token, token);
            } else {
                printf("To respond, type /accept <TOKEN> or /reject <TOKEN>.\n");
            }
        }
        fflush(stdout);
    }
    // server closed or error
    exit(0);
    return NULL;
}

static void do_signup(int fd){
    char sid[128], acc[128], pwd[128];

    printf("Student ID: "); fflush(stdout);
    if (!fgets(sid, sizeof(sid), stdin)) return; trim(sid);

    while (1) {
        printf("Account (8-15): "); fflush(stdout);
        if (!fgets(acc, sizeof(acc), stdin)) return; trim(acc);
        if (valid_len(acc)) break;
        printf("Invalid account length! Must be 8–15 chars.\n");
    }
    while (1) {
        printf("Password (8-15, include UPPERCASE + symbol): "); fflush(stdout);
        if (!fgets(pwd, sizeof(pwd), stdin)) return; trim(pwd);
        if (valid_len(pwd) && has_upper_symbol(pwd)) break;
        printf("Invalid password policy! (length 8–15, needs uppercase & symbol)\n");
    }

    send_line(fd, "SIGNUP");
    send_line(fd, "SID:%s", sid);
    send_line(fd, "ACC:%s", acc);
    send_line(fd, "PWD:%s", pwd);
}

static void do_login(int fd){
    char acc[128], pwd[128];
    printf("Account: "); fflush(stdout);
    if (!fgets(acc, sizeof(acc), stdin)) return; trim(acc);
    printf("Password: "); fflush(stdout);
    if (!fgets(pwd, sizeof(pwd), stdin)) return; trim(pwd);

    send_line(fd, "LOGIN");
    send_line(fd, "ACC:%s", acc);
    send_line(fd, "PWD:%s", pwd);
}

static void do_chat(int fd, volatile int *logged_in){
    if (!*logged_in) { printf("Please login first.\n"); return; }

    printf("=== Chat mode ===\n");
    printf("Type your message. Commands: '/menu' to return, 'Exit' to disconnect.\n");

    char line[BUF_SZ];
    while (1) {
        if (!fgets(line, sizeof(line), stdin)) return;
        trim(line);

        if (!strcmp(line, "/menu")) return;      // back to main menu
        if (!strcmp(line, "Exit")) {             // disconnect from server
            send_line(fd, "EXIT!");
            return;
        }
        if (line[0] == '\0') continue;           // ignore empty messages

        // **Single-line chat expected by your server**
        send_line(fd, "CHAT %s", line);
    }
}

static void do_create_private(int fd, volatile int *logged_in){
    if (!*logged_in) { printf("Please login first.\n"); return; }
    char line[64];
    printf("How many invitees? (1 or 2): "); fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) return; trim(line);
    int k = atoi(line);
    if (k < 1 || k > 2) { printf("Invalid number (must be 1 or 2).\n"); return; }

    int s1=-1, s2=-1;
    printf("Enter SockID #1: "); fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) return; trim(line); s1 = atoi(line);
    if (k == 2) {
        printf("Enter SockID #2: "); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) return; trim(line); s2 = atoi(line);
    }

    // **No-count format** (most servers): CREATEPRV <sock1> [<sock2>]
    if (k == 1) send_line(fd, "CREATEPRV %d", s1);
    else        send_line(fd, "CREATEPRV %d %d", s1, s2);

    printf("Invite sent. Watch for 'INVITE <TOKEN> ...' then respond with /accept or /reject.\n");
}

int main(int argc, char **argv){
    const char *host = (argc >= 2) ? argv[1] : "127.0.0.1";
    int port = (argc >= 3) ? atoi(argv[2]) : 5678;

    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0){ perror("socket"); return 1; }
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) <= 0){ fprintf(stderr, "inet_pton failed for '%s'\n", host); return 1; }
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0){ perror("connect"); return 1; }

    pthread_t th; rx_arg_t rx = { .fd = fd, .logged_in = 0 };
    if (pthread_create(&th, NULL, rx_thread, &rx) != 0){ perror("pthread_create"); return 1; }
    pthread_detach(th);

    char line[BUF_SZ];
    for (;;) {
        printf("\n== Q3 Menu ==\n"
               "1) Sign up\n"
               "2) Login\n"
               "3) Chat\n"
               "4) Create private room\n"
               "5) Leave room\n"
               "q) Exit\n"
               "(Use /accept <TOKEN> or /reject <TOKEN> for invites)\n> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;
        trim(line);

        if (!strcmp(line, "q")) { send_line(fd, "EXIT!"); break; }
        else if (!strcmp(line, "1")) do_signup(fd);
        else if (!strcmp(line, "2")) do_login(fd);
        else if (!strcmp(line, "3")) do_chat(fd, &rx.logged_in);
        else if (!strcmp(line, "4")) do_create_private(fd, &rx.logged_in);
        else if (!strcmp(line, "5")) send_line(fd, "LEAVE");
        else if (!strncmp(line, "/accept ", 8)) {
            const char *tok = line + 8; while (*tok==' ') ++tok;
            if (*tok) send_line(fd, "INVITE_RESP %s YES", tok); else printf("Usage: /accept <TOKEN>\n");
        }
        else if (!strncmp(line, "/reject ", 8)) {
            const char *tok = line + 8; while (*tok==' ') ++tok;
            if (*tok) send_line(fd, "INVITE_RESP %s NO", tok); else printf("Usage: /reject <TOKEN>\n");
        }
        else {
            printf("Unknown option or command.\n");
        }
    }

    shutdown(fd, SHUT_RDWR); close(fd);
    return 0;
}
