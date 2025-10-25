#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINE 1024

static int sockfd;

/* ---------- helpers ---------- */
static int send_line(const char *s){
    char out[MAX_LINE+4];
    snprintf(out,sizeof(out),"%s\n",s);
    size_t len=strlen(out),sent=0;
    while(sent<len){
        ssize_t n=send(sockfd,out+sent,len-sent,0);
        if(n<0){ if(errno==EINTR) continue; return -1; }
        sent+=(size_t)n;
    }
    return 0;
}
static void read_until_END(void){
    char buf[2048];
    char acc[8192]; size_t acc_len=0;
    while(1){
        ssize_t n = recv(sockfd, buf, sizeof(buf)-1, 0);
        if(n<=0){ printf("\n[Server closed]\n"); exit(0); }
        buf[n]='\0';
        // accumulate and check for "END\n"
        if(acc_len + (size_t)n >= sizeof(acc)) { fwrite(acc,1,acc_len,stdout); acc_len=0; }
        memcpy(acc+acc_len, buf, (size_t)n);
        acc_len += (size_t)n;
        acc[acc_len] = '\0';
        if(strstr(acc, "\nEND\n")){
            // print everything before END
            char *p = strstr(acc, "\nEND\n");
            *p = '\0';
            fputs(acc, stdout);
            fputc('\n', stdout);
            fflush(stdout);
            break;
        }
    }
}

/* timed integer input: prompt + timeout_sec; on timeout returns fallback_val and *timed_out=1 */
static long long timed_read_integer(const char *prompt, int timeout_sec, long long fallback_val, int *timed_out){
    printf("%s", prompt); fflush(stdout);
    fd_set rfds; FD_ZERO(&rfds); FD_SET(STDIN_FILENO,&rfds);
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    int rv = select(STDIN_FILENO+1, &rfds, NULL, NULL, &tv);
    if(rv == 0){
        if(timed_out) *timed_out = 1;
        printf("[timeout → %lld]\n", fallback_val);
        return fallback_val;
    }
    if(rv < 0){ perror("select"); if(timed_out) *timed_out=1; return fallback_val; }

    char line[256];
    if(!fgets(line, sizeof(line), stdin)){ if(timed_out) *timed_out=1; return fallback_val; }
    if(timed_out) *timed_out = 0;
    return atoll(line);
}

/* ---------- functions ---------- */
static void do_A(void){
    // two numbers, 5s each; timeout → -1
    int to;
    long long A = timed_read_integer("Enter A (5s): ", 5, -1, &to);
    long long B = timed_read_integer("Enter B (5s): ", 5, -1, &to);

    // send request
    send_line("A");
    char tmp[64];
    snprintf(tmp,sizeof(tmp),"%lld",A); send_line(tmp);
    snprintf(tmp,sizeof(tmp),"%lld",B); send_line(tmp);

    // show result
    read_until_END();
}

static void do_B(void){
    // multiple numbers, min 4, max 10; each 3s; timeout → push 0 and END input
    long long vals[10]; int n=0;
    for(int i=0;i<10;i++){
        char prompt[64];
        snprintf(prompt,sizeof(prompt),"Enter number #%d (3s; timeout ends with 0): ", i+1);
        int timed_out=0;
        long long v = timed_read_integer(prompt, 3, 0, &timed_out);
        vals[n++] = v;
        if(timed_out) break;              // timeout ends input immediately (0 was inserted)
        if(n>=10) break;                  // max 10
        // user may press Enter quickly; continue until they choose to stop via timeout
        // but ensure at least 4 entries before allowing user to stop via manual quit
        if(n>=4){
            printf("Press Enter promptly for more, or wait 3s to finish...\n");
        }
    }
    if(n<4){ printf("[Note] Fewer than 4 values captured due to early timeout.\n"); }

    // send request
    send_line("B");
    char tmp[64];
    snprintf(tmp,sizeof(tmp),"%d", n); send_line(tmp);
    for(int i=0;i<n;i++){ snprintf(tmp,sizeof(tmp),"%lld",vals[i]); send_line(tmp); }

    read_until_END();
}

