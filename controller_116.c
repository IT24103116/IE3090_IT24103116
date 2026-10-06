#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

#define PORT 9410   /* 7000 + 2410 (IT24103116) */

int main(int argc, char *argv[])
{
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(PORT);
    if (inet_pton(AF_INET, host, &srv.sin_addr) != 1) {
        fprintf(stderr, "Bad address: %s\n", host);
        return 1;
    }

    if (connect(s, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("connect");
        return 1;
    }
    printf("Connected to %s:%d\n", host, PORT);

    char buf[4096];
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(s, &rfds);

        if (select(s + 1, &rfds, NULL, NULL, NULL) < 0) {
            perror("select");
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
            if (send(s, buf, n, 0) < 0) { perror("send"); break; }
        }

        if (FD_ISSET(s, &rfds)) {
            ssize_t n = recv(s, buf, sizeof(buf) - 1, 0);
            if (n <= 0) {
                printf("Agent closed the connection\n");
                break;
            }
            buf[n] = '\0';
            fputs(buf, stdout);
        }
    }

    close(s);
    return 0;
}


