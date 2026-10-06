#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define PORT     9410                    /* 7000 + 2410 (IT24103116) */
#define BACKLOG  10
#define SID      "6113"                  /* last 4 digits 3116 reversed */
#define TOKEN    "OPS-3116"              /* OPS- + last 4 digits */
#define LOGFILE  "remoteops_IT24103116.log"
#define BUFSZ    8192

/* ---------- logging (thread-safe) ---------- */
static FILE *logfp = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void log_event(const char *fmt, ...)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    pthread_mutex_lock(&log_mutex);
    if (logfp) {
        va_list ap;
        fprintf(logfp, "%s ", ts);
        va_start(ap, fmt);
        vfprintf(logfp, fmt, ap);
        va_end(ap);
        fprintf(logfp, "\n");
        fflush(logfp);
    }
    pthread_mutex_unlock(&log_mutex);
}

struct conn {
    int    fd;
    char   buf[BUFSZ];
    size_t len;
    int    authed;
};

struct client_arg {
    int  fd;
    char ip[INET_ADDRSTRLEN];
    int  port;
};

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
    char out[8192];
    int n = snprintf(out, sizeof(out), "%s SID:%s\n", msg, SID);
    if (n < 0 || n >= (int)sizeof(out)) return -1;
    return send_all(c->fd, out, (size_t)n);
}

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
        if (c->len == BUFSZ) return -1;
        ssize_t r = recv(c->fd, c->buf + c->len, BUFSZ - c->len, 0);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        c->len += (size_t)r;
    }
}

/* ---------- SYSINFO ---------- */
static void get_sysinfo(char *out, size_t n)
{
    double load = 0.0, up = 0.0;
    long mem_total = 0, mem_avail = 0;
    FILE *f;

    f = fopen("/proc/loadavg", "r");
    if (f) {
        if (fscanf(f, "%lf", &load) != 1) load = 0.0;
        fclose(f);
    }

    f = fopen("/proc/meminfo", "r");
    if (f) {
        char key[64];
        long val;
        while (fscanf(f, "%63s %ld", key, &val) == 2) {
            if (strcmp(key, "MemTotal:") == 0) mem_total = val;
            else if (strcmp(key, "MemAvailable:") == 0) mem_avail = val;
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) { }
        }
        fclose(f);
    }

    f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &up) != 1) up = 0.0;
        fclose(f);
    }

    long used_mb = (mem_total - mem_avail) / 1024;
    snprintf(out, n, "SYSINFO %.2f %ld %ld", load, used_mb, (long)up);
}

/* ---------- LISTPROC ---------- */
static void list_procs(char *out, size_t n)
{
    out[0] = '\0';
    FILE *p = popen("ps -eo pid=,comm=", "r");
    if (!p) {
        snprintf(out, n, "unavailable");
        return;
    }
    char line[256];
    size_t used = 0;
    int first = 1;
    while (fgets(line, sizeof(line), p)) {
        int pid;
        char name[128];
        if (sscanf(line, "%d %127s", &pid, name) != 2) continue;
        char item[160];
        int w = snprintf(item, sizeof(item), "%s%d:%s", first ? "" : ",", pid, name);
        if (w < 0 || used + (size_t)w + 1 >= n) break;
        memcpy(out + used, item, (size_t)w);
        used += (size_t)w;
        out[used] = '\0';
        first = 0;
    }
    pclose(p);
}

/* Run a FIXED command string (never user input), flatten to one line. */
static void run_cmd(const char *cmd, char *out, size_t n)
{
    out[0] = '\0';
    FILE *p = popen(cmd, "r");
    if (!p) {
        snprintf(out, n, "error");
        return;
    }
    size_t used = 0;
    int ch;
    while ((ch = fgetc(p)) != EOF && used + 1 < n) {
        out[used++] = (ch == '\n' || ch == '\r') ? ' ' : (char)ch;
    }
    out[used] = '\0';
    pclose(p);
    while (used > 0 && out[used - 1] == ' ')
        out[--used] = '\0';
}

