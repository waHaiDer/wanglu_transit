#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define SERVER_IP  "127.0.0.1"
#define PORT       5678
#define MAXLINE    2048

static volatile int running = 1;

static void safe_send(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

static void *recv_thread(void *arg) {
    int fd = *(int*)arg;
    pthread_detach(pthread_self());
    char buf[MAXLINE];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf)-1, 0)) > 0) {
        buf[n] = '\0';
        fputs(buf, stdout);
        // If the server told us it's full, exit gracefully
        if (strstr(buf, "Server is full!") != NULL) {
            running = 0;
            break;
        }
    }
    running = 0;
    return NULL;
}

int main(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); exit(1); }

    struct sockaddr_in srv; memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(PORT);
    if (inet_pton(AF_INET, SERVER_IP, &srv.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed for %s\n", SERVER_IP);
        close(s); exit(1);
    }
    if (connect(s, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("connect"); close(s); exit(1);
    }

    printf("Connected to %s:%d\n", SERVER_IP, PORT);

    pthread_t t;
    if (pthread_create(&t, NULL, recv_thread, &s) != 0) {
        perror("pthread_create"); close(s); exit(1);
    }

    char line[MAXLINE];
    while (running && fgets(line, sizeof(line), stdin) != NULL) {
        // Ensure newline
        size_t L = strlen(line);
        if (L == 0 || line[L-1] != '\n') {
            if (L + 1 < sizeof(line)) { line[L] = '\n'; line[L+1] = '\0'; L++; }
        }
        safe_send(s, line, L);
        if (strncmp(line, "EXIT!", 5) == 0) {
            break;
        }
    }

    close(s);
    return 0;
}
