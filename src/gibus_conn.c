/* Ported from minidbus's conn/ (MIT; see LICENSE). */

#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "gibus_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#if !defined(MSG_NOSIGNAL) && !defined(SO_NOSIGPIPE)
#include <signal.h>
#endif

#define OPEN_MS          5000
#define BUS_CALL_MS      5000
#define DEFAULT_CALL_MS  25000
#define MAX_MESSAGE      (128u * 1024u * 1024u)
#define MAX_OUTGOING     MAX_MESSAGE
#define READ_CHUNK       65536
#define READ_ROUNDS      64
#define MAX_QUEUED       4096
#define QUEUE_KEEP       64
#define LOCAL_HANDLERS   8
#define NEVER            (~(uint64_t)0)
#define COUNT(a)         (sizeof(a) / sizeof((a)[0]))

#if defined(O_CLOEXEC)
#define CLOEXEC O_CLOEXEC
#else
#define CLOEXEC 0
#endif

#define ERR_NO_MEMORY   "org.freedesktop.DBus.Error.NoMemory"
#define ERR_BAD_ADDRESS "org.freedesktop.DBus.Error.BadAddress"
#define ERR_NO_SERVER   "org.freedesktop.DBus.Error.NoServer"
#define ERR_AUTH_FAILED "org.freedesktop.DBus.Error.AuthFailed"
#define ERR_TIMEOUT     "org.freedesktop.DBus.Error.Timeout"
#define IFACE_PEER      "org.freedesktop.DBus.Peer"

enum { H_FILTER, H_PATH, H_FALLBACK };

typedef struct handler {
    int           kind;
    char         *path;
    gibus_handler fn;
    void         *user;
} handler;

typedef struct waiter {
    struct waiter *next;
    uint32_t       serial;
    const char    *dest;
    gibus_msg     *reply;
} waiter;

struct gibus_conn {
    int             refs;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             has_lock, has_cond;
    int             fd;
    int             wake[2];
    int             open;
    int             reading;
    int             woken;
    uint32_t        serial;
    char           *unique;

    uint8_t        *out;
    size_t          out_at, out_len, out_cap;
    uint8_t        *in;
    size_t          in_len, in_cap, need;

    gibus_msg     **queue;
    int             qhead, nqueued, qcap;
    waiter         *waiters;
    handler        *handlers;
    int             nhandlers, chandlers;
};

static pthread_mutex_t g_session_lock = PTHREAD_MUTEX_INITIALIZER;
static gibus_conn     *g_session;

static uint64_t
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int
ms_left(uint64_t deadline)
{
    uint64_t now = now_ms();

    if (now >= deadline) return 0;
    return deadline - now > (uint64_t)INT_MAX ? INT_MAX : (int)(deadline - now);
}

static uint64_t
deadline_in(int ms)
{
    return ms < 0 ? NEVER : now_ms() + (uint64_t)ms;
}

static int
init_cond(pthread_cond_t *cond)
{
    pthread_condattr_t a;
    int ok;

    if (pthread_condattr_init(&a) != 0) return 0;
    ok = pthread_condattr_setclock(&a, CLOCK_MONOTONIC) == 0 && pthread_cond_init(cond, &a) == 0;
    pthread_condattr_destroy(&a);
    return ok;
}

static void
wait_locked(gibus_conn *c, int ms)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&c->cond, &c->lock, &ts);
}

static int
would_block(int e)
{
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    if (e == EWOULDBLOCK) return 1;
#endif
    return e == EAGAIN;
}

static int
wait_fd(int fd, short events, uint64_t deadline)
{
    for (;;) {
        struct pollfd p;
        int r, ms = deadline == NEVER ? -1 : ms_left(deadline);

        if (ms == 0) return 0;
        p.fd = fd;
        p.events = events;
        p.revents = 0;
        r = poll(&p, 1, ms);
        if (r > 0) return 1;
        if (r == 0) return 0;
        if (errno != EINTR && errno != EAGAIN) return -1;
    }
}

static ssize_t
send_bytes(int fd, const void *buf, size_t len)
{
#if defined(MSG_NOSIGNAL)
    return send(fd, buf, len, MSG_NOSIGNAL);
#elif defined(SO_NOSIGPIPE)
    return send(fd, buf, len, 0);
#else
    /* SIGPIPE from send goes to the calling thread: block it for the call and
       take back only one this call raised. */
    sigset_t pipe_set, pending, old;
    ssize_t n;
    int was, saved, sig;

    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_set, &old);
    sigpending(&pending);
    was = sigismember(&pending, SIGPIPE);
    n = send(fd, buf, len, 0);
    saved = errno;
    if (n < 0 && saved == EPIPE && !was) {
        sigpending(&pending);
        if (sigismember(&pending, SIGPIPE)) sigwait(&pipe_set, &sig);
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    errno = saved;
    return n;
#endif
}