/* ---------- EXEC: fixed whitelist, exact match only ---------- */
static void handle_exec(struct conn *c, const char *name)
{
    static const struct {
        const char *name;
        const char *cmd;
    } table[] = {
        { "DATE",     "date"     },
        { "UPTIME",   "uptime"   },
        { "DISKFREE", "df -h /"  },
        { "HOSTNAME", "hostname" },
        { "WHOAMI",   "whoami"   },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(name, table[i].name) == 0) {
            char out[2048], msg[2100];
            run_cmd(table[i].cmd, out, sizeof(out));
            snprintf(msg, sizeof(msg), "OK EXEC_RESULT %s", out);
            reply(c, msg);
            return;
        }
    }
    reply(c, "ERR 002 COMMAND_NOT_ALLOWED");
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
            if (strncmp(line, "AUTH ", 5) == 0) {
                if (strcmp(line + 5, TOKEN) == 0) {
                    c.authed = 1;
                    log_event("[%s:%d] AUTH success", ip, port);
                    reply(&c, "OK AUTHENTICATED");
                } else {
                    log_event("[%s:%d] AUTH failed", ip, port);
                    reply(&c, "ERR 001 AUTH_FAILED");
                }
            } else {
                log_event("[%s:%d] rejected before AUTH: %s", ip, port, line);
                reply(&c, "ERR 003 NOT_AUTHENTICATED");
            }
            continue;
        }

        log_event("[%s:%d] CMD %s", ip, port, line);

        if (strcmp(line, "QUIT") == 0) {
            reply(&c, "OK BYE");
            break;
        } else if (strncmp(line, "AUTH ", 5) == 0) {
            reply(&c, "OK AUTHENTICATED");
        } else if (strcmp(line, "SYSINFO") == 0) {
            char info[256], msg[300];
            get_sysinfo(info, sizeof(info));
            snprintf(msg, sizeof(msg), "OK %s", info);
            reply(&c, msg);
        } else if (strcmp(line, "LISTPROC") == 0) {
            char procs[6000], msg[6100];
            list_procs(procs, sizeof(procs));
            snprintf(msg, sizeof(msg), "OK PROCS %s", procs);
            reply(&c, msg);
        } else if (strncmp(line, "EXEC ", 5) == 0) {
            handle_exec(&c, line + 5);
        } else {
            reply(&c, "ERR 099 UNKNOWN_COMMAND");
        }
    }

    if (r == 0) {
        printf("[%s:%d] client disconnected\n", ip, port);
        log_event("[%s:%d] DISCONNECT (client closed)", ip, port);
    } else if (r < 0) {
        printf("[%s:%d] read error, closing\n", ip, port);
        log_event("[%s:%d] DISCONNECT (read error)", ip, port);
    } else {
        log_event("[%s:%d] DISCONNECT (QUIT)", ip, port);
    }
    close(fd);
}

/* One thread per client connection. */
static void *client_thread(void *p)
{
    struct client_arg *a = (struct client_arg *)p;
    pthread_detach(pthread_self());     /* resources freed automatically */
    handle_client(a->fd, a->ip, a->port);
    free(a);
    return NULL;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    logfp = fopen(LOGFILE, "a");
    if (!logfp) { perror("log file"); return 1; }

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
    log_event("Agent started, listening on port %d", PORT);

    while (1) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int cfd = accept(srv, (struct sockaddr *)&cli, &len);
        if (cfd < 0) {
            perror("accept");
            continue;
        }

        struct client_arg *a = malloc(sizeof(*a));
        if (!a) { close(cfd); continue; }
        a->fd = cfd;
        inet_ntop(AF_INET, &cli.sin_addr, a->ip, sizeof(a->ip));
        a->port = ntohs(cli.sin_port);

        printf("Connection from %s:%d\n", a->ip, a->port);
        log_event("[%s:%d] CONNECT", a->ip, a->port);

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, a) != 0) {
            perror("pthread_create");
            log_event("[%s:%d] thread creation failed", a->ip, a->port);
            close(cfd);
            free(a);
        }
    }
}
