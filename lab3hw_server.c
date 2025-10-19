#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT            5678
#define MAX_CLIENTS     5
#define MAXLINE         2048

typedef enum { ROOM_NONE = 0, ROOM_A = 1, ROOM_B = 2, ROOM_C = 3 } room_t;

typedef struct {
    int   fd;
    int   id;        // assigned client number (monotonic)
    room_t room;     // current room
} client_t;

static client_t clients[MAX_CLIENTS];
static int connected_count = 0;
static int next_client_id = 1;      // assigns IDs in connection order
static int disconnected_total = 0;  // server console only
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static const char *room_name(room_t r) {
    switch (r) {
        case ROOM_A: return "RoomA";
        case ROOM_B: return "RoomB";
        case ROOM_C: return "RoomC";
        default:     return "None";
    }
}
static room_t parse_room_letter(const char *s) {
    if (!s || !*s) return ROOM_NONE;
    char c = (char)toupper((unsigned char)s[0]);
    if (c == 'A') return ROOM_A;
    if (c == 'B') return ROOM_B;
    if (c == 'C') return ROOM_C;
    return ROOM_NONE;
}

static void safe_send(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}
static void sendf(int fd, const char *fmt, ...) {
    char out[MAXLINE];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(out, sizeof(out), fmt, ap);
    va_end(ap);
    if (n > 0) safe_send(fd, out, (size_t)n);
}

static void broadcast_room(room_t room, int except_fd, const char *fmt, ...) {
    char out[MAXLINE];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(out, sizeof(out), fmt, ap);
    va_end(ap);
    if (n <= 0) return;

    pthread_mutex_lock(&mtx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].fd > 0 && clients[i].room == room && clients[i].fd != except_fd) {
            safe_send(clients[i].fd, out, (size_t)n);
        }
    }
    pthread_mutex_unlock(&mtx);
}

static void broadcast_all(const char *fmt, ...) {
    char out[MAXLINE];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(out, sizeof(out), fmt, ap);
    va_end(ap);
    if (n <= 0) return;

    pthread_mutex_lock(&mtx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].fd > 0) {
            safe_send(clients[i].fd, out, (size_t)n);
        }
    }
    pthread_mutex_unlock(&mtx);
}

static int slot_of_fd(int fd) {
    for (int i = 0; i < MAX_CLIENTS; ++i) if (clients[i].fd == fd) return i;
    return -1;
}

static void remove_client_locked(int fd) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].fd == fd) {
            clients[i].fd = 0;
            clients[i].id = 0;
            clients[i].room = ROOM_NONE;
            connected_count--;
            break;
        }
    }
}

// trim CR/LF at end
static void chomp(char *s) {
    size_t L = strlen(s);
    while (L && (s[L-1] == '\n' || s[L-1] == '\r')) s[--L] = '\0';
}

static void *server_admin_thread(void *arg) {
    (void)arg;
    // Read from server stdin and broadcast to ALL rooms as SERVER:
    // Using stdio blocking read is fine (dedicated thread).
    char line[MAXLINE];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        chomp(line);
        if (line[0] == '\0') continue;
        printf("[ADMIN BROADCAST] %s\n", line);
        broadcast_all("SERVER: %s\n", line);
    }
    return NULL;
}