static int
unhex(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static long
percent_decode(const char *s, size_t n, char *out, size_t cap)
{
    size_t i = 0, o = 0;

    while (i < n) {
        int ch = (unsigned char)s[i];

        if (ch == '%') {
            int hi, lo;

            if (i + 2 >= n) return -1;
            hi = unhex((unsigned char)s[i + 1]);
            lo = unhex((unsigned char)s[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            ch = hi << 4 | lo;
            i += 3;
        } else {
            i++;
        }
        if (o >= cap) return -1;
        out[o++] = (char)ch;
    }
    return (long)o;
}

static int
open_wake(int fd[2])
{
    int i, fl;

    if (pipe(fd) < 0) return 0;
    for (i = 0; i < 2; i++) {
        if (fcntl(fd[i], F_SETFD, FD_CLOEXEC) < 0 || (fl = fcntl(fd[i], F_GETFL)) < 0 ||
            fcntl(fd[i], F_SETFL, fl | O_NONBLOCK) < 0) {
            close(fd[0]);
            close(fd[1]);
            return 0;
        }
    }
    return 1;
}

static int
open_socket(void)
{
    int fd;
#if defined(SO_NOSIGPIPE)
    int one = 1;
#endif

#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
#else
    int fl;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || (fl = fcntl(fd, F_GETFL)) < 0 ||
        fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
#endif
#if defined(SO_NOSIGPIPE)
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) < 0) {
        close(fd);
        return -1;
    }
#endif
    return fd;
}

static int
connect_unix(const char *name, size_t len, int abstract, uint64_t deadline, gibus_error *err)
{
    struct sockaddr_un sa;
    socklen_t salen;
    int fd, e;

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (len + 1 > sizeof(sa.sun_path)) {
        gibus__error_set(err, ERR_BAD_ADDRESS, "socket name too long");
        return -1;
    }
    if (abstract) {
        memcpy(sa.sun_path + 1, name, len);
        salen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + len);
    } else {
        if (!len || strnlen(name, len) != len) {
            gibus__error_set(err, ERR_BAD_ADDRESS, "bad socket path");
            return -1;
        }
        memcpy(sa.sun_path, name, len);
        salen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + len + 1);
    }

    if ((fd = open_socket()) < 0) {
        gibus__error_set(err, GIBUS_ERROR_FAILED, "socket: %s", strerror(errno));
        return -1;
    }
    for (;;) {
        int soerr = 0, w;
        socklen_t sl = sizeof(soerr);

        if (connect(fd, (struct sockaddr *)&sa, salen) == 0) return fd;
        e = errno;
        if (e == EISCONN) return fd;
        if (e == EAGAIN && ms_left(deadline) > 0) {
            /* Linux: the listener's backlog is full; nothing is in progress. */
            struct timespec ts;

            ts.tv_sec = 0;
            ts.tv_nsec = 10000000L;
            nanosleep(&ts, NULL);
            continue;
        }
        if (e != EINPROGRESS && e != EALREADY && e != EINTR) break;
        w = wait_fd(fd, POLLOUT, deadline);
        if (w == 0) {
            close(fd);
            gibus__error_set(err, ERR_TIMEOUT, "timed out connecting to the bus");
            return -1;
        }
        if (w < 0) {
            e = errno;
            break;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0) {
            e = errno;
            break;
        }
        if (soerr == 0 || soerr == EISCONN) return fd;
        e = soerr;
        break;
    }
    close(fd);
    gibus__error_set(err, ERR_NO_SERVER, "cannot connect to %s%.*s: %s", abstract ? "@" : "",
              (int)len, name, strerror(e));
    return -1;
}

static int
connect_entry(const char *entry, size_t n, uint64_t deadline, gibus_error *err)
{
    char name[sizeof(((struct sockaddr_un *)0)->sun_path)];
    const char *kv, *stop = entry + n, *val = NULL;
    size_t vlen = 0;
    int abstract = 0;
    long len;

    if (n < 5 || strncmp(entry, "unix:", 5) != 0) {
        gibus__error_set(err, ERR_BAD_ADDRESS, "unsupported transport in \"%.*s\"", (int)n, entry);
        return -1;
    }
    kv = entry + 5;
    while (kv < stop) {
        const char *ke = kv;

        while (ke < stop && *ke != ',') ke++;
        if (!val && ke - kv > 5 && strncmp(kv, "path=", 5) == 0) {
            val = kv + 5;
            vlen = (size_t)(ke - val);
            abstract = 0;
        } else if (!val && ke - kv > 9 && strncmp(kv, "abstract=", 9) == 0) {
            val = kv + 9;
            vlen = (size_t)(ke - val);
            abstract = 1;
        }
        if (ke == stop) break;
        kv = ke + 1;
    }
    if (!val) {
        gibus__error_set(err, ERR_BAD_ADDRESS, "no path in \"%.*s\"", (int)n, entry);
        return -1;
    }
#ifndef __linux__
    if (abstract) {
        gibus__error_set(err, ERR_BAD_ADDRESS, "abstract sockets are Linux's alone");
        return -1;
    }
#endif
    len = percent_decode(val, vlen, name, sizeof(name) - 1);
    if (len < 0) {
        gibus__error_set(err, ERR_BAD_ADDRESS, "bad value in \"%.*s\"", (int)n, entry);
        return -1;
    }
    return connect_unix(name, (size_t)len, abstract, deadline, err);
}

static int
connect_address(const char *address, uint64_t deadline, gibus_error *err)
{
    const char *p = address;
    gibus_error last;
    int fd = -1;

    memset(&last, 0, sizeof(last));
    for (;;) {
        const char *end = strchr(p, ';');
        size_t n = end ? (size_t)(end - p) : strlen(p);

        if (n) {
            memset(&last, 0, sizeof(last));
            if ((fd = connect_entry(p, n, deadline, &last)) >= 0) return fd;
        }
        if (!end) break;
        p = end + 1;
        if (!ms_left(deadline)) {
            memset(&last, 0, sizeof(last));
            gibus__error_set(&last, ERR_TIMEOUT, "timed out connecting to the bus");
            break;
        }
    }
    if (last.set)
        gibus__error_set(err, last.name, "%s", last.message);
    else
        gibus__error_set(err, ERR_BAD_ADDRESS, "empty bus address");
    return -1;
}

static int
connect_session(uint64_t deadline, gibus_error *err)
{
    const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
    const char *dir;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path) + 8];
    int n;

    if (addr && *addr) return connect_address(addr, deadline, err);
    dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir) {
        gibus__error_set(err, ERR_NO_SERVER,
                  "no session bus: DBUS_SESSION_BUS_ADDRESS and XDG_RUNTIME_DIR are unset");
        return -1;
    }
    n = snprintf(path, sizeof(path), "%s/bus", dir);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        gibus__error_set(err, ERR_BAD_ADDRESS, "XDG_RUNTIME_DIR is too long");
        return -1;
    }
    return connect_unix(path, (size_t)n, 0, deadline, err);
}

