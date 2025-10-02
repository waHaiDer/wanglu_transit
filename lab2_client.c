#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>

int main() {
    fd_set rfds; // Declare a set of file descriptors as rfds
    struct sockaddr_in server;
    struct timeval tv; // Create time variable
    int sock, readSize, addressSize;
    int retval;
    int num1, num2, ans;

    bzero(&server, sizeof(server));

    server.sin_family = PF_INET;
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_port = htons(5678);

    sock = socket(PF_INET, SOCK_DGRAM, 0);
    addressSize = sizeof(server);

    FD_ZERO(&rfds);      // Set all bits in the rfds set to 0
    FD_SET(0, &rfds);    // Add fd = 0 (stdin) to the rfds set

    tv.tv_sec = 5;       // Set the number of seconds
    tv.tv_usec = 0;      // Set the number of microseconds

    // Read number1
    scanf("%d", &num1);
    sendto(sock, &num1, sizeof(num1), 0, (struct sockaddr*)&server, sizeof(server));

    retval = select(1, &rfds, NULL, NULL, &tv); // retval takes result of select()

    if (retval == -1) {
        perror("select error!");
    } else if (retval) {
        // If input is detected within 5 seconds
        scanf("%d", &num2);
        sendto(sock, &num2, sizeof(num2), 0, (struct sockaddr*)&server, sizeof(server));
    } else {
        // No input within timeout
        num2 = 100;
        sendto(sock, &num2, sizeof(num2), 0, (struct sockaddr*)&server, sizeof(server));
    }

    // Receive the returned result
    readSize = recvfrom(sock, &ans, sizeof(ans), 0, (struct sockaddr*)&server, &addressSize);
    printf("Read Message: %d\n", ans);

    close(sock);
    return 0;
}
