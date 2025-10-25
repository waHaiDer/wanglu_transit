#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_LINE 1024

static int sockfd;

/* print everything from server */
static void *rx_thread(void *arg){
    (void)arg;
    char buf[2048];
    while(1){
        ssize_t n = recv(sockfd, buf, sizeof(buf)-1, 0);
        if(n == 0){ fprintf(stderr, "\n[Server closed]\n"); exit(0); }
        if(n < 0){
            if(errno == EINTR) continue;
            perror("recv"); exit(1);
        }
        buf[n] = '\0';
        fputs(buf, stdout);
        fflush(stdout);
    }
    return NULL;
}

static int send_line(const char *s){
    char out[MAX_LINE+4];
    snprintf(out,sizeof(out),"%s\n",s);
    size_t len=strlen(out), sent=0;
    while(sent < len){
        ssize_t n = send(sockfd, out+sent, len-sent, 0);
        if(n < 0){ if(errno==EINTR) continue; return -1; }
        sent += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv){
    const char *ip = "127.0.0.1";
    int port = 5678;
    if(argc >= 2) ip   = argv[1];
    if(argc >= 3) port = atoi(argv[2]);
    const char *env_ip = getenv("CHAT_IP");
    const char *env_pt = getenv("CHAT_PORT");
    if(env_ip && *env_ip) ip = env_ip;
    if(env_pt && *env_pt) port = atoi(env_pt);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd < 0){ perror("socket"); return 1; }

    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)port);
    if(inet_pton(AF_INET, ip, &addr.sin_addr) != 1){ fprintf(stderr,"Invalid IP\n"); return 1; }
    if(connect(sockfd,(struct sockaddr*)&addr,sizeof(addr))<0){ perror("connect"); return 1; }

    pthread_t th; pthread_create(&th,NULL,rx_thread,NULL); pthread_detach(th);

    // forward stdin to server
    char line[MAX_LINE];
    while(fgets(line,sizeof(line),stdin)){
        size_t L=strlen(line);
        while(L && (line[L-1]=='\n' || line[L-1]=='\r')) line[--L]='\0';
        if(send_line(line) < 0){ perror("send"); break; }
        // The server closes after “Bye.”, which will terminate the rx_thread
    }

    close(sockfd);
    return 0;
}
