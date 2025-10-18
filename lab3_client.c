// client.c — Multi-thread chat client (send from stdin, recv from server)
// Build: gcc -O2 -Wall -Wextra -o client client.c -lpthread
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define SERVER_IP "127.0.0.1"
#define PORT 5678
#define MAXLINE 1024

static volatile int running = 1;

static void *recv_thread(void *arg) {
    int fd = *(int*)arg;
    pthread_detach(pthread_self());
    char buf[MAXLINE];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf)-1, 0)) > 0) {
        buf[n] = '\0';
        fputs(buf, stdout);
        fflush(stdout);
    }
    running = 0;
    pthread_exit(NULL);
}

int main(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); exit(1); }

    struct sockaddr_in srv; memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(PORT);
    if (inet_pton(AF_INET, SERVER_IP, &srv.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed\n"); close(s); exit(1);
    }
    if (connect(s, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("connect"); close(s); exit(1);
    }

    printf("Connected to %s:%d\n", SERVER_IP, PORT);

    pthread_t t;
    if (pthread_create(&t, NULL, recv_thread, &s) != 0) {
        perror("pthread_create"); close(s); exit(1);
    }

    // Sender loop: read from stdin and send to server
    char line[MAXLINE];
    while (running && fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);
        if (send(s, line, len, 0) < 0) { perror("send"); break; }
    }

    // EOF or error => close socket; recv thread will exit
    close(s);
    return 0;
}
