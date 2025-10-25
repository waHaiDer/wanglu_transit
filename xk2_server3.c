#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define SERVER_PORT 5680
#define MAX_CLIENTS 64
#define MAX_LINE    2048

typedef struct { int fd; } client_t;
static client_t clients[MAX_CLIENTS];
static pthread_mutex_t clients_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ---------- IO helpers ---------- */
static ssize_t send_all(int fd, const void *buf, size_t len){
    const char *p=(const char*)buf; size_t left=len;
    while(left){
        ssize_t n=send(fd,p,left,0);
        if(n<0){ if(errno==EINTR) continue; return -1; }
        left -= (size_t)n; p += n;
    }
    return (ssize_t)len;
}
static int sendf(int fd, const char *fmt, ...){
    char buf[4096];
    va_list ap; va_start(ap, fmt);
    int n=vsnprintf(buf,sizeof(buf),fmt,ap);
    va_end(ap);
    if(n<0) return -1;
    if((size_t)n>sizeof(buf)) n=(int)sizeof(buf);
    return (int)send_all(fd, buf, (size_t)n);
}
static ssize_t recv_line(int fd, char *out, size_t cap){
    size_t i=0;
    while(i+1<cap){
        char c; ssize_t n=recv(fd,&c,1,0);
        if(n==0){ if(i==0) return 0; break; }
        if(n<0){ if(errno==EINTR) continue; return -1; }
        if(c=='\r') continue;
        if(c=='\n') break;
        out[i++]=c;
    }
    out[i]='\0';
    return (ssize_t)i;
}

/* ---------- Handlers ---------- */
static void handle_A(int fd){
    // Expect: A, then A_value, then B_value
    char line[MAX_LINE];
    long long A,B;
    if(recv_line(fd,line,sizeof(line))<=0) return;
    A = atoll(line);
    if(recv_line(fd,line,sizeof(line))<=0) return;
    B = atoll(line);

    if(A==-1 || B==-1){
        sendf(fd,"Request Denied\nEND\n");
        return;
    }

    // compute safely; division shown as floating with 6dp; handle div by zero
    long long add = A + B;
    long long sub = A - B;
    long long mul = A * B;
    char divbuf[64];
    if(B==0){
        snprintf(divbuf,sizeof(divbuf),"INF");
    }else{
        double q = (double)A / (double)B;
        snprintf(divbuf,sizeof(divbuf),"%.6f", q);
    }

    sendf(fd,
          "SUM=%lld\nSUB=%lld\nMUL=%lld\nDIV=%s\nEND\n",
          add, sub, mul, divbuf);
}

static void handle_B(int fd){
    // Expect: B, then N, then N numbers
    char line[MAX_LINE];
    if(recv_line(fd,line,sizeof(line))<=0) return;
    int n = atoi(line);
    if(n<0 || n>1000) n = 0; // guard

    long long *arr = (long long*)malloc(sizeof(long long)* (size_t)n);
    int zeros=0;
    for(int i=0;i<n;i++){
        if(recv_line(fd,line,sizeof(line))<=0){ n=i; break; }
        long long v = atoll(line);
        if(v==0) zeros++;
        arr[i]=v;
    }

    if(zeros>=2){
        sendf(fd,"No valid data\nEND\n");
        free(arr);
        return;
    }

    // sort ascending
    for(int i=0;i<n;i++){
        for(int j=i+1;j<n;j++){
            if(arr[j]<arr[i]){
                long long t=arr[i]; arr[i]=arr[j]; arr[j]=t;
            }
        }
    }

    sendf(fd,"SORTED:");
    for(int i=0;i<n;i++) sendf(fd," %lld", arr[i]);
    sendf(fd,"\nEND\n");
    free(arr);
}

static void handle_C(int fd){
    // Expect: C, then one line of raw text (letters counted)
    char line[MAX_LINE];
    if(recv_line(fd,line,sizeof(line))<=0) return;
    int cnt[26]={0};
    for(char *p=line; *p; ++p){
        char c=*p;
        if(c>='A'&&c<='Z') cnt[c-'A']++;
        else if(c>='a'&&c<='z') cnt[c-'a']++;
    }
    // print only letters that appeared, a→z
    bool any=false;
    for(int i=0;i<26;i++) if(cnt[i]){ any=true; break; }
    if(!any){ sendf(fd,"No letters found.\nEND\n"); return; }

    for(int i=0;i<26;i++){
        if(cnt[i]) sendf(fd,"%c: %d\n", 'a'+i, cnt[i]);
    }
    sendf(fd,"END\n");
}

/* ---------- Per client thread ---------- */
static void *client_thread(void *arg){
    int fd = *(int*)arg; free(arg);
    char line[MAX_LINE];

    // Session loop: receive a request code then payload
    while(1){
        ssize_t n = recv_line(fd, line, sizeof(line));
        if(n<=0) break;

        if(strcmp(line,"A")==0){
            handle_A(fd);
        }else if(strcmp(line,"B")==0){
            handle_B(fd);
        }else if(strcmp(line,"C")==0){
            handle_C(fd);
        }else if(strcmp(line,"Q")==0){
            sendf(fd,"Bye.\nEND\n");
            break;
        }else{
            sendf(fd,"Unknown request.\nEND\n");
        }
    }

    close(fd);
    pthread_mutex_lock(&clients_mtx);
    for(int i=0;i<MAX_CLIENTS;i++) if(clients[i].fd==fd){ clients[i].fd=-1; break; }
    pthread_mutex_unlock(&clients_mtx);
    return NULL;
}

int main(void){
    for(int i=0;i<MAX_CLIENTS;i++) clients[i].fd=-1;

    int srv=socket(AF_INET,SOCK_STREAM,0);
    if(srv<0){ perror("socket"); return 1; }
    int yes=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));

    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET; addr.sin_port=htons((uint16_t)SERVER_PORT);
    addr.sin_addr.s_addr=INADDR_ANY;

    if(bind(srv,(struct sockaddr*)&addr,sizeof(addr))<0){ perror("bind"); return 1; }
    if(listen(srv,16)<0){ perror("listen"); return 1; }

    printf("Q3 Server listening on port %d ...\n", SERVER_PORT);

    while(1){
        struct sockaddr_in cli; socklen_t clilen=sizeof(cli);
        int *cfd = malloc(sizeof(int));
        if(!cfd){ perror("malloc"); break; }
        *cfd = accept(srv,(struct sockaddr*)&cli,&clilen);
        if(*cfd<0){ perror("accept"); free(cfd); continue; }

        pthread_mutex_lock(&clients_mtx);
        bool placed=false;
        for(int i=0;i<MAX_CLIENTS;i++){
            if(clients[i].fd<=0){ clients[i].fd=*cfd; placed=true; break; }
        }
        pthread_mutex_unlock(&clients_mtx);

        if(!placed){
            sendf(*cfd,"Server full.\nEND\n");
            close(*cfd); free(cfd); continue;
        }
        pthread_t th; pthread_create(&th,NULL,client_thread,cfd); pthread_detach(th);
    }
    close(srv);
    return 0;
}
