#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
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

#define SERVER_PORT 5678      // run with ./server1 only
#define MAX_USERS   256
#define MAX_CLIENTS 64
#define MAX_NAME    32
#define MAX_PASS    64
#define MAX_STUID   32
#define MAX_LINE    1024

typedef struct {
    char student_id[MAX_STUID];
    char username[MAX_NAME];
    char password[MAX_PASS];
    bool in_use;
} user_t;

typedef struct {
    int fd;
} client_t;

static user_t users[MAX_USERS];
static client_t clients[MAX_CLIENTS];
static pthread_mutex_t users_mtx   = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clients_mtx = PTHREAD_MUTEX_INITIALIZER;

/* utilities */
static ssize_t send_all(int fd, const void *buf, size_t len){
    const char *p=(const char*)buf; size_t left=len;
    while(left){
        ssize_t n=send(fd,p,left,0);
        if(n<0){ if(errno==EINTR) continue; return -1; }
        left-=(size_t)n; p+=n;
    }
    return (ssize_t)len;
}
static int sendf(int fd, const char *fmt, ...){
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    int n=vsnprintf(buf,sizeof(buf),fmt,ap);
    va_end(ap);
    if(n<0) return -1;
    if((size_t)n>sizeof(buf)) n=(int)sizeof(buf);
    return (int)send_all(fd, buf, (size_t)n);
}
// Read one '\n'-terminated line (without '\n'); returns len, 0 if peer closed before any byte, -1 on error
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

/* user DB helpers (call with users_mtx held when stated) */
static int find_user_locked(const char *username){
    for(int i=0;i<MAX_USERS;i++)
        if(users[i].in_use && strcmp(users[i].username,username)==0) return i;
    return -1;
}
static int add_user_locked(const char *sid, const char *username, const char *password){
    if(find_user_locked(username)!=-1) return -2; // username exists
    for(int i=0;i<MAX_USERS;i++){
        if(!users[i].in_use){
            users[i].in_use=true;
            snprintf(users[i].student_id,sizeof(users[i].student_id),"%s",sid);
            snprintf(users[i].username,  sizeof(users[i].username),  "%s",username);
            snprintf(users[i].password,  sizeof(users[i].password),  "%s",password);
            return i;
        }
    }
    return -1; // full
}

static void remove_client(int fd){
    pthread_mutex_lock(&clients_mtx);
    for(int i=0;i<MAX_CLIENTS;i++){
        if(clients[i].fd==fd){ clients[i].fd=-1; break; }
    }
    pthread_mutex_unlock(&clients_mtx);
}

/* per-connection handler: REGISTER -> LOGIN -> Bye */
static void *client_thread(void *arg){
    int fd = *(int*)arg; free(arg);
    char line[MAX_LINE];

    /* === REGISTER === */
    if(sendf(fd,"=== REGISTER ===\nEnter Student ID:\n")<0){ close(fd); return NULL; }
    ssize_t n = recv_line(fd, line, sizeof(line)); if(n<=0){ close(fd); return NULL; }
    char sid[MAX_STUID]; snprintf(sid,sizeof(sid),"%s",line);

    if(sendf(fd,"Enter new username:\n")<0){ close(fd); return NULL; }
    n = recv_line(fd, line, sizeof(line)); if(n<=0){ close(fd); return NULL; }
    char username[MAX_NAME]; snprintf(username,sizeof(username),"%s",line);

    char password[MAX_PASS];
    while(1){
        if(sendf(fd,"Enter new password (6-15 chars):\n")<0){ close(fd); return NULL; }
        n = recv_line(fd, line, sizeof(line)); if(n<=0){ close(fd); return NULL; }
        size_t L = strlen(line);
        if(L>=6 && L<=15){ snprintf(password,sizeof(password),"%s",line); break; }
        if(sendf(fd,"Password length invalid. Please try again.\n")<0){ close(fd); return NULL; }
    }

    pthread_mutex_lock(&users_mtx);
    int rc = add_user_locked(sid, username, password);
    pthread_mutex_unlock(&users_mtx);

    if(rc == -2){
        sendf(fd,"Username already exists. Proceeding to login...\n");
    }else if(rc < 0){
        sendf(fd,"Server user database full. Disconnecting.\n");
        close(fd); return NULL;
    }else{
        sendf(fd,"Registration OK.\n");
    }

    /* === LOGIN === */
    while(1){
        if(sendf(fd,"=== LOGIN ===\nUsername:\n")<0){ close(fd); return NULL; }
        n = recv_line(fd, line, sizeof(line)); if(n<=0){ close(fd); return NULL; }
        char in_user[MAX_NAME]; snprintf(in_user,sizeof(in_user),"%s",line);

        pthread_mutex_lock(&users_mtx);
        int idx = find_user_locked(in_user);
        if(idx < 0){
            pthread_mutex_unlock(&users_mtx);
            if(sendf(fd,"Wrong ID!!!\n")<0){ close(fd); return NULL; }
            continue;
        }
        char expect_pass[MAX_PASS];
        snprintf(expect_pass,sizeof(expect_pass),"%s",users[idx].password);
        pthread_mutex_unlock(&users_mtx);

        if(sendf(fd,"Password:\n")<0){ close(fd); return NULL; }
        n = recv_line(fd, line, sizeof(line)); if(n<=0){ close(fd); return NULL; }

        if(strcmp(line, expect_pass)!=0){
            if(sendf(fd,"Wrong Password!!!\n")<0){ close(fd); return NULL; }
            continue;
        }

        // success
        sendf(fd,"Login OK.\nBye.\n");
        break;
    }

    close(fd);
    remove_client(fd);
    return NULL;
}

int main(void){
    // init client slots
    for(int i=0;i<MAX_CLIENTS;i++) clients[i].fd = -1;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if(srv<0){ perror("socket"); return 1; }
    int yes=1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(srv,(struct sockaddr*)&addr,sizeof(addr))<0){ perror("bind"); return 1; }
    if(listen(srv,16)<0){ perror("listen"); return 1; }

    printf("Q1 Server listening on port %d ...\n", SERVER_PORT);

    while(1){
        struct sockaddr_in cli; socklen_t clilen = sizeof(cli);
        int *cfd = malloc(sizeof(int));
        if(!cfd){ perror("malloc"); break; }
        *cfd = accept(srv,(struct sockaddr*)&cli,&clilen);
        if(*cfd < 0){ perror("accept"); free(cfd); continue; }

        pthread_mutex_lock(&clients_mtx);
        bool placed=false;
        for(int i=0;i<MAX_CLIENTS;i++){
            if(clients[i].fd <= 0){ clients[i].fd = *cfd; placed=true; break; }
        }
        pthread_mutex_unlock(&clients_mtx);

        if(!placed){
            sendf(*cfd,"Server full. Try later.\n");
            close(*cfd); free(cfd); continue;
        }

        pthread_t th; pthread_create(&th,NULL,client_thread,cfd); pthread_detach(th);
    }

    close(srv);
    return 0;
}
