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
#define MAX_CLIENTS 16
#define MAX_USERS   256
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
    int user_idx;   // index in users[]
} client_t;

static user_t users[MAX_USERS];
static client_t clients[MAX_CLIENTS];
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

/* ------------ Small I/O helpers ------------ */
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
        if (n == 0) return 0;           // peer closed
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        out[pos++] = c;
        if (c == '\n') break;
    }
    out[pos] = '\0';
    return (ssize_t)pos;
}

/* ------------ User/Client helpers ------------ */
static int users_find_by_acc(const char *acc) {
    for (int i = 0; i < MAX_USERS; ++i)
        if (users[i].in_use && strcmp(users[i].acc, acc) == 0) return i;
    return -1;
}
static int users_free_slot(void) {
    for (int i = 0; i < MAX_USERS; ++i) if (!users[i].in_use) return i;
    return -1;
}
static bool pass_ok(const char *pwd) {
    size_t L = strlen(pwd);
    return L >= 6 && L <= 15;
}

static int add_client(int fd) {
    pthread_mutex_lock(&mtx);
    int idx = -1;
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
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i].in_use && clients[i].authed)
            safe_send(clients[i].fd, line, L);
    pthread_mutex_unlock(&mtx);
}

static bool parse_kv(const char *line, const char *key, char *out, size_t cap) {
    size_t k = strlen(key);
    if (strncmp(line, key, k) != 0 || line[k] != ':') return false;
    snprintf(out, cap, "%s", line + k + 1);
    size_t L = strlen(out);
    while (L && (out[L-1] == '\n' || out[L-1] == '\r')) { out[L-1] = '\0'; --L; }
    return true;
}

/* ------------ Per-connection worker ------------ */
typedef struct { int fd; } arg_t;

