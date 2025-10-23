// server1.c — LAB3 Q1
// Build: gcc -O2 -Wall -Wextra -o server1 server1.c -lpthread
// Run:   ./server1
// Behavior per spec:
//  - Up to 5 clients; 6th -> "Server is full!" then close.
//  - Two request types from client: BROADCAST + message, UNICAST <sockid> + message
//  - Broadcast throttled: >= 5 seconds between a client's broadcasts, else "broadcast request denied!"
//  - Every delivered message is prefixed with sender SockID (the socket FD)

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PORT 5678
#define MAX_CLIENTS 5
#define BUF_SZ 2048

typedef struct {
    int fd;
    time_t last_broadcast; // last broadcast timestamp for rate limiting
    bool in_use;
} client_t;

static client_t clients[MAX_CLIENTS];
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static void safe_send(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
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
    // Ensure newline termination as required
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
        if (n == 0) return 0;        // peer closed
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

static int add_client(int fd) {
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].in_use) {
            clients[i].in_use = true;
            clients[i].fd = fd;
            clients[i].last_broadcast = 0;
            pthread_mutex_unlock(&mtx);
            return 0;
        }
    }
    pthread_mutex_unlock(&mtx);
    return -1;
}

static void remove_client(int fd) {
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].in_use && clients[i].fd == fd) {
            clients[i].in_use = false;
            clients[i].fd = -1;
            clients[i].last_broadcast = 0;
            break;
        }
    }
    pthread_mutex_unlock(&mtx);
}

static int online_count(void) {
    int c = 0;
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < MAX_CLIENTS; ++i) if (clients[i].in_use) c++;
    pthread_mutex_unlock(&mtx);
    return c;
}

static void broadcast_all_prefixed(int sender_fd, const char *payload) {
    char line[BUF_SZ];
    snprintf(line, sizeof(line), "[SockID %d]: %s", sender_fd, payload);
    // Make sure ends with '\n'
    size_t L = strlen(line);
    if (L == 0 || line[L - 1] != '\n') {
        if (L + 1 < sizeof(line)) {
            line[L] = '\n';
            line[L + 1] = '\0';
        }
    }
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].in_use) {
            safe_send(clients[i].fd, line, strlen(line));
        }
    }
    pthread_mutex_unlock(&mtx);
}

static bool send_to_sockid_prefixed(int sender_fd, int target_fd, const char *payload) {
    bool ok = false;
    char line[BUF_SZ];
    snprintf(line, sizeof(line), "[SockID %d]: %s", sender_fd, payload);
    size_t L = strlen(line);
    if (L == 0 || line[L - 1] != '\n') {
        if (L + 1 < sizeof(line)) {
            line[L] = '\n';
            line[L + 1] = '\0';
            L++;
        }
    }
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].in_use && clients[i].fd == target_fd) {
            safe_send(clients[i].fd, line, L);
            ok = true;
            break;
        }
    }
    pthread_mutex_unlock(&mtx);
    return ok;
}

typedef struct {
    int fd;
} thread_arg_t;

static void *client_thread(void *arg) {
    thread_arg_t *t = (thread_arg_t *)arg;
    int cfd = t->fd;
    free(t);

    char buf[BUF_SZ];
    // Announce connection info (optional)
    // send_line(cfd, "Welcome. Your SockID is %d", cfd);

    while (1) {
        ssize_t n = recv_line(cfd, buf, sizeof(buf));
        if (n <= 0) break; // disconnect or error

        // Strip trailing newline for parsing
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';

        if (strncmp(buf, "BROADCAST", 9) == 0) {
            // Next line must be the message body
            char msg[BUF_SZ];
            ssize_t m = recv_line(cfd, msg, sizeof(msg));
            if (m <= 0) break;
            if (m > 0 && msg[m - 1] == '\n') msg[m - 1] = '\0';

            // Rate limit check
            time_t now = time(NULL);
            bool deny = false;

            pthread_mutex_lock(&mtx);
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (clients[i].in_use && clients[i].fd == cfd) {
                    if (clients[i].last_broadcast != 0 &&
                        (now - clients[i].last_broadcast) < 5) {
                        deny = true;
                    } else {
                        clients[i].last_broadcast = now;
                    }
                    break;
                }
            }
            pthread_mutex_unlock(&mtx);

            if (deny) {
                send_line(cfd, "broadcast request denied!");
                continue;
            }

            // Deliver to all (including sender), prefixed with sender SockID
            broadcast_all_prefixed(cfd, msg);
        } else if (strncmp(buf, "UNICAST", 7) == 0) {
            // Expect: "UNICAST <sockid>"
            int target = -1;
            {
                // parse integer after keyword
                const char *p = buf + 7;
                while (*p == ' ') p++;
                if (*p) target = atoi(p);
            }

            // Next line is the message body
            char msg[BUF_SZ];
            ssize_t m = recv_line(cfd, msg, sizeof(msg));
            if (m <= 0) break;
            if (m > 0 && msg[m - 1] == '\n') msg[m - 1] = '\0';

            if (!send_to_sockid_prefixed(cfd, target, msg)) {
                send_line(cfd, "note: target SockID not online");
            }
        } else {
            // Unknown command; ignore politely
            send_line(cfd, "unknown command");
        }
    }

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

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(srv, 16) < 0) {
        perror("listen");
        return 1;
    }

    printf("Server listening on %d. Max clients = %d\n", PORT, MAX_CLIENTS);

    while (1) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        int cfd = accept(srv, (struct sockaddr*)&cli, &clilen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        // Enforce capacity
        if (online_count() >= MAX_CLIENTS) {
            const char *full = "Server is full!\n";
            safe_send(cfd, full, strlen(full));
            close(cfd);
            continue;
        }

        if (add_client(cfd) != 0) {
            const char *full = "Server is full!\n";
            safe_send(cfd, full, strlen(full));
            close(cfd);
            continue;
        }

        thread_arg_t *ta = (thread_arg_t *)malloc(sizeof(thread_arg_t));
        ta->fd = cfd;
        pthread_t th;
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
