#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 5678
#define BACKLOG 8
#define MAXLINE 2048
static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static ssize_t recv_line(int fd, char *buf, size_t cap) {
    size_t used = 0;
    while (used + 1 < cap) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0) return -1;          
        if (n == 0) return (used == 0) ? 0 : (ssize_t)used;
        buf[used++] = c;
        if (c == '\n') break;
    }
    buf[used] = '\0';
    return (ssize_t)used;
}
static char shift_letter(char ch) {
    if ('A' <= ch && ch <= 'Z') {
        return (ch == 'Z') ? 'A' : (char)(ch + 1);
    }
    return ch;
}

int main(void) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(srv); exit(1);
    }
    if (listen(srv, BACKLOG) < 0) {
        perror("listen"); close(srv); exit(1);
    }

    printf("Server listening on %d...\n", PORT);

    struct sockaddr_in cli;
    socklen_t clen = sizeof(cli);
    int cfd = accept(srv, (struct sockaddr*)&cli, &clen);
    if (cfd < 0) { perror("accept"); close(srv); exit(1); }
    char line[MAXLINE];
    for (;;) {
        ssize_t n = recv_line(cfd, line, sizeof(line));
        if (n < 0) { perror("recv"); close(cfd); close(srv); exit(1); }
        if (n == 0) {
            printf("Client has closed the connection.\n");
            close(cfd);
            close(srv);
            exit(0);
        }
        int letters = 0, numbers = 0;
        char nums_list[MAXLINE] = {0};
        char letters_shift_list[MAXLINE] = {0};
        char tmp[MAXLINE];
        strncpy(tmp, line, sizeof(tmp));
        tmp[sizeof(tmp)-1] = '\0';

        for (char *tok = strtok(tmp, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
            if (tok[0] == '\0') continue;
            for (size_t i = 0; tok[i]; ++i) {
                unsigned char ch = (unsigned char)tok[i];
                if (isdigit(ch)) {
                    numbers++;
                    size_t L = strlen(nums_list);
                    if (L > 0) strncat(nums_list, " ", sizeof(nums_list)-L-1);
                    char one[4]; one[0] = ch; one[1] = '\0';
                    strncat(nums_list, one, sizeof(nums_list)-strlen(nums_list)-1);
                } else if (isalpha(ch)) {
                    letters++;
                    char up = (char)toupper(ch);
                    char sh = shift_letter(up);
                    size_t L2 = strlen(letters_shift_list);
                    if (L2 > 0) strncat(letters_shift_list, " ", sizeof(letters_shift_list)-L2-1);
                    char one[4]; one[0] = sh; one[1] = '\0';
                    strncat(letters_shift_list, one, sizeof(letters_shift_list)-strlen(letters_shift_list)-1);
                }
            }
        }
        char out[MAXLINE];
        snprintf(out, sizeof(out),
                 "letters: %d numbers: %d [%s] and [%s]\n",
                 letters, numbers,
                 (numbers ? nums_list : ""),
                 (letters ? letters_shift_list : ""));

        if (send_all(cfd, out, strlen(out)) < 0) {
            perror("send"); close(cfd); close(srv); exit(1);
        }
    }
}
