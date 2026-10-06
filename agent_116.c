#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define PORT    9410         /* 7000 + 2410 (IT24103116) */
#define BACKLOG 10
#define SID     "6113"       /* last 4 digits 3116 reversed */
#define TOKEN   "OPS-3116"   /* OPS- + last 4 digits */
#define BUFSZ   8192

/* One of these per connection. buf holds bytes received but not yet
   consumed, so leftover data (half a line, or the start of the next
   line) is never lost. It also lets PUT read file bytes later. */
struct conn {
    int    fd;
    char   buf[BUFSZ];
    size_t len;
    int    authed;
};

/* Send exactly n bytes, however many send() calls it takes. */
static int send_all(int fd, const char *data, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(fd, data + sent, n - sent, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)r;
    }
    return 0;
}

/* Every response ends with " SID:<sid>" and a newline. */
static int reply(struct conn *c, const char *msg)
{
    char out[1024];
    int n = snprintf(out, sizeof(out), "%s SID:%s\n", msg, SID);
    if (n < 0 || n >= (int)sizeof(out)) return -1;
    return send_all(c->fd, out, (size_t)n);
}

/* Read one full line (without the newline) into line.
   Returns 1 = got a line, 0 = client closed, -1 = error. */
static int read_line(struct conn *c, char *line, size_t max)
{
    while (1) {
        char *nl = memchr(c->buf, '\n', c->len);
        if (nl) {
            size_t linelen = (size_t)(nl - c->buf);
            size_t copy = linelen < max - 1 ? linelen : max - 1;
            memcpy(line, c->buf, copy);
            line[copy] = '\0';
            if (copy > 0 && line[copy - 1] == '\r')
                line[copy - 1] = '\0';
            size_t consumed = linelen + 1;
            memmove(c->buf, c->buf + consumed, c->len - consumed);
            c->len -= consumed;
            return 1;
        }
        if (c->len == BUFSZ) return -1;   /* line too long */
        ssize_t r = recv(c->fd, c->buf + c->len, BUFSZ - c->len, 0);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        c->len += (size_t)r;
    }
}

static void handle_client(int fd, const char *ip, int port)
{
    struct conn c;
    memset(&c, 0, sizeof(c));
    c.fd = fd;

    char line[1024];
    int r;
    while ((r = read_line(&c, line, sizeof(line))) == 1) {
        printf("[%s:%d] > %s\n", ip, port, line);

        if (!c.authed) {
            /* Only AUTH is accepted until it succeeds. */
            if (strncmp(line, "AUTH ", 5) == 0) {
                if (strcmp(line + 5, TOKEN) == 0) {
                    c.authed = 1;
                    reply(&c, "OK AUTHENTICATED");
                } else {
                    reply(&c, "ERR 001 AUTH_FAILED");
                }
            } else {
                reply(&c, "ERR 003 NOT_AUTHENTICATED");
            }
            continue;
        }

        if (strcmp(line, "QUIT") == 0) {
            reply(&c, "OK BYE");
            break;
        } else if (strncmp(line, "AUTH ", 5) == 0) {
            reply(&c, "OK AUTHENTICATED");
        } else {
            reply(&c, "ERR 099 UNKNOWN_COMMAND");   /* handlers come next */
        }
    }

    if (r == 0)      printf("[%s:%d] client disconnected\n", ip, port);
    else if (r < 0)  printf("[%s:%d] read error, closing\n", ip, port);
    close(fd);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);   /* a dead client must not kill the agent */

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
        int cfd = accept(srv, (struct sockaddr *)&cli, &len);
        if (cfd < 0) {
            perror("accept");
            continue;
        }
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        printf("Connection from %s:%d\n", ip, ntohs(cli.sin_port));

        handle_client(cfd, ip, ntohs(cli.sin_port));
    }
}
