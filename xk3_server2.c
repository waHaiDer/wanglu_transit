// server2.c — LAB3 Q2: Sign up & Login, then global chat (tagged by student_id)
// Build: gcc -O2 -Wall -Wextra -o server2 server2.c -lpthread
// Run:   ./server2
//
// Slide requirements implemented:
// - Client must Sign up before Login.
// - Sign up requires student id binding + account & password length 8–15.
// - Password must contain at least one uppercase letter and one symbol.
// - Server records successful registrations: (student_id, account, password).
// - On LOGIN, server checks if account exists and password correct; otherwise refuse.
// - After successful login, user can chat; messages are broadcast and tagged with student_id.
// - Max 5 concurrent connections; 6th gets "Server is full!" and connection is closed.
// - Server replies/prints always end with '\n'.
//
// Protocol (line-based):
//   SIGNUP\nSID:<sid>\nACC:<acc>\nPWD:<pwd>\n
//   LOGIN\nACC:<acc>\nPWD:<pwd>\n
//   CHAT\n<message>\n
//   EXIT!\n
//
// Notes:
// - Server enforces only the connection cap; input-format/length rules are enforced by client
//   and rechecked on server (defense in depth).

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define PORT 5678
#define MAX_CLIENTS 5
#define MAX_USERS   128
#define BUF_SZ 4096

typedef struct {
    char sid[64];
    char acc[64];
    char pwd[64];
    bool in_use;
} user_t;

typedef struct {
    int fd;
    bool in_use;
    bool authed;
    int user_idx;       // index in users[]
} client_t;

static user_t users[MAX_USERS];
static client_t clients[MAX_CLIENTS];
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static void safe_send(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0) { if (errno == EINTR) continue; break; }
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

static int online_count(void) {
    int c = 0;
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < MAX_CLIENTS; ++i) if (clients[i].in_use) c++;
    pthread_mutex_unlock(&mtx);
    return c;
}

static int add_client(int fd) {
    int idx = -1;
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].in_use) {
            clients[i].in_use = true;
            clients[i].fd = fd;
            clients[i].authed = false;
            clients[i].user_idx = -1;
            idx = i;
            break;
        }
    }
    pthread_mutex_unlock(&mtx);
    return idx;
}

static void remove_client(int fd) {
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].in_use && clients[i].fd == fd) {
            clients[i].in_use = false;
            clients[i].authed = false;
            clients[i].user_idx = -1;
            clients[i].fd = -1;
            break;
        }
    }
    pthread_mutex_unlock(&mtx);
}

static void broadcast_authed(const char *fmt, ...) {
    char line[BUF_SZ];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t L = strlen(line);
    if (L == 0 || line[L-1] != '\n') {
        if (L + 1 < sizeof(line)) { line[L] = '\n'; line[L+1] = '\0'; L++; }
    }

    pthread_mutex_lock(&mtx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].in_use && clients[i].authed) {
            safe_send(clients[i].fd, line, L);
        }
    }
    pthread_mutex_unlock(&mtx);
}

static int users_find_by_acc(const char *acc) {
    for (int i = 0; i < MAX_USERS; ++i) {
        if (users[i].in_use && strcmp(users[i].acc, acc) == 0) return i;
    }
    return -1;
}

static int users_find_free(void) {
    for (int i = 0; i < MAX_USERS; ++i) if (!users[i].in_use) return i;
    return -1;
}

static bool valid_len(const char *s) {
    size_t L = strlen(s);
    return L >= 8 && L <= 15;
}

static bool contains_upper_and_symbol(const char *s) {
    bool upper = false, symbol = false;
    for (const unsigned char *p=(const unsigned char*)s; *p; ++p) {
        if (*p >= 'A' && *p <= 'Z') upper = true;
        if (!( (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ))
            symbol = true;
    }
    return upper && symbol;
}

static bool parse_kv(const char *line, const char *key, char *out, size_t outcap) {
    // expects "KEY:value"
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) return false;
    if (line[klen] != ':') return false;
    snprintf(out, outcap, "%s", line + klen + 1);
    // strip trailing newline if any
    size_t L = strlen(out);
    while (L && (out[L-1] == '\r' || out[L-1] == '\n')) { out[L-1] = '\0'; L--; }
    return true;
}

typedef struct { int fd; } thread_arg_t;

