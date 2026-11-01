// The terminal holder (docs/panes.md): one small process per world, outliving it. It owns the PTYs,
// keeps a libghostty-vt terminal for each and a ring of raw output nobody has acknowledged, and serves
// one unix socket in the data dir. The world is its only client and stays the only thing clients
// talk to. Restarting the world loses nothing: it reconnects, is told what is running, and asks for
// the output after what it has already put on the record.
//
// One thread, one poll loop, no locks. A frame either way is [u32 length][u8 op][u8 id length][id]
// [payload], big-endian, length counting everything after itself.
#include "vt.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

#define VERSION 1
#define RING (4u * 1024 * 1024)        // raw output kept until the world says it is on the record
#define READ_CHUNK (64u * 1024)        // read at most this much from one PTY in a pass
#define OUTPUT_CHUNK (32u * 1024)      // an OUTPUT frame
#define CLIENT_HIGH (1024u * 1024)     // stop filling the socket's queue past this
#define MAX_FRAME (16u * 1024 * 1024)
#define SCROLLBACK_LINES 5000
#define FRAME_MS 16
#define DRAIN_MS 500                   // a child's own children can hold the pty open past its exit
#define KILL_MS 300

enum { OP_HELLO = 1, OP_OPEN, OP_WRITE, OP_RESIZE, OP_KILL, OP_ACK, OP_ATTACH, OP_FORGET, OP_TEXT };
enum { EV_HELLO = 101, EV_PANE, EV_END, EV_OPENED, EV_ERROR, EV_TEXT, EV_OUTPUT = 110, EV_SCREEN, EV_EXIT };

typedef struct pane {
    struct pane *next;
    char *id;
    int master;       // -1 once closed
    pid_t pid;        // 0 once reaped
    vt_t *vt;
    uint8_t *ring;
    uint64_t total;   // bytes read from the program, ever
    uint64_t acked;   // the world has these on its record
    uint64_t sent;    // the world has been sent these
    buf_t in;         // bytes on their way to the program
    size_t in_at;
    bool attached;    // the world asked for this pane
    bool dirty, full;
    bool exited, exit_sent, killing;
    int code;
    uint16_t cols, rows;
    uint64_t last_screen_ms, drain_until_ms, kill_at_ms;
} pane_t;

static pane_t *panes;
static int listen_fd = -1, client = -1;
static int sig_pipe[2];
static buf_t out_q, in_q;
static size_t out_at;
static volatile sig_atomic_t stopping;
static const char *sock_path;

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void cloexec_nonblock(int fd, bool nonblock) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (nonblock) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static void on_signal(int s) {
    if (s == SIGTERM || s == SIGINT) stopping = 1;
    const int saved = errno;
    char b = 1;
    (void)!write(sig_pipe[1], &b, 1);
    errno = saved;
}

// ---------------------------------------------------------------- frames to the world

static void begin(buf_t *b, uint8_t op, const char *id) {
    const size_t n = id ? strlen(id) : 0;
    buf_u32(b, 0);
    buf_u8(b, op);
    buf_u8(b, (uint8_t)n);
    if (n) buf_bytes(b, id, n);
}

static void finish(buf_t *b, size_t at) {
    const uint32_t n = (uint32_t)(b->len - at - 4);
    b->p[at] = (uint8_t)(n >> 24);
    b->p[at + 1] = (uint8_t)(n >> 16);
    b->p[at + 2] = (uint8_t)(n >> 8);
    b->p[at + 3] = (uint8_t)n;
}

static size_t queued(void) { return out_q.len - out_at; }

static void flush_client(void) {
    while (client >= 0 && queued() > 0) {
        const ssize_t n = write(client, out_q.p + out_at, queued());
        if (n > 0) {
            out_at += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            close(client);
            client = -1;
            for (pane_t *p = panes; p; p = p->next) p->attached = false;
            break;
        }
    }
    if (out_at == out_q.len) out_at = out_q.len = 0;
}

static void send_text(uint8_t op, const char *id, const char *text) {
    const size_t at = out_q.len;
    begin(&out_q, op, id);
    buf_bytes(&out_q, text, strlen(text));
    finish(&out_q, at);
}

// ---------------------------------------------------------------- panes

