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

/* ---------- menus ---------- */
static void show_main_menu(int fd){
    sendf(fd,
        "\n=== MAIN MENU ===\n"
        "1) Login\n"
        "2) Register new account\n"
        "3) Exit\n"
        "Select (1-3):\n");
}
static void show_post_login_menu(int fd){
    sendf(fd,
        "\n=== MENU ===\n"
        "1) Register new account\n"
        "2) Change password\n"
        "3) Logout\n"
        "Select (1-3):\n");
}

/* ---------- actions ---------- */
static void handle_register(int fd){
    char line[MAX_LINE];
    if(sendf(fd,"=== Register New Account ===\nEnter Student ID:\n")<0) return;
    if(recv_line_blocking(fd,line,sizeof(line))<=0) return;
    char sid[MAX_STUID]; snprintf(sid,sizeof(sid),"%s",line);

    if(sendf(fd,"Enter new username:\n")<0) return;
    if(recv_line_blocking(fd,line,sizeof(line))<=0) return;
    char uname[MAX_NAME]; snprintf(uname,sizeof(uname),"%s",line);

    char pw[MAX_PASS];
    while(1){
        if(sendf(fd,"Enter new password (6-15 chars):\n")<0) return;
        if(recv_line_blocking(fd,line,sizeof(line))<=0) return;
        size_t L=strlen(line);
        if(L>=6 && L<=15){ snprintf(pw,sizeof(pw),"%s",line); break; }
        if(sendf(fd,"Password length invalid. Please try again.\n")<0) return;
    }

    pthread_mutex_lock(&users_mtx);
    if(user_count_locked() >= MAX_USERS){
        pthread_mutex_unlock(&users_mtx);
        sendf(fd,"User database is full (max 3 accounts).\n");
        return;
    }
    int rc = add_user_locked(sid, uname, pw);
    pthread_mutex_unlock(&users_mtx);

    if(rc == -2)      sendf(fd,"Username already exists. Registration rejected.\n");
    else if(rc == -3) sendf(fd,"Student ID already registered. Registration rejected.\n");
    else if(rc < 0)   sendf(fd,"Registration failed.\n");
    else              sendf(fd,"Registration OK.\n");
}

static void handle_change_password(int fd, int user_idx){
    char line[MAX_LINE];
    while(1){
        if(sendf(fd,"=== Change Password ===\nEnter new password (12-20 chars; must include upper & lower):\n")<0) return;
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
        sendf(fd,"Password changed.\n");
        break;
    }
}

static int do_login(int fd){
    // returns user index on success, -1 otherwise (loops until success or user backs out by reconnecting)
    char line[MAX_LINE];

    // Username
    if(sendf(fd,"=== Login ===\nUsername:\n")<0) return -1;
    ssize_t n = recv_line_blocking(fd, line, sizeof(line));
    if(n <= 0) return -1;
    char in_user[MAX_NAME]; snprintf(in_user,sizeof(in_user),"%s",line);

    // Lookup & cooldown
    pthread_mutex_lock(&users_mtx);
    int idx = find_user_by_username_locked(in_user);
    if(idx < 0){
        pthread_mutex_unlock(&users_mtx);
        sendf(fd,"Wrong ID!!!\n");
        return -1;
    }
    time_t now = time(NULL);
    if(users[idx].login_cooldown_until > now){
        int wait = (int)(users[idx].login_cooldown_until - now);
        pthread_mutex_unlock(&users_mtx);
        sendf(fd,"Please wait %d seconds before login.\n", wait);
        return -1;
    }
    char expect[MAX_PASS];
    snprintf(expect,sizeof(expect),"%s",users[idx].password);
    pthread_mutex_unlock(&users_mtx);

    // Password (5-second limit)
    if(sendf(fd,"Password (you have 5 seconds):\n")<0) return -1;
    n = recv_line_timeout(fd, line, sizeof(line), 5);
    if(n == -2){
        pthread_mutex_lock(&users_mtx);
        users[idx].login_cooldown_until = time(NULL) + 10;
        pthread_mutex_unlock(&users_mtx);
        sendf(fd,"Login timeout. Please wait 10 seconds before trying again.\n");
        return -1;
    }
    if(n <= 0) return -1;

    if(strcmp(line, expect) != 0){
        sendf(fd,"Wrong Password!!!\n");
        return -1;
    }

    sendf(fd,"Login OK.\n");
    return idx;
}

/* ---------- sessions ---------- */
static void post_login_session(int fd, int user_idx){
    char line[MAX_LINE];
    while(1){
        show_post_login_menu(fd);
        ssize_t n = recv_line_blocking(fd, line, sizeof(line));
        if(n <= 0) return;
        if(strcmp(line,"1")==0){
            handle_register(fd);
        }else if(strcmp(line,"2")==0){
            handle_change_password(fd, user_idx);
        }else if(strcmp(line,"3")==0){
            sendf(fd,"Logged out.\n");
            return;
        }else{
            sendf(fd,"Invalid choice.\n");
        }
    }
}

static void main_menu_session(int fd){
    char line[MAX_LINE];
    while(1){
        show_main_menu(fd);
        ssize_t n = recv_line_blocking(fd, line, sizeof(line));
        if(n <= 0) return; // disconnect
        if(strcmp(line,"1")==0){
            int idx = do_login(fd);
            if(idx >= 0){
                post_login_session(fd, idx);  // after logout, return to main menu
            }
        }else if(strcmp(line,"2")==0){
            handle_register(fd);
        }else if(strcmp(line,"3")==0){
            sendf(fd,"Goodbye.\n");
            return;
        }else{
            sendf(fd,"Invalid choice.\n");
        }
    }
}

/* ---------- per-connection ---------- */
static void *client_thread(void *arg){
    int fd = *(int*)arg; free(arg);
    main_menu_session(fd); // start at main menu (English)
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
