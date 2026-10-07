#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT   9410   /* 7000 + 2410 (IT24103116) */
#define BUFSZ  16384

struct sock {
    int    fd;
    char   buf[BUFSZ];   /* bytes received but not yet consumed */
    size_t len;
};

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

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

static int write_all(int fd, const char *data, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, data + done, n - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}

/* One line from the agent (no newline). 1 = line, 0 = closed, -1 = error. */
static int read_line(struct sock *s, char *line, size_t max)
{
    while (1) {
        char *nl = memchr(s->buf, '\n', s->len);
        if (nl) {
            size_t linelen = (size_t)(nl - s->buf);
            size_t copy = linelen < max - 1 ? linelen : max - 1;
            memcpy(line, s->buf, copy);
            line[copy] = '\0';
            size_t consumed = linelen + 1;
            memmove(s->buf, s->buf + consumed, s->len - consumed);
            s->len -= consumed;
            return 1;
        }
        if (s->len == BUFSZ) return -1;
        ssize_t r = recv(s->fd, s->buf + s->len, BUFSZ - s->len, 0);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        s->len += (size_t)r;
    }
}

/* Receive exactly n bytes (buffered bytes first). Writes to out_fd if >= 0,
   otherwise discards them. 0 = ok, -1 = error. */
static int recv_bytes(struct sock *s, int out_fd, unsigned long long n)
{
    char tmp[BUFSZ];
    while (n > 0) {
        size_t chunk;
        if (s->len > 0) {
            chunk = s->len < n ? s->len : (size_t)n;
            if (out_fd >= 0 && write_all(out_fd, s->buf, chunk) < 0) return -1;
            memmove(s->buf, s->buf + chunk, s->len - chunk);
            s->len -= chunk;
        } else {
            size_t want = n < sizeof(tmp) ? (size_t)n : sizeof(tmp);
            ssize_t r = recv(s->fd, tmp, want, 0);
            if (r == 0) return -1;
            if (r < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            chunk = (size_t)r;
            if (out_fd >= 0 && write_all(out_fd, tmp, chunk) < 0) return -1;
        }
        n -= chunk;
    }
    return 0;
}

/* Any ordinary one-line command: send it, print the one-line reply. */
static int do_simple(struct sock *s, const char *cmd)
{
    char out[BUFSZ];
    int n = snprintf(out, sizeof(out), "%s\n", cmd);
    if (n < 0 || n >= (int)sizeof(out)) {
        printf("Command too long\n");
        return 0;
    }
    if (send_all(s->fd, out, (size_t)n) < 0) return -1;

    char line[BUFSZ];
    if (read_line(s, line, sizeof(line)) <= 0) return -1;
    puts(line);
    return 0;
}

/* PUT <localfile>: sends "PUT <name> <size>\n" then exactly <size> bytes. */
static int do_put(struct sock *s, const char *path)
{
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (*name == '\0' || strchr(name, ' ')) {
        printf("PUT: bad file name (no spaces allowed)\n");
        return 0;
    }

    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        printf("PUT: cannot read local file '%s'\n", path);
        if (fd >= 0) close(fd);
        return 0;
    }
    unsigned long long size = (unsigned long long)st.st_size;

    char cmd[512];
    int n = snprintf(cmd, sizeof(cmd), "PUT %s %llu\n", name, size);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        printf("PUT: file name too long\n");
        close(fd);
        return 0;
    }

    double t0 = now_sec();
    if (send_all(s->fd, cmd, (size_t)n) < 0) { close(fd); return -1; }

    char tmp[BUFSZ];
    unsigned long long left = size;
    while (left > 0) {
        size_t want = left < sizeof(tmp) ? (size_t)left : sizeof(tmp);
        ssize_t r = read(fd, tmp, want);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0 || send_all(s->fd, tmp, (size_t)r) < 0) {
            printf("PUT: transfer failed\n");
            close(fd);
            return -1;
        }
        left -= (unsigned long long)r;
    }
    close(fd);

    char line[BUFSZ];
    int rc = read_line(s, line, sizeof(line));
    double dt = now_sec() - t0;
    if (rc <= 0) return -1;
    puts(line);

    if (strncmp(line, "OK ", 3) == 0) {
        if (dt < 1e-6) dt = 1e-6;
        printf("Uploaded %llu bytes in %.3f s (%.0f bytes/s)\n",
               size, dt, (double)size / dt);
    }
    return 0;
}

/* GET <name>: reads "OK FILE_SEND <name> <size>", then exactly <size> bytes. */
static int do_get(struct sock *s, const char *name)
{
    if (*name == '\0' || strchr(name, '/') || strchr(name, ' ')) {
        printf("GET: give just a file name (no path, no spaces)\n");
        return 0;
    }

    char cmd[512];
    int n = snprintf(cmd, sizeof(cmd), "GET %s\n", name);
    if (n < 0 || n >= (int)sizeof(cmd)) {
        printf("GET: file name too long\n");
        return 0;
    }

    double t0 = now_sec();
    if (send_all(s->fd, cmd, (size_t)n) < 0) return -1;

    char line[BUFSZ];
    if (read_line(s, line, sizeof(line)) <= 0) return -1;
    puts(line);

    char got[256];
    unsigned long long size;
    if (sscanf(line, "OK FILE_SEND %255s %llu", got, &size) != 2)
        return 0;   /* an ERR line: no file bytes follow */

    mkdir("downloads", 0755);
    char path[512];
    snprintf(path, sizeof(path), "downloads/%s", name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("GET: cannot create local file");
        return (recv_bytes(s, -1, size) == 0) ? 0 : -1;   /* keep stream in sync */
    }

    int rc = recv_bytes(s, fd, size);
    double dt = now_sec() - t0;
    close(fd);
    if (rc < 0) {
        unlink(path);
        printf("GET: transfer failed\n");
        return -1;
    }

    if (dt < 1e-6) dt = 1e-6;
    printf("Saved %s (%llu bytes) in %.3f s (%.0f bytes/s)\n",
           path, size, dt, (double)size / dt);
    return 0;
}

int main(int argc, char *argv[])
{
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct sock s;
    s.len = 0;
    s.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s.fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(PORT);
    if (inet_pton(AF_INET, host, &srv.sin_addr) != 1) {
        fprintf(stderr, "Bad address: %s\n", host);
        return 1;
    }
    if (connect(s.fd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("connect");
        return 1;
    }
    printf("Connected to %s:%d\n", host, PORT);
    printf("Commands: AUTH, SYSINFO, LISTPROC, EXEC, PUT <localfile>, GET <name>, QUIT\n");

    char input[1024];
    while (fgets(input, sizeof(input), stdin)) {
        input[strcspn(input, "\r\n")] = '\0';
        if (input[0] == '\0') continue;

        int rc;
        if (strncmp(input, "PUT ", 4) == 0)
            rc = do_put(&s, input + 4);
        else if (strncmp(input, "GET ", 4) == 0)
            rc = do_get(&s, input + 4);
        else
            rc = do_simple(&s, input);

        if (rc < 0) {
            printf("Connection to agent lost\n");
            break;
        }
        if (strcmp(input, "QUIT") == 0) break;
    }

    close(s.fd);
    return 0;
}
