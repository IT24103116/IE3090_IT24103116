#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define PORT 9410
#define BACKLOG 10

int main(void)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(srv, BACKLOG) < 0) {
        perror("listen");
        return 1;
    }

    printf("RemoteOps Agent listening on port %d\n", PORT);

    while (1) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int c = accept(srv, (struct sockaddr *)&cli, &len);
        if (c < 0) {
            perror("accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        printf("Connection from %s:%d\n", ip, ntohs(cli.sin_port));

        close(c);
    }
}