static pane_t *find(const char *id) {
    for (pane_t *p = panes; p; p = p->next) {
        if (strcmp(p->id, id) == 0) return p;
    }
    return NULL;
}

static void pty_write_later(pane_t *p, const uint8_t *bytes, size_t len) {
    if (p->master >= 0) buf_bytes(&p->in, bytes, len);
}

// A terminal's own replies (cursor position, device attributes) go back to its program.
static void on_reply(void *user, const uint8_t *bytes, size_t len) { pty_write_later(user, bytes, len); }

static void close_master(pane_t *p) {
    if (p->master >= 0) close(p->master);
    p->master = -1;
    p->in.len = 0;
    p->in_at = 0;
}

static void finish_exit(pane_t *p) {
    if (!p->exited) {
        p->exited = true;
        close_master(p);
    }
}

static void pane_free(pane_t *p) {
    if (p->pid > 0) kill(-p->pid, SIGKILL);
    close_master(p);
    vt_free(p->vt);
    free(p->ring);
    free(p->in.p);
    free(p->id);
    free(p);
}

static void forget(const char *id) {
    for (pane_t **at = &panes; *at; at = &(*at)->next) {
        if (strcmp((*at)->id, id) == 0) {
            pane_t *p = *at;
            *at = p->next;
            pane_free(p);
            return;
        }
    }
}

static const char **strings(const uint8_t **cur, const uint8_t *end, uint16_t *count) {
    if (*cur + 2 > end) return NULL;
    const uint16_t n = (uint16_t)((*cur)[0] << 8 | (*cur)[1]);
    *cur += 2;
    const char **v = calloc((size_t)n + 1, sizeof *v);
    if (!v) return NULL;
    for (uint16_t i = 0; i < n; i++) {
        if (*cur + 2 > end) goto bad;
        const size_t len = (size_t)((*cur)[0] << 8 | (*cur)[1]);
        *cur += 2;
        if (*cur + len > end) goto bad;
        char *s = malloc(len + 1);
        if (!s) goto bad;
        memcpy(s, *cur, len);
        s[len] = 0;
        v[i] = s;
        *cur += len;
    }
    *count = n;
    return v;
bad:
    for (uint16_t i = 0; i < n; i++) free((void *)v[i]);
    free(v);
    return NULL;
}

static void free_strings(const char **v, uint16_t n) {
    if (!v) return;
    for (uint16_t i = 0; i < n; i++) free((void *)v[i]);
    free(v);
}

extern char **environ;

