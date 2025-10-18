// server_part1.c — Multi-client chat server (limit=2, silently reject 3rd)
// Build: gcc -O2 -Wall -Wextra -o server_part1 server_part1.c -lpthread
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 5678
#define MAX_CLIENTS 2
#define MAXLINE 1024

static int clients[MAX_CLIENTS];
static int client_count = 0;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static void broadcast(int from_fd, const char *msg, size_t len) {
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] > 0 && clients[i] != from_fd) {
            send(clients[i], msg, len, 0);
        }
    }
    pthread_mutex_unlock(&mtx);
}

static void remove_client(int fd) {
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] == fd) {
            clients[i] = 0;
            client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&mtx);
}

static void *connection_handler(void *arg) {
    int fd = *(int*)arg;
    free(arg);
    pthread_detach(pthread_self());

    char buf[MAXLINE];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        broadcast(fd, buf, (size_t)n);
    }
    close(fd);
    remove_client(fd);
    pthread_exit(NULL);
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

    printf("Server (part1) listening on %d...\n", PORT);

    for (;;) {
        struct sockaddr_in cli; socklen_t clen = sizeof(cli);
        int cfd = accept(srv, (struct sockaddr*)&cli, &clen);
        if (cfd < 0) { perror("accept"); continue; }

        // admit up to 2; silently close beyond that
        int admitted = 0;
        pthread_mutex_lock(&mtx);
        if (client_count < MAX_CLIENTS) {
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (clients[i] == 0) { clients[i] = cfd; client_count++; admitted = 1; break; }
            }
        }
        pthread_mutex_unlock(&mtx);

        if (!admitted) {
            // silent reject (no message)
            close(cfd);
            continue;
        }

        int *arg = malloc(sizeof(int));
        *arg = cfd;
        pthread_t th;
        if (pthread_create(&th, NULL, connection_handler, arg) != 0) {
            perror("pthread_create");
            close(cfd);
            remove_client(cfd);
        }
    }
    return 0;
}