static int
write_all(int fd, const char *buf, size_t len, uint64_t deadline)
{
    while (len) {
        ssize_t n = send_bytes(fd, buf, len);

        if (n > 0) {
            buf += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && would_block(errno) && wait_fd(fd, POLLOUT, deadline) > 0) continue;
        return 0;
    }
    return 1;
}

/* FreeBSD's and DragonFly's dbus-daemon may take the credentials from an
   SCM_CREDS message sent with the NUL byte; the kernel fills them in. */
static int
send_nul(int fd, uint64_t deadline)
{
#if defined(SCM_CREDS) && (defined(__FreeBSD__) || defined(__DragonFly__))
    union {
        struct cmsghdr h;
        char           buf[CMSG_SPACE(sizeof(struct cmsgcred))];
    } ctl;
    struct msghdr mh;
    struct iovec iov;
    char nul = 0;
    int flags = 0;

#if defined(MSG_NOSIGNAL)
    flags = MSG_NOSIGNAL;
#endif
    for (;;) {
        ssize_t n;

        memset(&ctl, 0, sizeof(ctl));
        memset(&mh, 0, sizeof(mh));
        iov.iov_base = &nul;
        iov.iov_len = 1;
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = ctl.buf;
        mh.msg_controllen = sizeof(ctl.buf);
        ctl.h.cmsg_len = CMSG_LEN(sizeof(struct cmsgcred));
        ctl.h.cmsg_level = SOL_SOCKET;
        ctl.h.cmsg_type = SCM_CREDS;
        n = sendmsg(fd, &mh, flags);
        if (n == 1) return 1;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && would_block(errno) && wait_fd(fd, POLLOUT, deadline) > 0) continue;
        return 0;
    }
#else
    return write_all(fd, "", 1, deadline);
#endif
}

static int
read_line(int fd, char *line, size_t cap, uint64_t deadline, gibus_error *err)
{
    size_t n = 0;

    for (;;) {
        char ch;
        ssize_t r = recv(fd, &ch, 1, 0);

        if (r == 0) {
            gibus__error_set(err, ERR_AUTH_FAILED, "the bus hung up during authentication");
            return 0;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            if (would_block(errno)) {
                int w = wait_fd(fd, POLLIN, deadline);

                if (w > 0) continue;
                if (w == 0)
                    gibus__error_set(err, ERR_TIMEOUT, "timed out authenticating");
                else
                    gibus__error_set(err, ERR_AUTH_FAILED, "poll: %s", strerror(errno));
                return 0;
            }
            gibus__error_set(err, ERR_AUTH_FAILED, "recv: %s", strerror(errno));
            return 0;
        }
        if ((unsigned char)ch > 0x7f || n + 1 >= cap) {
            gibus__error_set(err, ERR_AUTH_FAILED, "malformed authentication reply");
            return 0;
        }
        line[n++] = ch;
        if (n >= 2 && line[n - 2] == '\r' && line[n - 1] == '\n') {
            line[n - 2] = 0;
            return 1;
        }
    }
}

