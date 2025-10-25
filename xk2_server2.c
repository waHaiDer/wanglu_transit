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
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SERVER_PORT 5679
#define MAX_USERS   3
#define MAX_NAME    32
#define MAX_PASS    64
#define MAX_STUID   32
#define MAX_LINE    1024
#define MAX_CLIENTS 32

typedef struct {
    char student_id[MAX_STUID];
    char username[MAX_NAME];
    char password[MAX_PASS];
    bool in_use;
    time_t login_cooldown_until; // 10s cooldown after login timeout
} user_t;

typedef struct { int fd; } client_t;

static user_t users[MAX_USERS];
static client_t clients[MAX_CLIENTS];
static pthread_mutex_t users_mtx   = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clients_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ---------- I/O helpers ---------- */
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
// blocking line read
static ssize_t recv_line_blocking(int fd, char *out, size_t cap){
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
// timed line read; -2 => timeout
static ssize_t recv_line_timeout(int fd, char *out, size_t cap, int timeout_sec){
    size_t i=0;
    while(i+1<cap){
        fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
        struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
        int rv = select(fd+1, &rfds, NULL, NULL, &tv);
        if(rv == 0) return -2;
        if(rv < 0){ if(errno==EINTR) continue; return -1; }
        char c; ssize_t n=recv(fd,&c,1,0);
        if(n==0){ if(i==0) return 0; break; }
        if(n<0){ if(errno==EINTR) continue; return -1; }
        if(c=='\r') continue;
        if(c=='\n') break;
        out[i++]=c;
        timeout_sec = 2; // after first byte, keep snappy
    }
    out[i]='\0';
    return (ssize_t)i;
}

/* ---------- user DB ---------- */
static int find_user_by_username_locked(const char *u){
    for(int i=0;i<MAX_USERS;i++)
        if(users[i].in_use && strcmp(users[i].username,u)==0) return i;
    return -1;
}
static int find_user_by_stuid_locked(const char *sid){
    for(int i=0;i<MAX_USERS;i++)
        if(users[i].in_use && strcmp(users[i].student_id,sid)==0) return i;
    return -1;
}
static int user_count_locked(void){
    int c=0; for(int i=0;i<MAX_USERS;i++) if(users[i].in_use) c++; return c;
}
static bool has_upper(const char *s){ for(const char *p=s;*p;++p) if(*p>='A'&&*p<='Z') return true; return false; }
static bool has_lower(const char *s){ for(const char *p=s;*p;++p) if(*p>='a'&&*p<='z') return true; return false; }

static int add_user_locked(const char *sid, const char *u, const char *pw){
    if(find_user_by_username_locked(u)!=-1) return -2; // duplicate username
    if(find_user_by_stuid_locked(sid)!=-1)    return -3; // duplicate identity
    for(int i=0;i<MAX_USERS;i++){
        if(!users[i].in_use){
            users[i].in_use = true;
            snprintf(users[i].student_id,sizeof(users[i].student_id),"%s",sid);
            snprintf(users[i].username,  sizeof(users[i].username),  "%s",u);
            snprintf(users[i].password,  sizeof(users[i].password),  "%s",pw);
            users[i].login_cooldown_until = 0;
            return i;
        }
    }
    return -1; // full
}

/* ---------- menu handlers ---------- */
static void show_menu(int fd){
    sendf(fd,
        "\n選擇功能：\n"
        "1. 新增帳號\n"
        "2. 修改密碼\n"
        "3. 登出\n"
        "請輸入選項 (1-3):\n");
}

static void handle_register(int fd){
    char line[MAX_LINE];
    if(sendf(fd,"=== 註冊新帳號 ===\n請輸入學號(Student ID):\n")<0) return;
    if(recv_line_blocking(fd,line,sizeof(line))<=0) return;
    char sid[MAX_STUID]; snprintf(sid,sizeof(sid),"%s",line);

    if(sendf(fd,"請輸入新帳號(Username):\n")<0) return;
    if(recv_line_blocking(fd,line,sizeof(line))<=0) return;
    char uname[MAX_NAME]; snprintf(uname,sizeof(uname),"%s",line);

    char pw[MAX_PASS];
    while(1){
        if(sendf(fd,"請輸入新密碼(6-15字元):\n")<0) return;
        if(recv_line_blocking(fd,line,sizeof(line))<=0) return;
        size_t L=strlen(line);
        if(L>=6 && L<=15){ snprintf(pw,sizeof(pw),"%s",line); break; }
        if(sendf(fd,"密碼長度不符規定，請再試一次。\n")<0) return;
    }

    pthread_mutex_lock(&users_mtx);
    if(user_count_locked() >= MAX_USERS){
        pthread_mutex_unlock(&users_mtx);
        sendf(fd,"帳號資料庫已滿(最多3組)。\n");
        return;
    }
    int rc = add_user_locked(sid, uname, pw);
    pthread_mutex_unlock(&users_mtx);

    if(rc == -2)      sendf(fd,"此帳號(Username)已存在，拒絕新增。\n");
    else if(rc == -3) sendf(fd,"此學號已註冊，拒絕新增。\n");
    else if(rc < 0)   sendf(fd,"註冊失敗。\n");
    else              sendf(fd,"註冊成功。\n");
}

static void handle_change_password(int fd, int user_idx){
    char line[MAX_LINE];
    while(1){
        if(sendf(fd,"=== 修改密碼 ===\n請輸入新密碼(12-20字元，且需包含至少一個大寫與一個小寫):\n")<0) return;
        if(recv_line_blocking(fd,line,sizeof(line))<=0) return;
        size_t L=strlen(line);
        bool ok = (L>=12 && L<=20) && has_upper(line) && has_lower(line);
        if(!ok){
            // exact required sentence:
            if(sendf(fd,"Please enter the new password again.\n")<0) return;
            continue;
        }
        pthread_mutex_lock(&users_mtx);
        snprintf(users[user_idx].password, sizeof(users[user_idx].password), "%s", line);
        pthread_mutex_unlock(&users_mtx);
        sendf(fd,"密碼已更新。\n");
        break;
    }
}

static void post_login_session(int fd, int user_idx){
    char line[MAX_LINE];
    while(1){
        show_menu(fd);
        ssize_t n = recv_line_blocking(fd, line, sizeof(line));
        if(n <= 0) return;
        if(strcmp(line,"1")==0){
            handle_register(fd);
        }else if(strcmp(line,"2")==0){
            handle_change_password(fd, user_idx);
        }else if(strcmp(line,"3")==0){
            sendf(fd,"您已登出。\n");
            return;
        }else{
            sendf(fd,"選項無效。\n");
        }
    }
}

/* ---------- per-connection ---------- */
static void *client_thread(void *arg){
    int fd = *(int*)arg; free(arg);

    while(1){
        // ask username
        if(sendf(fd,"=== 登入(Login) ===\n請輸入帳號(Username):\n")<0){ close(fd); return NULL; }
        char line[MAX_LINE];
        ssize_t n = recv_line_blocking(fd, line, sizeof(line));
        if(n <= 0){ close(fd); return NULL; }
        char in_user[MAX_NAME]; snprintf(in_user,sizeof(in_user),"%s",line);

        // lookup
        pthread_mutex_lock(&users_mtx);
        int idx = find_user_by_username_locked(in_user);
        if(idx < 0){
            pthread_mutex_unlock(&users_mtx);
            if(sendf(fd,"Wrong ID!!!\n")<0){ close(fd); return NULL; }
            continue;
        }
        time_t now = time(NULL);
        if(users[idx].login_cooldown_until > now){
            int wait = (int)(users[idx].login_cooldown_until - now);
            pthread_mutex_unlock(&users_mtx);
            if(sendf(fd,"請稍候 %d 秒再嘗試登入。\n", wait)<0){ close(fd); return NULL; }
            continue;
        }
        char expect[MAX_PASS];
        snprintf(expect,sizeof(expect),"%s",users[idx].password);
        pthread_mutex_unlock(&users_mtx);

        // password with 5-second limit
        if(sendf(fd,"請輸入密碼(5秒內完成):\n")<0){ close(fd); return NULL; }
        n = recv_line_timeout(fd, line, sizeof(line), 5);
        if(n == -2){
            pthread_mutex_lock(&users_mtx);
            users[idx].login_cooldown_until = time(NULL) + 10;
            pthread_mutex_unlock(&users_mtx);
            if(sendf(fd,"登入逾時，請 10 秒後再試。\n")<0){ close(fd); return NULL; }
            continue;
        }
        if(n <= 0){ close(fd); return NULL; }

        if(strcmp(line, expect) != 0){
            if(sendf(fd,"Wrong Password!!!\n")<0){ close(fd); return NULL; }
            continue;
        }

        // success → menu
        if(sendf(fd,"Login OK.\n")<0){ close(fd); return NULL; }
        post_login_session(fd, idx);
        // after logout, go back to login loop
    }

    close(fd);
    return NULL;
}

/* ---------- main ---------- */
int main(void){
    for(int i=0;i<MAX_CLIENTS;i++) clients[i].fd = -1;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if(srv < 0){ perror("socket"); return 1; }
    int yes=1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(srv,(struct sockaddr*)&addr,sizeof(addr))<0){ perror("bind"); return 1; }
    if(listen(srv, 16)<0){ perror("listen"); return 1; }

    printf("Q2 Server listening on port %d ...\n", SERVER_PORT);

    while(1){
        struct sockaddr_in cli; socklen_t clilen = sizeof(cli);
        int *cfd = malloc(sizeof(int));
        if(!cfd){ perror("malloc"); break; }
        *cfd = accept(srv, (struct sockaddr*)&cli, &clilen);
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