static void do_C(void){
    // type letters for up to 10 seconds; we will compress to a single line
    printf("Type letters for up to 10 seconds; press Enter as you like.\n");

    time_t start = time(NULL);
    char buf[4096]; size_t L=0;

    while(1){
        time_t now = time(NULL);
        int remain = (int)(10 - (now - start));
        if(remain <= 0) break;

        fd_set rfds; FD_ZERO(&rfds); FD_SET(STDIN_FILENO,&rfds);
        struct timeval tv = { .tv_sec = remain, .tv_usec = 0 };
        int rv = select(STDIN_FILENO+1, &rfds, NULL, NULL, &tv);
        if(rv == 0) break;          // time’s up
        if(rv < 0){ perror("select"); break; }

        char line[256];
        if(!fgets(line,sizeof(line),stdin)) break;
        size_t add = strlen(line);
        if(L + add >= sizeof(buf)) add = sizeof(buf)-1 - L;
        memcpy(buf+L, line, add);
        L += add; buf[L] = '\0';
    }

    // Normalize to ONE LINE: keep only a–z/A–Z; map newlines/tabs/spaces out.
    char oneline[4096]; size_t k=0;
    for(size_t i=0; i<L && k+1<sizeof(oneline); ++i){
        unsigned char c = (unsigned char)buf[i];
        if( (c>='A' && c<='Z') || (c>='a' && c<='z') ){
            oneline[k++] = (char)c;
        }
        // ignore everything else (spaces, newlines, digits, punctuation)
    }
    oneline[k] = '\0';

    // Send request: code + single-line payload
    if (send_line("C") < 0) { perror("send"); return; }
    if (send_line(oneline) < 0) { perror("send"); return; }

    // Get server response (letter frequency or "No letters found.")
    read_until_END();
}

/* ---------- main ---------- */
int main(int argc, char **argv){
    const char *ip = "127.0.0.1";
    int port = 5680;
    if(argc>=2) ip   = argv[1];
    if(argc>=3) port = atoi(argv[2]);
    const char *env_ip = getenv("CHAT_IP");
    const char *env_pt = getenv("CHAT_PORT");
    if(env_ip&&*env_ip) ip = env_ip;
    if(env_pt&&*env_pt) port = atoi(env_pt);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd<0){ perror("socket"); return 1; }
    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET; addr.sin_port=htons((uint16_t)port);
    if(inet_pton(AF_INET, ip, &addr.sin_addr) != 1){ fprintf(stderr,"Invalid IP\n"); return 1; }
    if(connect(sockfd,(struct sockaddr*)&addr,sizeof(addr))<0){ perror("connect"); return 1; }

    while(1){
        printf(
            "\n=== MAIN MENU (Q3) ===\n"
            "A) Two numbers (5s each; -1 on timeout)\n"
            "B) Multiple numbers (min 4, max 10; 3s each; 0 on timeout ends)\n"
            "C) Letters for 10s (server returns frequency)\n"
            "Q) Quit\n"
            "Select: ");
        fflush(stdout);

        char choice[16];
        if(!fgets(choice,sizeof(choice),stdin)) break;
        // trim
        size_t L=strlen(choice);
        while(L && (choice[L-1]=='\n'||choice[L-1]=='\r')) choice[--L]='\0';

        if(!strcasecmp(choice,"A")){
            do_A();
        }else if(!strcasecmp(choice,"B")){
            do_B();
        }else if(!strcasecmp(choice,"C")){
            do_C();
        }else if(!strcasecmp(choice,"Q")){
            send_line("Q");
            read_until_END();
            break;
        }else{
            printf("Invalid choice.\n");
        }
    }

    close(sockfd);
    return 0;
}