// payload: u16 cols, u16 rows, strings [dir], strings argv, strings env. The world builds argv, so a
// box around the program is exactly the box pty.clj starts.
static void open_pane(const char *id, const uint8_t *cur, const uint8_t *end) {
    if (find(id) || end - cur < 4) {
        send_text(EV_ERROR, id, "cannot open");
        return;
    }
    const uint16_t cols = (uint16_t)(cur[0] << 8 | cur[1]), rows = (uint16_t)(cur[2] << 8 | cur[3]);
    cur += 4;
    uint16_t ndir = 0, nargv = 0, nenv = 0;
    const char **dir = strings(&cur, end, &ndir);
    const char **argv = dir ? strings(&cur, end, &nargv) : NULL;
    const char **env = argv ? strings(&cur, end, &nenv) : NULL;
    if (!env || ndir != 1 || nargv == 0 || cols == 0 || rows == 0) {
        send_text(EV_ERROR, id, "bad open");
        goto done;
    }
    pane_t *p = calloc(1, sizeof *p);
    if (!p) goto done;
    p->id = strdup(id);
    p->cols = cols;
    p->rows = rows;
    p->ring = malloc(RING);
    p->vt = vt_new(cols, rows, SCROLLBACK_LINES, on_reply, p);
    p->master = -1;
    if (!p->ring || !p->vt) {
        pane_free(p);
        send_text(EV_ERROR, id, "no memory");
        goto done;
    }
    struct winsize ws = {.ws_row = rows, .ws_col = cols};
    int master = -1;
    const pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        const char *why = strerror(errno);
        pane_free(p);
        send_text(EV_ERROR, id, why);
        goto done;
    }
    if (pid == 0) {
        // the holder ignores these; what it runs must not
        signal(SIGPIPE, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (chdir(dir[0]) != 0) _exit(126);
        environ = (char **)env;
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    cloexec_nonblock(master, true);
    p->master = master;
    p->pid = pid;
    p->next = panes;
    panes = p;
    const size_t at = out_q.len;
    begin(&out_q, EV_OPENED, id);
    buf_u32(&out_q, (uint32_t)pid);
    finish(&out_q, at);
done:
    free_strings(dir, ndir);
    free_strings(argv, nargv);
    free_strings(env, nenv);
}

static uint64_t u64_at(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}

static void attach_pane(pane_t *p, uint64_t from) {
    const uint64_t oldest = p->total > RING ? p->total - RING : 0;
    p->sent = from < oldest ? oldest : (from > p->total ? p->total : from);
    if (from > p->acked) p->acked = from > p->total ? p->total : from;
    p->attached = true;
    p->dirty = true;
    p->full = true;
    p->exit_sent = false;
}

static void hello(void) {
    size_t at = out_q.len;
    begin(&out_q, EV_HELLO, NULL);
    buf_u16(&out_q, VERSION);
    finish(&out_q, at);
    for (pane_t *p = panes; p; p = p->next) {
        at = out_q.len;
        begin(&out_q, EV_PANE, p->id);
        buf_u64(&out_q, p->total);
        buf_u8(&out_q, p->exited ? 1 : 0);
        buf_u32(&out_q, (uint32_t)p->code);
        buf_u16(&out_q, p->cols);
        buf_u16(&out_q, p->rows);
        finish(&out_q, at);
    }
    at = out_q.len;
    begin(&out_q, EV_END, NULL);
    finish(&out_q, at);
}

static void handle(uint8_t op, const char *id, const uint8_t *body, size_t n) {
    pane_t *p = id[0] ? find(id) : NULL;
    switch (op) {
    case OP_HELLO: hello(); break;
    case OP_OPEN: open_pane(id, body, body + n); break;
    case OP_WRITE:
        if (p) pty_write_later(p, body, n);
        break;
    case OP_RESIZE:
        if (p && n >= 4) {
            p->cols = (uint16_t)(body[0] << 8 | body[1]);
            p->rows = (uint16_t)(body[2] << 8 | body[3]);
            vt_resize(p->vt, p->cols, p->rows);
            if (p->master >= 0) {
                struct winsize ws = {.ws_row = p->rows, .ws_col = p->cols};
                ioctl(p->master, TIOCSWINSZ, &ws);
            }
            p->dirty = true;
            p->full = true;
        }
        break;
    case OP_KILL:
        if (p && p->pid > 0) {
            kill(-p->pid, SIGHUP);
            p->killing = true;
            p->kill_at_ms = now_ms() + KILL_MS;
        }
        break;
    case OP_ACK:
        if (p && n >= 8) {
            const uint64_t v = u64_at(body);
            if (v > p->acked && v <= p->total) p->acked = v;
        }
        break;
    case OP_ATTACH:
        if (p && n >= 8) attach_pane(p, u64_at(body));
        break;
    case OP_FORGET: forget(id); break;
    case OP_TEXT: {
        const size_t at = out_q.len;
        begin(&out_q, EV_TEXT, id);
        if (p) vt_text(p->vt, &out_q);
        finish(&out_q, at);
        break;
    }
    default: break;
    }
}

static void drop_client(void) {
    close(client);
    client = -1;
    out_at = out_q.len = 0;
    in_q.len = 0;
    for (pane_t *p = panes; p; p = p->next) p->attached = false;
}

static void read_client(void) {
    uint8_t chunk[65536];
    for (;;) {
        const ssize_t n = read(client, chunk, sizeof chunk);
        if (n > 0) {
            buf_bytes(&in_q, chunk, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        drop_client();
        return;
    }
    size_t at = 0;
    while (in_q.len - at >= 4) {
        const uint8_t *f = in_q.p + at;
        const uint32_t len = (uint32_t)f[0] << 24 | (uint32_t)f[1] << 16 | (uint32_t)f[2] << 8 | f[3];
        if (len < 2 || len > MAX_FRAME) {
            drop_client();
            return;
        }
        if (in_q.len - at - 4 < len) break;
        const uint8_t idlen = f[5];
        if ((size_t)idlen + 2 > len) {
            drop_client();
            return;
        }
        char id[256];
        memcpy(id, f + 6, idlen);
        id[idlen] = 0;
        handle(f[4], id, f + 6 + idlen, len - 2 - idlen);
        at += 4 + (size_t)len;
    }
    memmove(in_q.p, in_q.p + at, in_q.len - at);
    in_q.len -= at;
}

// ---------------------------------------------------------------- PTY pumps

static bool can_read(const pane_t *p) { return p->master >= 0 && p->total - p->acked + READ_CHUNK <= RING; }

static void read_pty(pane_t *p) {
    static uint8_t chunk[READ_CHUNK];
    const ssize_t n = read(p->master, chunk, sizeof chunk);
    if (n > 0) {
        for (ssize_t i = 0; i < n; ) { // the ring wraps at most once per chunk
            const size_t off = (size_t)((p->total + (uint64_t)i) % RING);
            size_t take = RING - off;
            if (take > (size_t)(n - i)) take = (size_t)(n - i);
            memcpy(p->ring + off, chunk + i, take);
            i += (ssize_t)take;
        }
        p->total += (uint64_t)n;
        vt_write(p->vt, chunk, (size_t)n);
        p->dirty = true;
    } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        close_master(p); // the far side has gone, on Linux with EIO
        if (p->pid == 0) finish_exit(p);
    }
}

static void write_pty(pane_t *p) {
    while (p->in_at < p->in.len) {
        const ssize_t n = write(p->master, p->in.p + p->in_at, p->in.len - p->in_at);
        if (n > 0) {
            p->in_at += (size_t)n;
        } else {
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) p->in_at = p->in.len;
            break;
        }
    }
    if (p->in_at == p->in.len) p->in_at = p->in.len = 0;
}

static void reap(void) {
    int status = 0;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (pane_t *p = panes; p; p = p->next) {
            if (p->pid == pid) {
                p->pid = 0;
                p->killing = false;
                p->code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                if (p->master < 0) {
                    finish_exit(p);
                } else {
                    p->drain_until_ms = now_ms() + DRAIN_MS;
                }
                break;
            }
        }
    }
}