static void *client_thread(void *arg) {
    int cfd = ((arg_t*)arg)->fd;
    free(arg);

    char line[BUF_SZ];

    // 1) expect SIGNUP block
    while (1) {
        ssize_t n = recv_line(cfd, line, sizeof(line));
        if (n <= 0) goto done;

        if (strncmp(line, "SIGNUP", 6) == 0) {
            char sid[64] = "", acc[64] = "", pwd[64] = "";
            if (recv_line(cfd, line, sizeof(line)) <= 0) goto done;
            if (!parse_kv(line, "SID", sid, sizeof(sid))) { send_line(cfd, "FAIL SIGNUP: bad SID"); continue; }
            if (recv_line(cfd, line, sizeof(line)) <= 0) goto done;
            if (!parse_kv(line, "ACC", acc, sizeof(acc))) { send_line(cfd, "FAIL SIGNUP: bad ACC"); continue; }
            if (recv_line(cfd, line, sizeof(line)) <= 0) goto done;
            if (!parse_kv(line, "PWD", pwd, sizeof(pwd))) { send_line(cfd, "FAIL SIGNUP: bad PWD"); continue; }

            if (!pass_ok(pwd)) { send_line(cfd, "FAIL SIGNUP: password length 6..15"); continue; }

            pthread_mutex_lock(&mtx);
            int ui = users_find_by_acc(acc);
            if (ui != -1) { pthread_mutex_unlock(&mtx); send_line(cfd, "FAIL SIGNUP: account exists"); continue; }
            int freei = users_free_slot();
            if (freei == -1) { pthread_mutex_unlock(&mtx); send_line(cfd, "FAIL SIGNUP: user DB full"); continue; }

            users[freei].in_use = true;
            snprintf(users[freei].sid, sizeof(users[freei].sid), "%s", sid);
            snprintf(users[freei].acc, sizeof(users[freei].acc), "%s", acc);
            snprintf(users[freei].pwd, sizeof(users[freei].pwd), "%s", pwd);
            pthread_mutex_unlock(&mtx);

            send_line(cfd, "OK SIGNUP");
            break; // proceed to login phase
        } else {
            send_line(cfd, "note: please SIGNUP first");
        }
    }

    // 2) expect LOGIN block (retry allowed on failure)
    while (1) {
        ssize_t n = recv_line(cfd, line, sizeof(line));
        if (n <= 0) goto done;

        if (strncmp(line, "LOGIN", 5) == 0) {
            char acc[64] = "", pwd[64] = "";
            if (recv_line(cfd, line, sizeof(line)) <= 0) goto done;
            if (!parse_kv(line, "ACC", acc, sizeof(acc))) { send_line(cfd, "FAIL LOGIN: bad ACC"); continue; }
            if (recv_line(cfd, line, sizeof(line)) <= 0) goto done;
            if (!parse_kv(line, "PWD", pwd, sizeof(pwd))) { send_line(cfd, "FAIL LOGIN: bad PWD"); continue; }

            pthread_mutex_lock(&mtx);
            int ui = users_find_by_acc(acc);
            if (ui == -1) {
                pthread_mutex_unlock(&mtx);
                send_line(cfd, "Wrong ID!!!");
                continue;  // allow retry
            }
            if (strcmp(users[ui].pwd, pwd) != 0) {
                pthread_mutex_unlock(&mtx);
                send_line(cfd, "Wrong Password!!!");
                continue;  // allow retry
            }
            // bind this socket to authed user
            for (int i = 0; i < MAX_CLIENTS; ++i)
                if (clients[i].in_use && clients[i].fd == cfd) { clients[i].authed = true; clients[i].user_idx = ui; break; }
            char sid[64]; snprintf(sid, sizeof(sid), "%s", users[ui].sid);
            pthread_mutex_unlock(&mtx);

            send_line(cfd, "OK LOGIN sid:%s", sid);
            broadcast_authed("SYSTEM: %s joined the chat", sid);
            break; // go to chat loop
        } else {
            send_line(cfd, "note: please LOGIN");
        }
    }

    // 3) chat loop
    while (1) {
        ssize_t n = recv_line(cfd, line, sizeof(line));
        if (n <= 0) break;

        if (strncmp(line, "CHAT", 4) == 0) {
            char msg[BUF_SZ];
            if (recv_line(cfd, msg, sizeof(msg)) <= 0) break;
            size_t L = strlen(msg);
            while (L && (msg[L-1] == '\n' || msg[L-1] == '\r')) { msg[L-1] = '\0'; --L; }

            pthread_mutex_lock(&mtx);
            const char *sid = NULL;
            for (int i = 0; i < MAX_CLIENTS; ++i)
                if (clients[i].in_use && clients[i].fd == cfd && clients[i].authed) {
                    sid = users[clients[i].user_idx].sid; break;
                }
            pthread_mutex_unlock(&mtx);

            if (!sid) { send_line(cfd, "note: not logged in"); continue; }
            broadcast_authed("[%s]: %s", sid, msg[0] ? msg : "(empty)");
        } else if (strncmp(line, "EXIT!", 5) == 0) {
            break;
        } else {
            send_line(cfd, "unknown command");
        }
    }

done:
    // farewell
    pthread_mutex_lock(&mtx);
    const char *sid = NULL;
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i].in_use && clients[i].fd == cfd && clients[i].authed) {
            sid = users[clients[i].user_idx].sid; break;
        }
    pthread_mutex_unlock(&mtx);
    if (sid) broadcast_authed("SYSTEM: %s left the chat", sid);

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
    if (listen(srv, 32) < 0) { perror("listen"); return 1; }

    printf("Q1 Server listening on %d\n", PORT);

    while (1) {
        struct sockaddr_in ca; socklen_t cal = sizeof(ca);
        int cfd = accept(srv, (struct sockaddr*)&ca, &cal);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); continue; }

        if (add_client(cfd) < 0) { const char *m = "Server busy\n"; safe_send(cfd, m, strlen(m)); close(cfd); continue; }

        pthread_t th; arg_t *a = (arg_t*)malloc(sizeof(*a)); a->fd = cfd;
        if (pthread_create(&th, NULL, client_thread, a) != 0) { perror("pthread_create"); close(cfd); remove_client(cfd); continue; }
        pthread_detach(th);
    }
    return 0;
}