static int
authenticate(int fd, uint64_t deadline, gibus_error *err)
{
    static const char digits[] = "0123456789abcdef", auth[] = "AUTH EXTERNAL ";
    char uid[24], buf[96], line[512];
    size_t i, n = sizeof(auth) - 1;

    snprintf(uid, sizeof(uid), "%lu", (unsigned long)getuid());
    memcpy(buf, auth, n);
    for (i = 0; uid[i]; i++) {
        buf[n++] = digits[((unsigned char)uid[i] >> 4) & 15];
        buf[n++] = digits[(unsigned char)uid[i] & 15];
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    if (!send_nul(fd, deadline) || !write_all(fd, buf, n, deadline)) {
        gibus__error_set(err, ERR_AUTH_FAILED, "cannot write to the bus");
        return 0;
    }
    if (!read_line(fd, line, sizeof(line), deadline, err)) return 0;
    if (strncmp(line, "OK ", 3) != 0) {
        gibus__error_set(err, ERR_AUTH_FAILED, "the bus refused EXTERNAL: %s", line);
        return 0;
    }
    for (i = 3; line[i]; i++)
        if (unhex((unsigned char)line[i]) < 0) break;
    if (i != 3 + 32 || line[i]) {
        gibus__error_set(err, ERR_AUTH_FAILED, "malformed OK line from the bus");
        return 0;
    }
    if (!write_all(fd, "BEGIN\r\n", 7, deadline)) {
        gibus__error_set(err, ERR_AUTH_FAILED, "cannot write to the bus");
        return 0;
    }
    return 1;
}

static void
fail_locked(gibus_conn *c)
{
    if (!c->open) return;
    c->open = 0;
    shutdown(c->fd, SHUT_RDWR);
    free(c->out);
    c->out = NULL;
    c->out_at = c->out_len = c->out_cap = 0;
    free(c->in);
    c->in = NULL;
    c->in_len = c->in_cap = c->need = 0;
    pthread_cond_broadcast(&c->cond);
}

static void
write_locked(gibus_conn *c)
{
    while (c->open && c->out_at < c->out_len) {
        ssize_t n = send_bytes(c->fd, c->out + c->out_at, c->out_len - c->out_at);

        if (n > 0) {
            c->out_at += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && would_block(errno)) return;
        fail_locked(c);
        return;
    }
    c->out_at = c->out_len = 0;
    if (c->out_cap > 4 * READ_CHUNK) {
        free(c->out);
        c->out = NULL;
        c->out_cap = 0;
    }
}

static size_t
send_now_locked(gibus_conn *c, const uint8_t *b, size_t n)
{
    size_t sent = 0;

    while (sent < n) {
        ssize_t k = send_bytes(c->fd, b + sent, n - sent);

        if (k > 0) {
            sent += (size_t)k;
            continue;
        }
        if (k < 0 && errno == EINTR) continue;
        if (!(k < 0 && would_block(errno))) fail_locked(c);
        break;
    }
    return sent;
}

static void
wake_locked(gibus_conn *c)
{
    static const char byte = 0;

    if (!c->reading || c->woken || c->out_at == c->out_len) return;
    c->woken = 1;
    while (write(c->wake[1], &byte, 1) < 0 && errno == EINTR)
        ;
}

static void
drain_wake_locked(gibus_conn *c)
{
    char buf[16];
    ssize_t n;

    if (!c->woken) return;
    c->woken = 0;
    do n = read(c->wake[0], buf, sizeof(buf));
    while (n > 0 || (n < 0 && errno == EINTR));
}

static int
room_locked(gibus_conn *c, size_t n)
{
    size_t pending = c->out_len - c->out_at;

    if (c->out_len + n > c->out_cap && c->out_at) {
        memmove(c->out, c->out + c->out_at, pending);
        c->out_at = 0;
        c->out_len = pending;
    }
    if (c->out_len + n > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 4096;
        uint8_t *p;

        while (cap < c->out_len + n) cap *= 2;
        if (!(p = (uint8_t *)realloc(c->out, cap))) return 0;
        c->out = p;
        c->out_cap = cap;
    }
    return 1;
}

static int
queue_locked(gibus_conn *c, gibus_msg *m, uint32_t *serial)
{
    const uint8_t *b;
    size_t n, sent = 0;
    uint32_t s;

    if (!c->open) return 0;
    s = c->serial;
    if (!(b = gibus_msg_bytes(m, s, &n)) || !n) return 0;
    if (c->out_at == c->out_len) c->out_at = c->out_len = 0;
    if (n > MAX_OUTGOING - (c->out_len - c->out_at)) return 0;
    if (!room_locked(c, n)) return 0;
    if (c->out_at == c->out_len) sent = send_now_locked(c, b, n);
    if (c->open && sent < n) {
        memcpy(c->out + c->out_len, b + sent, n - sent);
        c->out_len += n - sent;
        wake_locked(c);
    }
    c->serial = s + 1 ? s + 1 : 1;
    *serial = s;
    return 1;
}

static int
answers(const waiter *w, const gibus_msg *m)
{
    const char *from = gibus_msg_sender(m);

    if (!w->dest || (w->dest[0] != ':' && strcmp(w->dest, GIBUS_SERVICE_DBUS) != 0))
        return 1;
    if (!from) return 0;
    if (strcmp(from, w->dest) == 0) return 1;
    return w->dest[0] == ':' && gibus_msg_type(m) == GIBUS_MESSAGE_ERROR &&
           strcmp(from, GIBUS_SERVICE_DBUS) == 0;
}

static int
grow_queue_locked(gibus_conn *c)
{
    int cap = c->qcap ? c->qcap * 2 : 16;
    gibus_msg **p = (gibus_msg **)realloc(c->queue, sizeof(*p) * (size_t)cap);

    if (!p) return 0;
    if (c->qhead + c->nqueued > c->qcap)
        memcpy(p + c->qcap, p, sizeof(*p) * (size_t)(c->qhead + c->nqueued - c->qcap));
    c->queue = p;
    c->qcap = cap;
    return 1;
}

static gibus_msg *
pop_locked(gibus_conn *c)
{
    gibus_msg *m;

    if (!c->nqueued) return NULL;
    m = c->queue[c->qhead];
    c->qhead = (c->qhead + 1) & (c->qcap - 1);
    if (!--c->nqueued) {
        c->qhead = 0;
        if (c->qcap > QUEUE_KEEP) {
            free(c->queue);
            c->queue = NULL;
            c->qcap = 0;
        }
    }
    return m;
}

static void
route_locked(gibus_conn *c, gibus_msg *m)
{
    int type = gibus_msg_type(m);

    if (type == GIBUS_MESSAGE_METHOD_RETURN || type == GIBUS_MESSAGE_ERROR) {
        uint32_t rs = gibus_msg_reply_serial(m);
        waiter *w;

        for (w = c->waiters; w; w = w->next) {
            if (w->serial == rs && !w->reply && answers(w, m)) {
                w->reply = m;
                return;
            }
        }
        gibus_msg_free(m);
        return;
    }
    if (type != GIBUS_MESSAGE_METHOD_CALL && type != GIBUS_MESSAGE_SIGNAL) {
        gibus_msg_free(m);
        return;
    }
    if (c->nqueued >= MAX_QUEUED) {
        gibus_msg_free(pop_locked(c));
    } else if (c->nqueued == c->qcap && !grow_queue_locked(c)) {
        gibus_msg_free(m);
        return;
    }
    c->queue[(c->qhead + c->nqueued) & (c->qcap - 1)] = m;
    c->nqueued++;
}

static void
frames_locked(gibus_conn *c)
{
    size_t at = 0;

    c->need = 0;
    while (c->open) {
        size_t have = c->in_len - at;
        long n = gibus__frame_size(c->in + at, have);
        gibus_msg *m;
        int type;

        if (n < 0 || (unsigned long)n > MAX_MESSAGE) {
            fail_locked(c);
            return;
        }
        if (n == 0 || (size_t)n > have) {
            c->need = (size_t)n;
            break;
        }
        type = c->in[at + 1];
        if (type > GIBUS_MESSAGE_SIGNAL) {
            at += (size_t)n;
            continue;
        }
        m = gibus_msg_parse(c->in + at, (size_t)n);
        at += (size_t)n;
        if (m) route_locked(c, m);
    }
    if (at == c->in_len) {
        c->in_len = 0;
        if (c->in_cap > 4 * READ_CHUNK) {
            free(c->in);
            c->in = NULL;
            c->in_cap = 0;
        }
    } else if (at) {
        memmove(c->in, c->in + at, c->in_len - at);
        c->in_len -= at;
    }
}

static void
read_locked(gibus_conn *c)
{
    int round;

    for (round = 0; round < READ_ROUNDS && c->open; round++) {
        size_t want = READ_CHUNK;
        ssize_t n;

        if (c->need > c->in_len && c->need - c->in_len > want) want = c->need - c->in_len;
        if (c->in_cap - c->in_len < want) {
            size_t cap = c->in_len + want;
            uint8_t *p = (uint8_t *)realloc(c->in, cap);

            if (!p) {
                fail_locked(c);
                return;
            }
            c->in = p;
            c->in_cap = cap;
        }
        n = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len, 0);
        if (n > 0) {
            c->in_len += (size_t)n;
            frames_locked(c);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && would_block(errno)) return;
        fail_locked(c);
        return;
    }
}