// What the world is owed: output in order, then the screen it produced, then the exit.
static void pump(uint64_t now) {
    for (pane_t *p = panes; p; p = p->next) {
        if (p->killing && now >= p->kill_at_ms && p->pid > 0) {
            kill(-p->pid, SIGKILL);
            p->killing = false;
        }
        if (p->pid == 0 && !p->exited && p->drain_until_ms && now >= p->drain_until_ms) finish_exit(p);
        if (client < 0 || !p->attached) continue;
        while (p->sent < p->total && queued() < CLIENT_HIGH) {
            const size_t off = (size_t)(p->sent % RING);
            size_t take = (size_t)(p->total - p->sent);
            if (take > OUTPUT_CHUNK) take = OUTPUT_CHUNK;
            if (take > RING - off) take = RING - off;
            const size_t at = out_q.len;
            begin(&out_q, EV_OUTPUT, p->id);
            buf_u64(&out_q, p->sent);
            buf_bytes(&out_q, p->ring + off, take);
            finish(&out_q, at);
            p->sent += take;
        }
        if (p->dirty && now - p->last_screen_ms >= FRAME_MS && queued() < CLIENT_HIGH) {
            const size_t at = out_q.len;
            begin(&out_q, EV_SCREEN, p->id);
            vt_screen(p->vt, p->full, &out_q);
            finish(&out_q, at);
            p->dirty = p->full = false;
            p->last_screen_ms = now;
        }
        if (p->exited && !p->exit_sent && p->sent == p->total && !p->dirty) {
            const size_t at = out_q.len;
            begin(&out_q, EV_EXIT, p->id);
            buf_u32(&out_q, (uint32_t)p->code);
            finish(&out_q, at);
            p->exit_sent = true;
        }
    }
}