static void *client_thread(void *arg) {
    thread_arg_t *ta = (thread_arg_t*)arg;
    int cfd = ta->fd;
    free(ta);

    char line[BUF_SZ];

    while (1) {
        ssize_t n = recv_line(cfd, line, sizeof(line));
        if (n <= 0) break;

        if (strncmp(line, "SIGNUP", 6) == 0) {
            char sid[64]="", acc[64]="", pwd[64]="";
            // Expect 3 following lines
            if (recv_line(cfd, line, sizeof(line)) <= 0) break;
            if (!parse_kv(line, "SID", sid, sizeof(sid))) { send_line(cfd, "FAIL SIGNUP: bad SID"); continue; }

            if (recv_line(cfd, line, sizeof(line)) <= 0) break;
            if (!parse_kv(line, "ACC", acc, sizeof(acc))) { send_line(cfd, "FAIL SIGNUP: bad ACC"); continue; }

            if (recv_line(cfd, line, sizeof(line)) <= 0) break;
            if (!parse_kv(line, "PWD", pwd, sizeof(pwd))) { send_line(cfd, "FAIL SIGNUP: bad PWD"); continue; }

            // Validate lengths and password policy
            if (!valid_len(acc) || !valid_len(pwd) || !contains_upper_and_symbol(pwd)) {
                send_line(cfd, "FAIL SIGNUP: policy violation");
                continue;
            }

            pthread_mutex_lock(&mtx);
            if (users_find_by_acc(acc) != -1) {
                pthread_mutex_unlock(&mtx);
                send_line(cfd, "FAIL SIGNUP: account exists");
                continue;
            }
            int ui = users_find_free();
            if (ui == -1) {
                pthread_mutex_unlock(&mtx);
                send_line(cfd, "FAIL SIGNUP: user DB full");
                continue;
            }
            users[ui].in_use = true;
            snprintf(users[ui].sid, sizeof(users[ui].sid), "%s", sid);
            snprintf(users[ui].acc, sizeof(users[ui].acc), "%s", acc);
            snprintf(users[ui].pwd, sizeof(users[ui].pwd), "%s", pwd);
            pthread_mutex_unlock(&mtx);

            send_line(cfd, "OK SIGNUP");
        }
        else if (strncmp(line, "LOGIN", 5) == 0) {
            char acc[64]="", pwd[64]="";
            if (recv_line(cfd, line, sizeof(line)) <= 0) break;
            if (!parse_kv(line, "ACC", acc, sizeof(acc))) { send_line(cfd, "FAIL LOGIN: bad ACC"); continue; }
            if (recv_line(cfd, line, sizeof(line)) <= 0) break;
            if (!parse_kv(line, "PWD", pwd, sizeof(pwd))) { send_line(cfd, "FAIL LOGIN: bad PWD"); continue; }

            pthread_mutex_lock(&mtx);
            int ui = users_find_by_acc(acc);
            if (ui == -1 || strcmp(users[ui].pwd, pwd) != 0) {
                pthread_mutex_unlock(&mtx);
                send_line(cfd, "FAIL LOGIN: invalid credentials");
                continue;
            }
            // Bind this connection to the authed user
            for (int i=0;i<MAX_CLIENTS;++i) {
                if (clients[i].in_use && clients[i].fd == cfd) {
                    clients[i].authed = true;
                    clients[i].user_idx = ui;
                    break;
                }
            }
            char sid[64]; snprintf(sid, sizeof(sid), "%s", users[ui].sid);
            pthread_mutex_unlock(&mtx);

            send_line(cfd, "OK LOGIN sid:%s", sid);
            // (Optional) announce join
            broadcast_authed("SYSTEM: %s is online", users[ui].sid);
        }
        else if (strncmp(line, "CHAT", 4) == 0) {
            // Next line is message body
            char msg[BUF_SZ];
            if (recv_line(cfd, msg, sizeof(msg)) <= 0) break;
            // strip newline
            size_t L = strlen(msg);
            while (L && (msg[L-1]=='\n'||msg[L-1]=='\r')) { msg[L-1]='\0'; L--; }

            pthread_mutex_lock(&mtx);
            const char *sid = NULL;
            for (int i=0;i<MAX_CLIENTS;++i) {
                if (clients[i].in_use && clients[i].fd == cfd && clients[i].authed) {
                    sid = users[clients[i].user_idx].sid;
                    break;
                }
            }
            pthread_mutex_unlock(&mtx);

            if (!sid) { send_line(cfd, "note: please LOGIN first"); continue; }
            broadcast_authed("[%s]: %s", sid, msg);
        }
        else if (strncmp(line, "EXIT!", 5) == 0) {
            break;
        }
        else {
            send_line(cfd, "unknown command");
        }
    }

    // Optional: announce offline if authed
    pthread_mutex_lock(&mtx);
    const char *sid = NULL;
    for (int i=0;i<MAX_CLIENTS;++i) if (clients[i].in_use && clients[i].fd == cfd && clients[i].authed) {
        sid = users[clients[i].user_idx].sid; break;
    }
    pthread_mutex_unlock(&mtx);
    if (sid) broadcast_authed("SYSTEM: %s is offline", sid);

    close(cfd);
    remove_client(cfd);
    pthread_exit(NULL);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_ANY); sa.sin_port = htons(PORT);
    if (bind(srv, (struct sockaddr*)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    if (listen(srv, 16) < 0) { perror("listen"); return 1; }

    printf("Q2 Server listening on %d (max %d clients)\n", PORT, MAX_CLIENTS);

    while (1) {
        struct sockaddr_in ca; socklen_t calen = sizeof(ca);
        int cfd = accept(srv, (struct sockaddr*)&ca, &calen);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); continue; }

        if (online_count() >= MAX_CLIENTS) {
            const char *full = "Server is full!\n";
            safe_send(cfd, full, strlen(full));
            close(cfd);
            continue;
        }
        if (add_client(cfd) < 0) {
            const char *full = "Server is full!\n";
            safe_send(cfd, full, strlen(full));
            close(cfd);
            continue;
        }

        pthread_t th;
        thread_arg_t *ta = (thread_arg_t*)malloc(sizeof(thread_arg_t));
        ta->fd = cfd;
        if (pthread_create(&th, NULL, client_thread, ta) != 0) {
            perror("pthread_create");
            close(cfd);
            remove_client(cfd);
            continue;
        }
        pthread_detach(th);
    }
    return 0;
}
