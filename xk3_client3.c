// client3.c — LAB3 Q3: Login → Hall chat → invite/join private rooms
// Build: gcc -O2 -Wall -Wextra -o client3 client3.c -lpthread
// Run:   ./client3 127.0.0.1 5678
//
// Menu:
//   1) Sign up   2) Login   3) Chat (Hall/Room)   4) Create private room   5) Leave room   q) Exit
// Hidden: typing "Exit" in chat mode sends EXIT! and disconnects.
//
// Commands sent to server (line-based): see server3.c header.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BUF_SZ 4096

static void safe_send(int fd, const char *buf, size_t len){ size_t off=0; while(off<len){ ssize_t n=send(fd,buf+off,len-off,0); if(n<0){ if(errno==EINTR) continue; perror("send"); exit(1);} if(n==0) break; off+=(size_t)n; } }
static void send_line(int fd, const char *fmt, ...){ char out[BUF_SZ]; va_list ap; va_start(ap,fmt); vsnprintf(out,sizeof(out),fmt,ap); va_end(ap); size_t L=strlen(out); if(L==0||out[L-1]!='\n'){ if(L+1<sizeof(out)){ out[L]='\n'; out[L+1]='\0'; L++; } } safe_send(fd,out,L); }
static ssize_t recv_line(int fd, char *out, size_t cap){ size_t pos=0; while(pos+1<cap){ char c; ssize_t n=recv(fd,&c,1,0); if(n==0) return 0; if(n<0){ if(errno==EINTR) continue; return -1; } out[pos++]=c; if(c=='\n') break; } out[pos]='\0'; return (ssize_t)pos; }
static void trim(char*s){ size_t L=strlen(s); while(L && (s[L-1]=='\n'||s[L-1]=='\r')){ s[L-1]='\0'; L--; } }
static bool valid_len(const char*s){ size_t L=strlen(s); return L>=8 && L<=15; }
static bool has_upper_symbol(const char*s){ bool up=false,sym=false; for(const unsigned char*p=(const unsigned char*)s; *p; ++p){ if(*p>='A'&&*p<='Z') up=true; if(!( (*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9') )) sym=true; } return up&&sym; }

typedef struct { int fd; volatile int logged_in; } rx_arg_t;

static void *rx_thread(void *arg){
    rx_arg_t *rx=(rx_arg_t*)arg; int fd=rx->fd; char line[BUF_SZ];
    while(1){
        ssize_t n=recv_line(fd,line,sizeof(line)); if(n<=0) break;
        fputs(line, stdout);
        if (!strncmp(line,"OK LOGIN",8)) rx->logged_in=1;
        if (!strncmp(line,"Server is full!",15)){ shutdown(fd,SHUT_RDWR); close(fd); exit(0); }
        // If invited, prompt user to accept/reject:
        if (!strncmp(line,"INVITE ",7)){
            // line example: INVITE <token> FROM <fd> : Accept? ...
            char token[64]; int fromfd=0;
            if (sscanf(line,"INVITE %63s FROM %d",&token,&fromfd)==2){
                printf("Accept invite from SockID %d? (YES/NO): ", fromfd); fflush(stdout);
                char ans[16]; if(!fgets(ans,sizeof(ans),stdin)) break; trim(ans);
                if(strcasecmp(ans,"YES")==0) send_line(fd,"INVITE_RESP %s YES", token);
                else send_line(fd,"INVITE_RESP %s NO", token);
            }
        }
        fflush(stdout);
    }
    exit(0);
    return NULL;
}

int main(int argc, char **argv){
    if(argc<3){ fprintf(stderr,"usage: %s <server_ip> <port>\n", argv[0]); return 1; }
    int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0){ perror("socket"); return 1; }
    struct sockaddr_in sa; memset(&sa,0,sizeof(sa)); sa.sin_family=AF_INET; sa.sin_port=htons(atoi(argv[2]));
    if(inet_pton(AF_INET,argv[1],&sa.sin_addr)<=0){ perror("inet_pton"); return 1; }
    if(connect(fd,(struct sockaddr*)&sa,sizeof(sa))<0){ perror("connect"); return 1; }

    pthread_t th; rx_arg_t rx={.fd=fd,.logged_in=0};
    if(pthread_create(&th,NULL,rx_thread,&rx)!=0){ perror("pthread_create"); return 1; }
    pthread_detach(th);

    char line[BUF_SZ];
    while(1){
        printf("\n== Q3 Menu ==\n1) Sign up\n2) Login\n3) Chat\n4) Create private room\n5) Leave room\nq) Exit\n> ");
        fflush(stdout);
        if(!fgets(line,sizeof(line),stdin)) break; trim(line);
        if(!strcmp(line,"q")){ send_line(fd,"EXIT!"); break; }

        if(!strcmp(line,"1")){
            char sid[128],acc[128],pwd[128];
            printf("Student ID: "); fflush(stdout); if(!fgets(sid,sizeof(sid),stdin)) break; trim(sid);
            while(1){ printf("Account (8-15): "); fflush(stdout); if(!fgets(acc,sizeof(acc),stdin)) goto done; trim(acc); if(valid_len(acc)) break; printf("invalid account length!\n"); }
            while(1){ printf("Password (8-15, include UPPERCASE + symbol): "); fflush(stdout); if(!fgets(pwd,sizeof(pwd),stdin)) goto done; trim(pwd); if(valid_len(pwd)&&has_upper_symbol(pwd)) break; printf("invalid password policy!\n"); }
            send_line(fd,"SIGNUP"); send_line(fd,"SID:%s",sid); send_line(fd,"ACC:%s",acc); send_line(fd,"PWD:%s",pwd);
        }
        else if(!strcmp(line,"2")){
            char acc[128],pwd[128];
            printf("Account: "); fflush(stdout); if(!fgets(acc,sizeof(acc),stdin)) break; trim(acc);
            printf("Password: "); fflush(stdout); if(!fgets(pwd,sizeof(pwd),stdin)) break; trim(pwd);
            send_line(fd,"LOGIN"); send_line(fd,"ACC:%s",acc); send_line(fd,"PWD:%s",pwd);
        }
        else if(!strcmp(line,"3")){
            if(!rx.logged_in){ printf("Please login first.\n"); continue; }
            printf("Enter message (or 'Exit' to disconnect, '/menu' to back): ");
            fflush(stdout);
            if(!fgets(line,sizeof(line),stdin)) break; trim(line);
            if(!strcmp(line,"/menu")) continue;
            if(!strcmp(line,"Exit")){ send_line(fd,"EXIT!"); break; }
            send_line(fd,"CHAT"); send_line(fd,"%s", line[0]?line:"(empty)");
        }
        else if(!strcmp(line,"4")){
            if(!rx.logged_in){ printf("Please login first.\n"); continue; }
            printf("How many invitees? (1 or 2): "); fflush(stdout);
            if(!fgets(line,sizeof(line),stdin)) break; trim(line);
            int k=atoi(line); if(k<1||k>2){ printf("invalid number\n"); continue; }
            int s1=-1,s2=-1;
            printf("Enter SockID #1: "); fflush(stdout); if(!fgets(line,sizeof(line),stdin)) break; trim(line); s1=atoi(line);
            if(k==2){ printf("Enter SockID #2: "); fflush(stdout); if(!fgets(line,sizeof(line),stdin)) break; trim(line); s2=atoi(line); }
            if(k==1) send_line(fd,"CREATEPRV %d %d", k, s1);
            else     send_line(fd,"CREATEPRV %d %d %d", k, s1, s2);
        }
        else if(!strcmp(line,"5")){
            send_line(fd,"LEAVE");
        }
        else{
            printf("unknown option\n");
        }
    }

done:
    shutdown(fd,SHUT_RDWR); close(fd); return 0;
}