static void
io_locked(gibus_conn *c, int timeout_ms)
{
    uint64_t deadline = deadline_in(timeout_ms);

    write_locked(c);
    while (c->open && timeout_ms != 0) {
        struct pollfd p[2];
        int r, e, ms = deadline == NEVER ? -1 : ms_left(deadline);

        if (ms == 0) break;
        p[0].fd = c->fd;
        p[0].events = POLLIN;
        p[0].revents = 0;
        if (c->out_at < c->out_len) p[0].events |= POLLOUT;
        p[1].fd = c->wake[0];
        p[1].events = POLLIN;
        p[1].revents = 0;
        c->reading = 1;
        pthread_mutex_unlock(&c->lock);
        r = poll(p, 2, ms);
        e = errno;
        pthread_mutex_lock(&c->lock);
        c->reading = 0;
        drain_wake_locked(c);
        if (r == 0) break;
        if (r < 0) {
            if (e == EINTR || e == EAGAIN) continue;
            fail_locked(c);
            break;
        }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) break;
        write_locked(c);
    }
    write_locked(c);
    read_locked(c);
    pthread_cond_broadcast(&c->cond);
}

static void
destroy(gibus_conn *c)
{
    int i;

    while (c->nqueued) gibus_msg_free(pop_locked(c));
    free(c->queue);
    for (i = 0; i < c->nhandlers; i++) free(c->handlers[i].path);
    free(c->handlers);
    if (c->fd >= 0) close(c->fd);
    if (c->wake[0] >= 0) close(c->wake[0]);
    if (c->wake[1] >= 0) close(c->wake[1]);
    free(c->out);
    free(c->in);
    free(c->unique);
    if (c->has_cond) pthread_cond_destroy(&c->cond);
    if (c->has_lock) pthread_mutex_destroy(&c->lock);
    free(c);
}

static gibus_conn *
establish(int fd, uint64_t deadline, gibus_error *err)
{
    gibus_conn *c;
    gibus_msg *m, *r;
    const char *name = NULL;
    int left;

    if (fd < 0) return NULL;
    if (!authenticate(fd, deadline, err)) {
        close(fd);
        return NULL;
    }
    if (!(c = (gibus_conn *)calloc(1, sizeof(*c)))) {
        close(fd);
        gibus__error_set(err, ERR_NO_MEMORY, "out of memory");
        return NULL;
    }
    c->fd = fd;
    c->wake[0] = c->wake[1] = -1;
    c->open = 1;
    c->serial = 1;
    c->refs = 1;
    c->has_lock = pthread_mutex_init(&c->lock, NULL) == 0;
    c->has_cond = init_cond(&c->cond);
    if (!c->has_lock || !c->has_cond) {
        destroy(c);
        gibus__error_set(err, ERR_NO_MEMORY, "out of memory");
        return NULL;
    }
    if (!open_wake(c->wake)) {
        gibus__error_set(err, GIBUS_ERROR_FAILED, "pipe: %s", strerror(errno));
        c->wake[0] = c->wake[1] = -1;
        destroy(c);
        return NULL;
    }

    m = gibus_msg_call(GIBUS_SERVICE_DBUS, GIBUS_PATH_DBUS, GIBUS_INTERFACE_DBUS, "Hello");
    if (!m) {
        destroy(c);
        gibus__error_set(err, ERR_NO_MEMORY, "out of memory");
        return NULL;
    }
    left = ms_left(deadline);
    r = gibus_call(c, m, left > 1 ? left : 1, err);
    gibus_msg_free(m);
    if (!r) {
        destroy(c);
        return NULL;
    }
    if (!gibus_msg_get_args(r, NULL, GIBUS_TYPE_STRING, &name, GIBUS_TYPE_INVALID) ||
        !name || name[0] != ':' || !(c->unique = strdup(name))) {
        gibus_msg_free(r);
        destroy(c);
        gibus__error_set(err, GIBUS_ERROR_FAILED, "the bus answered Hello without a unique name");
        return NULL;
    }
    gibus_msg_free(r);
    return c;
}

gibus_conn *
gibus_open(const char *address, gibus_error *err)
{
    uint64_t deadline = deadline_in(OPEN_MS);

    if (!address) {
        gibus__error_set(err, ERR_BAD_ADDRESS, "no bus address");
        return NULL;
    }
    return establish(connect_address(address, deadline, err), deadline, err);
}

gibus_conn *
gibus_session_private(gibus_error *err)
{
    uint64_t deadline = deadline_in(OPEN_MS);

    return establish(connect_session(deadline, err), deadline, err);
}

gibus_conn *
gibus_session(gibus_error *err)
{
    gibus_conn *c;

    pthread_mutex_lock(&g_session_lock);
    if (g_session && !gibus_connected(g_session)) {
        gibus_unref(g_session);
        g_session = NULL;
    }
    if (!g_session) g_session = gibus_session_private(err);
    if ((c = g_session)) __atomic_fetch_add(&c->refs, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&g_session_lock);
    return c;
}

