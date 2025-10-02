#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>

int main() {
    struct sockaddr_in server, client;
    int sock, addressSize;
    int num1, num2, ans;

    bzero(&server, sizeof(server));

    server.sin_family = PF_INET;
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_port = htons(5678);

    sock = socket(PF_INET, SOCK_DGRAM, 0);
    bind(sock, (struct sockaddr*)&server, sizeof(server));

    addressSize = sizeof(client);

    // Receive number1
    recvfrom(sock, &num1, sizeof(num1), 0, (struct sockaddr*)&client, &addressSize);
    printf("Read Message [1]: %d\n", num1);

    // Receive number2
    recvfrom(sock, &num2, sizeof(num2), 0, (struct sockaddr*)&client, &addressSize);
    printf("Read Message [2]: %d\n", num2);

    // Add two values
    ans = num1 + num2;
    printf("Answer: %d\n", ans);

    // Return the calculated value
    sendto(sock, &ans, sizeof(ans), 0, (struct sockaddr*)&client, sizeof(client));

    close(sock);
    return 0;
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>

int main() {
    struct sockaddr_in server, client;
    struct timeval tv;
    fd_set rfds;
    int sock, addressSize;
    int num1, num2, ans;
    int retval;

    sock = socket(PF_INET, SOCK_DGRAM, 0);
    bzero(&server, sizeof(server));
    server.sin_family = PF_INET;
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_port = htons(5678);

    bind(sock, (struct sockaddr*)&server, sizeof(server));
    addressSize = sizeof(client);

    while (1) {
        printf("Waiting for first number...\n");

        // Receive first number
        recvfrom(sock, &num1, sizeof(num1), 0, (struct sockaddr*)&client, &addressSize);
        printf("Received num1: %d\n", num1);

        // Setup select for 3-second wait
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        tv.tv_sec = 3;
        tv.tv_usec = 0;

        retval = select(sock + 1, &rfds, NULL, NULL, &tv);

        if (retval > 0) {
            // Second number received in time
            recvfrom(sock, &num2, sizeof(num2), 0, (struct sockaddr*)&client, &addressSize);
            printf("Received num2: %d\n", num2);
            ans = num1 * num2;
        } else {
            // No second number, multiply by 100
            printf("No second number received within 3 seconds.\n");
            ans = num1 * 100;
        }

        // Send result back to client
        sendto(sock, &ans, sizeof(ans), 0, (struct sockaddr*)&client, addressSize);
        printf("Sent answer: %d\n\n", ans);
    }

    close(sock);
    return 0;
}