static void *client_thread(void *arg) {
    int fd = *(int*)arg;
    free(arg);
    pthread_detach(pthread_self());

    char buf[MAXLINE];
    ssize_t n;

    for (;;) {
        n = recv(fd, buf, sizeof(buf)-1, 0);
        if (n <= 0) break; // client closed or error
        buf[n] = '\0';
        chomp(buf);
        if (buf[0] == '\0') continue;

        // Snapshot client info under lock (fd might move rooms)
        int my_id; room_t my_room;
        pthread_mutex_lock(&mtx);
        int idx = slot_of_fd(fd);
        my_id   = (idx >= 0) ? clients[idx].id  : -1;
        my_room = (idx >= 0) ? clients[idx].room: ROOM_NONE;
        pthread_mutex_unlock(&mtx);

        // Commands:
        // 1) EXIT!
        if (strcmp(buf, "EXIT!") == 0) {
            sendf(fd, "SYSTEM: Bye.\n");
            break;
        }

        // 2) ENTER <A|B|C>
        if (strncasecmp(buf, "ENTER ", 6) == 0) {
            const char *arg = buf + 6;
            while (*arg == ' ') arg++;
            room_t r = parse_room_letter(arg);
            if (r == ROOM_NONE) {
                sendf(fd, "SYSTEM: Unknown room. Use: ENTER A|B|C\n");
                continue;
            }
            // Move client into room r
            pthread_mutex_lock(&mtx);
            int ok = 0; int my_slot = slot_of_fd(fd);
            if (my_slot >= 0) {
                room_t old = clients[my_slot].room;
                clients[my_slot].room = r;
                ok = 1;
                // notify others in the NEW room
                int id_for_msg = clients[my_slot].id;
                pthread_mutex_unlock(&mtx);
                sendf(fd, "SYSTEM: You joined %s.\n", room_name(r));
                broadcast_room(r, fd, "SYSTEM [%s]: Client #%d has joined the room.\n",
                               room_name(r), id_for_msg);
                // If coming from a different room, it's okay; no need to notify old room.
            } else {
                pthread_mutex_unlock(&mtx);
            }
            if (!ok) {
                sendf(fd, "SYSTEM: Internal error.\n");
            }
            continue;
        }

        // 3) Otherwise, normal chat
        if (my_room == ROOM_NONE) {
            sendf(fd, "SYSTEM: You are not in a room. Use: ENTER A|B|C\n");
            continue;
        }
        // Deliver to same-room clients (exclude self), and echo to sender for clarity
        sendf(fd, "Client #%d [%s]: %s\n", my_id, room_name(my_room), buf);
        broadcast_room(my_room, fd, "Client #%d [%s]: %s\n", my_id, room_name(my_room), buf);
    }

    // Cleanup on disconnect
    close(fd);
    pthread_mutex_lock(&mtx);
    int idx = slot_of_fd(fd);
    if (idx >= 0) {
        int id = clients[idx].id;
        room_t r = clients[idx].room;
        remove_client_locked(fd);
        disconnected_total++;
        printf("Client #%d disconnected. Total disconnected so far: %d\n",
               id, disconnected_total);
        // Inform remaining members in that room (optional)
        if (r != ROOM_NONE) {
            pthread_mutex_unlock(&mtx);
            broadcast_room(r, -1, "SYSTEM [%s]: Client #%d has left the room.\n",
                           room_name(r), id);
            return NULL;
        }
    }
    pthread_mutex_unlock(&mtx);
    return NULL;
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

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
    if (listen(srv, 16) < 0) { perror("listen"); exit(1); }

    printf("Server listening on %d (max %d clients). Rooms: A/B/C\n", PORT, MAX_CLIENTS);
    printf("Tip: type in this server console to broadcast -> clients see: SERVER: <text>\n");

    // Start admin broadcast thread (reads server stdin and broadcasts to all rooms)
    pthread_t admin;
    if (pthread_create(&admin, NULL, server_admin_thread, NULL) != 0) {
        perror("pthread_create(admin)"); /* non-fatal */ ;
    }

    for (;;) {
        struct sockaddr_in cli; socklen_t clen = sizeof(cli);
        int cfd = accept(srv, (struct sockaddr*)&cli, &clen);
        if (cfd < 0) { perror("accept"); continue; }

        // Enforce max clients = 5
        int admitted = 0; int assigned_id = -1;
        pthread_mutex_lock(&mtx);
        if (connected_count < MAX_CLIENTS) {
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (clients[i].fd == 0) {
                    clients[i].fd   = cfd;
                    clients[i].id   = next_client_id++;
                    clients[i].room = ROOM_NONE;
                    connected_count++;
                    admitted = 1;
                    assigned_id = clients[i].id;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&mtx);

        if (!admitted) {
            const char *msg = "Server is full!\n";
            safe_send(cfd, msg, strlen(msg));
            close(cfd);
            continue;
        }

        // Greet new client (not in any room yet)
        sendf(cfd, "SYSTEM: Welcome, Client #%d. Use: ENTER A|B|C  |  EXIT!\n", assigned_id);

        int *arg = malloc(sizeof(int));
        *arg = cfd;
        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, arg) != 0) {
            perror("pthread_create");
            // rollback this slot
            pthread_mutex_lock(&mtx);
            int idx = slot_of_fd(cfd);
            if (idx >= 0) { clients[idx].fd = 0; clients[idx].id = 0; clients[idx].room = ROOM_NONE; connected_count--; }
            pthread_mutex_unlock(&mtx);
            close(cfd);
        }
    }
    // close(srv); // not reached
    return 0;
}