static int
release(gibus_conn *c)
{
    if (__atomic_sub_fetch(&c->refs, 1, __ATOMIC_SEQ_CST) != 0) return 0;
    destroy(c);
    return 1;
}

void
gibus_unref(gibus_conn *c)
{
    if (c) release(c);
}

const char *
gibus_unique_name(gibus_conn *c)
{
    return c ? c->unique : NULL;
}

int
gibus_fd(gibus_conn *c)
{
    int fd;

    if (!c) return -1;
    pthread_mutex_lock(&c->lock);
    fd = c->open ? c->fd : -1;
    pthread_mutex_unlock(&c->lock);
    return fd;
}

int
gibus_connected(gibus_conn *c)
{
    int open;

    if (!c) return 0;
    pthread_mutex_lock(&c->lock);
    open = c->open;
    pthread_mutex_unlock(&c->lock);
    return open;
}

int
gibus_send(gibus_conn *c, gibus_msg *m, uint32_t *serial)
{
    uint32_t s = 0;
    int ok;

    if (!c || !m) return 0;
    pthread_mutex_lock(&c->lock);
    ok = queue_locked(c, m, &s);
    if (ok) write_locked(c);
    ok = ok && c->open;
    pthread_mutex_unlock(&c->lock);
    if (ok && serial) *serial = s;
    return ok;
}

int
gibus_flush(gibus_conn *c)
{
    int ok;

    if (!c) return 0;
    pthread_mutex_lock(&c->lock);
    write_locked(c);
    while (c->open && c->out_at < c->out_len) {
        int fd = c->fd;

        if (!c->reading) io_locked(c, 0);
        if (!c->open || c->out_at == c->out_len) break;
        pthread_mutex_unlock(&c->lock);
        wait_fd(fd, POLLOUT, deadline_in(1000));
        pthread_mutex_lock(&c->lock);
        write_locked(c);
    }
    ok = c->open;
    pthread_mutex_unlock(&c->lock);
    return ok;
}

gibus_msg *
gibus_call(gibus_conn *c, gibus_msg *m, int timeout_ms, gibus_error *err)
{
    waiter w, **pp;
    uint64_t deadline;
    gibus_msg *r;
    int open;

    if (!c || !m) {
        gibus__error_set(err, GIBUS_ERROR_FAILED, "no connection or no message");
        return NULL;
    }
    if (gibus_msg_type(m) != GIBUS_MESSAGE_METHOD_CALL || gibus__msg_no_reply(m)) {
        gibus__error_set(err, GIBUS_ERROR_INVALID_ARGS, "no reply to wait for");
        return NULL;
    }
    if (timeout_ms < 0) timeout_ms = DEFAULT_CALL_MS;
    deadline = deadline_in(timeout_ms);
    w.next = NULL;
    w.serial = 0;
    w.dest = gibus_msg_destination(m);
    w.reply = NULL;

    pthread_mutex_lock(&c->lock);
    if (!c->open) {
        pthread_mutex_unlock(&c->lock);
        gibus__error_set(err, GIBUS_ERROR_DISCONNECTED, "not connected to the bus");
        return NULL;
    }
    if (!queue_locked(c, m, &w.serial)) {
        pthread_mutex_unlock(&c->lock);
        gibus__error_set(err, GIBUS_ERROR_FAILED, "the call could not be queued");
        return NULL;
    }
    w.next = c->waiters;
    c->waiters = &w;
    write_locked(c);
    while (!w.reply && c->open) {
        int left = ms_left(deadline);

        if (!left) break;
        if (!c->reading)
            io_locked(c, left);
        else
            wait_locked(c, left);
    }
    for (pp = &c->waiters; *pp; pp = &(*pp)->next) {
        if (*pp == &w) {
            *pp = w.next;
            break;
        }
    }
    open = c->open;
    pthread_mutex_unlock(&c->lock);

    if (!(r = w.reply)) {
        if (open)
            gibus__error_set(err, GIBUS_ERROR_NO_REPLY, "no reply within %d ms", timeout_ms);
        else
            gibus__error_set(err, GIBUS_ERROR_DISCONNECTED, "disconnected before the reply");
        return NULL;
    }
    if (gibus_msg_type(r) == GIBUS_MESSAGE_ERROR) {
        const char *name = gibus_msg_error_name(r), *sig = gibus_msg_signature(r);
        const char *text = NULL;

        if (sig && sig[0] == GIBUS_TYPE_STRING)
            gibus_msg_get_args(r, NULL, GIBUS_TYPE_STRING, &text, GIBUS_TYPE_INVALID);
        gibus__error_set(err, name ? name : GIBUS_ERROR_FAILED, "%s", text ? text : "");
        gibus_msg_free(r);
        return NULL;
    }
    return r;
}

int
gibus_read_write(gibus_conn *c, int timeout_ms)
{
    int ok;

    if (!c) return 0;
    pthread_mutex_lock(&c->lock);
    if (c->nqueued) timeout_ms = 0;
    if (c->open && !c->reading) {
        io_locked(c, timeout_ms);
    } else if (c->open) {
        write_locked(c);
        if (timeout_ms < 0)
            pthread_cond_wait(&c->cond, &c->lock);
        else if (timeout_ms > 0)
            wait_locked(c, timeout_ms);
    }
    ok = c->open;
    pthread_mutex_unlock(&c->lock);
    return ok;
}

static int
covers(const char *base, const char *path)
{
    size_t n = strlen(base);

    if (n == 1) return path[0] == '/' && path[1];
    return strncmp(base, path, n) == 0 && path[n] == '/';
}