static int bind_socket(const char *path) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path) {
        fprintf(stderr, "holder: socket path too long: %s\n", path);
        return -1;
    }
    strcpy(a.sun_path, path);
    const int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0 && connect(probe, (struct sockaddr *)&a, sizeof a) == 0) {
        fprintf(stderr, "holder: another holder serves %s\n", path);
        close(probe);
        return -1;
    }
    if (probe >= 0) close(probe);
    unlink(path);
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    const mode_t old = umask(077);
    const int r = bind(fd, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (r != 0 || chmod(path, 0600) != 0 || listen(fd, 4) != 0) {
        perror("holder: bind");
        close(fd);
        return -1;
    }
    cloexec_nonblock(fd, true);
    return fd;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: holder <socket>\n");
        return 2;
    }
    sock_path = argv[1];
    setsid();
    if (pipe(sig_pipe) != 0) return 1;
    cloexec_nonblock(sig_pipe[0], true);
    cloexec_nonblock(sig_pipe[1], true);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN); // a closed Terminal is no reason to end terminals
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    listen_fd = bind_socket(sock_path);
    if (listen_fd < 0) return 1;

    uint64_t stop_at = 0;
    for (;;) {
        size_t count = 0;
        for (pane_t *p = panes; p; p = p->next) count++;
        struct pollfd *fds = calloc(count + 3, sizeof *fds);
        pane_t **who = calloc(count + 3, sizeof *who);
        if (!fds || !who) abort();
        nfds_t n = 0;
        fds[n++] = (struct pollfd){.fd = sig_pipe[0], .events = POLLIN};
        fds[n++] = (struct pollfd){.fd = listen_fd, .events = POLLIN};
        if (client >= 0) {
            fds[n++] = (struct pollfd){.fd = client, .events = (short)(POLLIN | (queued() ? POLLOUT : 0))};
        }
        const nfds_t first_pane = n;
        int timeout = 1000;
        for (pane_t *p = panes; p; p = p->next) {
            if (p->dirty || p->killing || (p->pid == 0 && !p->exited)) timeout = FRAME_MS;
            if (p->attached && p->sent < p->total) timeout = FRAME_MS;
            if (p->master < 0) continue;
            short ev = can_read(p) ? POLLIN : 0;
            if (p->in_at < p->in.len) ev |= POLLOUT;
            if (!ev) continue;
            who[n] = p;
            fds[n++] = (struct pollfd){.fd = p->master, .events = ev};
        }
        const int ready = poll(fds, n, timeout);
        if (ready > 0) {
            if (fds[0].revents) {
                char b[64];
                while (read(sig_pipe[0], b, sizeof b) > 0) {
                }
                reap();
            }
            if (fds[1].revents) {
                const int c = accept(listen_fd, NULL, NULL);
                if (c >= 0) {
                    if (client >= 0) drop_client(); // the newest world is the world
                    cloexec_nonblock(c, true);
                    client = c;
                }
            }
            if (first_pane == 3 && client >= 0 && fds[2].fd == client) {
                if (fds[2].revents & (POLLIN | POLLHUP | POLLERR)) read_client();
            }
            // a frame above can free a pane: only touch those still listed
            for (nfds_t i = first_pane; i < n; i++) {
                bool live = false;
                for (pane_t *p = panes; p; p = p->next) {
                    if (p == who[i]) live = true;
                }
                if (!live || who[i]->master != fds[i].fd) continue;
                if (fds[i].revents & POLLOUT) write_pty(who[i]);
                if (who[i]->master >= 0 && (fds[i].revents & (POLLIN | POLLHUP | POLLERR))) read_pty(who[i]);
            }
        }
        free(fds);
        free(who);
        // bytes a client or a reply queued this pass, sent without waiting for POLLOUT
        for (pane_t *p = panes; p; p = p->next) {
            if (p->master >= 0 && p->in_at < p->in.len) write_pty(p);
        }
        const uint64_t now = now_ms();
        pump(now);
        flush_client();

        if (stopping) {
            if (!stop_at) {
                stop_at = now + KILL_MS;
                for (pane_t *p = panes; p; p = p->next) {
                    if (p->pid > 0) kill(-p->pid, SIGHUP);
                }
            }
            bool any = false;
            for (pane_t *p = panes; p; p = p->next) {
                if (p->pid > 0) any = true;
            }
            if (!any || now >= stop_at) {
                for (pane_t *p = panes; p; p = p->next) {
                    if (p->pid > 0) kill(-p->pid, SIGKILL);
                }
                unlink(sock_path);
                return 0;
            }
        }
    }
}