static int
read_machine_id(char *id)
{
    static const char *const files[] = { "/etc/machine-id", "/var/lib/dbus/machine-id",
                                         "/var/db/dbus/machine-id" };
    size_t f;

    for (f = 0; f < COUNT(files); f++) {
        int fd = open(files[f], O_RDONLY | CLOEXEC), i;
        ssize_t n;

        if (fd < 0) continue;
        n = read(fd, id, 32);
        close(fd);
        if (n != 32) continue;
        for (i = 0; i < 32 && unhex((unsigned char)id[i]) >= 0; i++)
            ;
        if (i == 32) {
            id[32] = 0;
            return 1;
        }
    }
    return 0;
}

static int
is_peer(const gibus_msg *m)
{
    const char *iface = gibus_msg_interface(m);

    return gibus_msg_type(m) == GIBUS_MESSAGE_METHOD_CALL && iface &&
           strcmp(iface, IFACE_PEER) == 0;
}

static void
answer_peer(gibus_conn *c, gibus_msg *m)
{
    char id[33];
    const char *s = id;
    gibus_msg *r;

    if (gibus__msg_no_reply(m)) return;
    if (gibus_msg_has_member(m, "Ping")) {
        r = gibus_msg_return(m);
    } else if (!gibus_msg_has_member(m, "GetMachineId")) {
        r = gibus_msg_error(m, GIBUS_ERROR_UNKNOWN_METHOD,
                            "Unknown method invoked on org.freedesktop.DBus.Peer interface");
    } else if (!read_machine_id(id)) {
        r = gibus_msg_error(m, GIBUS_ERROR_FAILED, "no machine ID");
    } else if ((r = gibus_msg_return(m)) &&
               !gibus_msg_append_args(r, GIBUS_TYPE_STRING, &s, GIBUS_TYPE_INVALID)) {
        gibus_msg_free(r);
        r = NULL;
    }
    if (r) {
        gibus_send(c, r, NULL);
        gibus_msg_free(r);
    }
}

typedef struct strbuf {
    char  *s;
    size_t len, cap;
    int    failed;
} strbuf;

static void
strbuf_add(strbuf *b, const char *s, size_t n)
{
    if (b->failed) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 512;
        char *p;

        while (cap < b->len + n + 1) cap *= 2;
        if (!(p = (char *)realloc(b->s, cap))) {
            b->failed = 1;
            return;
        }
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

static int
path_char(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '_';
}

static const char *
child_of(const handler *h, const char *path, size_t skip, size_t *len)
{
    const char *p;
    size_t n;

    if (h->kind == H_FILTER || !covers(path, h->path)) return NULL;
    p = h->path + skip;
    for (n = 0; p[n] && p[n] != '/'; n++)
        if (!path_char(p[n])) return NULL;
    *len = n;
    return n ? p : NULL;
}

static gibus_msg *
introspect_reply(gibus_conn *c, gibus_msg *m)
{
    static const char head[] = GIBUS_INTROSPECT_DOCTYPE "<node>\n";
    const char *path = gibus_msg_path(m), *xml;
    gibus_msg *r = NULL;
    strbuf b;
    size_t skip;
    int i, j;

    if (!path || !gibus_msg_is_call(m, GIBUS_INTERFACE_INTROSPECTABLE, "Introspect")) return NULL;
    memset(&b, 0, sizeof(b));
    skip = path[1] ? strlen(path) + 1 : 1;
    strbuf_add(&b, head, sizeof(head) - 1);
    pthread_mutex_lock(&c->lock);
    for (i = 0; i < c->nhandlers; i++) {
        size_t n = 0, k = 0;
        const char *e = child_of(&c->handlers[i], path, skip, &n), *f;

        if (!e) continue;
        for (j = 0; j < i; j++) {
            f = child_of(&c->handlers[j], path, skip, &k);
            if (f && k == n && memcmp(e, f, n) == 0) break;
        }
        if (j < i) continue;
        strbuf_add(&b, "  <node name=\"", 14);
        strbuf_add(&b, e, n);
        strbuf_add(&b, "\"/>\n", 4);
    }
    pthread_mutex_unlock(&c->lock);
    strbuf_add(&b, "</node>\n", 8);
    xml = b.s;
    if (!b.failed && (r = gibus_msg_return(m)) &&
        !gibus_msg_append_args(r, GIBUS_TYPE_STRING, &xml, GIBUS_TYPE_INVALID)) {
        gibus_msg_free(r);
        r = NULL;
    }
    free(b.s);
    return r;
}

static void
answer_unhandled(gibus_conn *c, gibus_msg *m)
{
    gibus_msg *r = introspect_reply(c, m);

    if (!r) {
        const char *iface = gibus_msg_interface(m), *member = gibus_msg_member(m);
        const char *sig = gibus_msg_signature(m), *path = gibus_msg_path(m);
        char text[256];

        snprintf(text, sizeof(text), "No method %s with signature \"%s\" on interface %s at %s",
                 member ? member : "", sig ? sig : "", iface ? iface : "(none)", path ? path : "");
        r = gibus_msg_error(m, GIBUS_ERROR_UNKNOWN_METHOD, text);
    }
    if (r) {
        gibus_send(c, r, NULL);
        gibus_msg_free(r);
    }
}

int
gibus_dispatch(gibus_conn *c)
{
    gibus_msg *m;
    handler local[LOCAL_HANDLERS], *calls = NULL;
    const char *path;
    int i, j, n = 0, handled = 0, peer, more;

    if (!c) return 0;
    pthread_mutex_lock(&c->lock);
    if (!(m = pop_locked(c))) {
        pthread_mutex_unlock(&c->lock);
        return 0;
    }
    /* A handler may drop the caller's last reference. */
    __atomic_fetch_add(&c->refs, 1, __ATOMIC_SEQ_CST);

    path = gibus_msg_path(m);
    peer = is_peer(m);
    if (!peer && c->nhandlers <= LOCAL_HANDLERS)
        calls = local;
    else if (!peer)
        calls = (handler *)malloc(sizeof(*calls) * (size_t)c->nhandlers);
    if (calls) {
        for (i = 0; i < c->nhandlers; i++)
            if (c->handlers[i].kind == H_FILTER) calls[n++] = c->handlers[i];
        if (path) {
            int first;

            for (i = 0; i < c->nhandlers; i++) {
                if (c->handlers[i].kind != H_FILTER && strcmp(c->handlers[i].path, path) == 0) {
                    calls[n++] = c->handlers[i];
                    break;
                }
            }
            first = n;
            for (i = 0; i < c->nhandlers; i++) {
                handler h = c->handlers[i];

                if (h.kind != H_FALLBACK || !covers(h.path, path)) continue;
                for (j = n; j > first && strlen(calls[j - 1].path) < strlen(h.path); j--)
                    calls[j] = calls[j - 1];
                calls[j] = h;
                n++;
            }
        }
    }
    pthread_mutex_unlock(&c->lock);

    if (peer) {
        answer_peer(c, m);
    } else {
        for (i = 0; i < n && !handled; i++)
            handled = calls[i].fn(c, m, calls[i].user) == GIBUS_HANDLED;
        if (!handled && gibus_msg_type(m) == GIBUS_MESSAGE_METHOD_CALL && !gibus__msg_no_reply(m))
            answer_unhandled(c, m);
    }
    if (calls != local) free(calls);
    gibus_msg_free(m);

    pthread_mutex_lock(&c->lock);
    more = c->nqueued != 0;
    pthread_mutex_unlock(&c->lock);
    return release(c) ? 0 : more;
}

int
gibus_read_write_dispatch(gibus_conn *c, int timeout_ms)
{
    int ok;

    if (!c) return 0;
    __atomic_fetch_add(&c->refs, 1, __ATOMIC_SEQ_CST);
    gibus_read_write(c, timeout_ms);
    while (gibus_dispatch(c))
        ;
    ok = gibus_connected(c);
    return release(c) ? 0 : ok;
}

static int
add_handler(gibus_conn *c, int kind, const char *path, gibus_handler fn, void *user)
{
    handler h;
    int i;

    if (!c || !fn) return 0;
    if (kind != H_FILTER && (!path || path[0] != '/')) return 0;
    h.kind = kind;
    h.path = NULL;
    h.fn = fn;
    h.user = user;
    if (kind != H_FILTER && !(h.path = strdup(path))) return 0;

    pthread_mutex_lock(&c->lock);
    for (i = 0; kind != H_FILTER && i < c->nhandlers; i++) {
        if (c->handlers[i].kind != H_FILTER && strcmp(c->handlers[i].path, path) == 0) {
            pthread_mutex_unlock(&c->lock);
            free(h.path);
            return 0;
        }
    }
    if (c->nhandlers == c->chandlers) {
        int cap = c->chandlers ? c->chandlers * 2 : 8;
        handler *p = (handler *)realloc(c->handlers, sizeof(*p) * (size_t)cap);

        if (!p) {
            pthread_mutex_unlock(&c->lock);
            free(h.path);
            return 0;
        }
        c->handlers = p;
        c->chandlers = cap;
    }
    c->handlers[c->nhandlers++] = h;
    pthread_mutex_unlock(&c->lock);
    return 1;
}

int
gibus_add_filter(gibus_conn *c, gibus_handler fn, void *user)
{
    return add_handler(c, H_FILTER, NULL, fn, user);
}

int
gibus_register_path(gibus_conn *c, const char *path, gibus_handler fn, void *user)
{
    return add_handler(c, H_PATH, path, fn, user);
}

int
gibus_register_fallback(gibus_conn *c, const char *path, gibus_handler fn, void *user)
{
    return add_handler(c, H_FALLBACK, path, fn, user);
}

static gibus_msg *
bus_call(gibus_conn *c, const char *member, const char *s, const uint32_t *u, gibus_error *err)
{
    gibus_msg *m, *r;
    int ok;

    if (!c || !s) {
        gibus__error_set(err, GIBUS_ERROR_INVALID_ARGS, "no connection or no argument");
        return NULL;
    }
    if (!(m = gibus_msg_call(GIBUS_SERVICE_DBUS, GIBUS_PATH_DBUS, GIBUS_INTERFACE_DBUS, member))) {
        gibus__error_set(err, ERR_NO_MEMORY, "out of memory");
        return NULL;
    }
    if (u)
        ok = gibus_msg_append_args(m, GIBUS_TYPE_STRING, &s, GIBUS_TYPE_UINT32, u,
                                   GIBUS_TYPE_INVALID);
    else
        ok = gibus_msg_append_args(m, GIBUS_TYPE_STRING, &s, GIBUS_TYPE_INVALID);
    if (!ok) {
        gibus_msg_free(m);
        gibus__error_set(err, GIBUS_ERROR_INVALID_ARGS, "bad argument to %s", member);
        return NULL;
    }
    r = gibus_call(c, m, BUS_CALL_MS, err);
    gibus_msg_free(m);
    return r;
}

int
gibus_request_name(gibus_conn *c, const char *name, unsigned flags, gibus_error *err)
{
    uint32_t f = flags, code = 0;
    gibus_msg *r = bus_call(c, "RequestName", name, &f, err);
    int ok;

    if (!r) return -1;
    ok = gibus_msg_get_args(r, NULL, GIBUS_TYPE_UINT32, &code, GIBUS_TYPE_INVALID);
    gibus_msg_free(r);
    if (!ok) {
        gibus__error_set(err, GIBUS_ERROR_FAILED, "RequestName answered without a code");
        return -1;
    }
    return (int)code;
}

int
gibus_add_match(gibus_conn *c, const char *rule, gibus_error *err)
{
    gibus_msg *r = bus_call(c, "AddMatch", rule, NULL, err);

    if (!r) return 0;
    gibus_msg_free(r);
    return 1;
}
