/* Ported from minidbus's marshal/ and message/ (MIT; see LICENSE). */

#include "gibus_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MESSAGE    (128u * 1024u * 1024u)
#define MAX_ARRAY      (64u * 1024u * 1024u)
#define MAX_DEPTH      64
#define MAX_TYPE_DEPTH 32

#define F_PATH         1
#define F_INTERFACE    2
#define F_MEMBER       3
#define F_ERROR_NAME   4
#define F_REPLY_SERIAL 5
#define F_DESTINATION  6
#define F_SENDER       7
#define F_SIGNATURE    8
#define F_UNIX_FDS     9

#define FLAG_NO_REPLY  0x1

#define COUNT(a)       (sizeof(a) / sizeof((a)[0]))

#define LOCAL_PATH     "/org/freedesktop/DBus/Local"
#define LOCAL_IFACE    "org.freedesktop.DBus.Local"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define HOST_BIG 1
#else
#define HOST_BIG 0
#endif

enum { MODE_NONE, MODE_READ, MODE_WRITE };
enum { CTX_TOP, CTX_ELEM, CTX_FIELD, CTX_KEY, CTX_VALUE };

typedef struct swapped {
    struct swapped *next;
    void           *data;
} swapped;

struct gibus_msg {
    int       type;
    int       flags;
    int       big;
    int       parsed;
    int       failed;
    int       dirty;
    int       open;
    uint32_t  serial;
    uint32_t  reply_serial;
    uint32_t  fds;
    char     *path;
    char     *iface;
    char     *member;
    char     *error_name;
    char     *dest;
    char     *sender;
    char     *names;
    char      sig[256];
    size_t    sig_len;
    uint8_t  *raw;
    size_t    raw_len;
    uint8_t  *body;
    size_t    body_len;
    size_t    body_cap;
    uint8_t  *wire;
    size_t    wire_len;
    swapped  *swapped;
};

#define RAW_AT ((sizeof(struct gibus_msg) + 15) & ~(size_t)15)

typedef struct reader {
    const uint8_t *b;
    size_t         len;
    size_t         pos;
    int            big;
    int            depth;
    uint32_t       fds;
} reader;

static uint64_t
get_n(const uint8_t *p, size_t n, int big)
{
    uint64_t v = 0;
    size_t i;

    for (i = 0; i < n; i++) v = v << 8 | p[big ? i : n - 1 - i];
    return v;
}

static uint16_t
get16(const uint8_t *p, int big)
{
    return (uint16_t)get_n(p, 2, big);
}

static uint32_t
get32(const uint8_t *p, int big)
{
    return (uint32_t)get_n(p, 4, big);
}

static uint64_t
get64(const uint8_t *p, int big)
{
    return get_n(p, 8, big);
}

static void
put32(uint8_t *p, uint32_t v, int big)
{
    int i;

    for (i = 0; i < 4; i++) p[big ? 3 - i : i] = (uint8_t)(v >> (8 * i));
}

static size_t
align_up(size_t at, size_t a)
{
    return (at + a - 1) & ~(a - 1);
}

static size_t
align_of(int c)
{
    switch (c) {
    case 'y': case 'g': case 'v':
        return 1;
    case 'n': case 'q':
        return 2;
    case 'x': case 't': case 'd': case '(': case '{': case 'r': case 'e':
        return 8;
    default:
        return 4;
    }
}

static size_t
fixed_size(int c)
{
    switch (c) {
    case 'y':
        return 1;
    case 'n': case 'q':
        return 2;
    case 'b': case 'i': case 'u': case 'h':
        return 4;
    case 'x': case 't': case 'd':
        return 8;
    default:
        return 0;
    }
}

static int
is_basic(int c)
{
    return fixed_size(c) || c == 's' || c == 'o' || c == 'g';
}

static int
utf8_valid(const void *bytes, size_t n)
{
    const uint8_t *s = (const uint8_t *)bytes;
    size_t i = 0, j, k;
    uint32_t cp, min;

    while (i < n) {
        uint8_t c = s[i];

        if (c < 0x80) {
            if (!c) return 0;
            while (++i < n && s[i] && s[i] < 0x80) {}
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            k = 1;
            min = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            k = 2;
            min = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            k = 3;
            min = 0x10000;
        } else {
            return 0;
        }
        cp = (uint32_t)(c & (0x3F >> k));
        if (n - i <= k) return 0;
        for (j = 1; j <= k; j++) {
            if ((s[i + j] & 0xC0) != 0x80) return 0;
            cp = cp << 6 | (s[i + j] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += k + 1;
    }
    return 1;
}

static int
alnum(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static int
digit(int c)
{
    return c >= '0' && c <= '9';
}

static int
valid_path(const char *s, size_t n)
{
    size_t i, elem = 0;

    if (n == 1) return s[0] == '/';
    if (!n || s[0] != '/') return 0;
    for (i = 1; i < n; i++) {
        if (s[i] == '/') {
            if (!elem) return 0;
            elem = 0;
        } else if (alnum(s[i]) || s[i] == '_') {
            elem++;
        } else {
            return 0;
        }
    }
    return elem > 0;
}

static int
valid_dotted(const char *s, size_t n, int digit_start, int dash)
{
    size_t i, elem = 0, parts = 1;

    if (!n || n > 255) return 0;
    for (i = 0; i < n; i++) {
        int c = (unsigned char)s[i];

        if (c == '.') {
            if (!elem) return 0;
            elem = 0;
            parts++;
            continue;
        }
        if (!alnum(c) && c != '_' && !(dash && c == '-')) return 0;
        if (!elem && !digit_start && digit(c)) return 0;
        elem++;
    }
    return elem && parts >= 2;
}

static int
valid_iface(const char *s, size_t n)
{
    return valid_dotted(s, n, 0, 0);
}

static int
valid_bus(const char *s, size_t n)
{
    if (n && s[0] == ':') return n <= 255 && valid_dotted(s + 1, n - 1, 1, 1);
    return valid_dotted(s, n, 0, 1);
}

static int
valid_member(const char *s, size_t n)
{
    size_t i;

    if (!n || n > 255 || digit(s[0])) return 0;
    for (i = 0; i < n; i++)
        if (!alnum(s[i]) && s[i] != '_') return 0;
    return 1;
}

static size_t
type_len(const char *s, size_t n, int ctx, int arrays, int structs)
{
    size_t k, v, pos, fields;

    if (!n || arrays > MAX_TYPE_DEPTH || structs > MAX_TYPE_DEPTH) return 0;
    switch (s[0]) {
    case 'y': case 'b': case 'n': case 'q': case 'i': case 'u': case 'x': case 't':
    case 'd': case 's': case 'o': case 'g': case 'h':
        return 1;
    case 'v':
        return ctx == CTX_KEY ? 0 : 1;
    case 'a':
        if (ctx == CTX_KEY) return 0;
        k = type_len(s + 1, n - 1, CTX_ELEM, arrays + 1, structs);
        return k ? k + 1 : 0;
    case '(':
        if (ctx == CTX_KEY) return 0;
        for (pos = 1, fields = 0; pos < n && s[pos] != ')'; pos += k, fields++)
            if (!(k = type_len(s + pos, n - pos, CTX_FIELD, arrays, structs + 1))) return 0;
        return pos < n && fields ? pos + 1 : 0;
    case '{':
        if (ctx != CTX_ELEM) return 0;
        if (!(k = type_len(s + 1, n - 1, CTX_KEY, arrays, structs))) return 0;
        if (!(v = type_len(s + 1 + k, n - 1 - k, CTX_VALUE, arrays, structs))) return 0;
        return 1 + k + v < n && s[1 + k + v] == '}' ? k + v + 2 : 0;
    default:
        return 0;
    }
}

static int
sig_valid(const char *s, size_t n)
{
    size_t at, k;

    if (n > 255) return 0;
    for (at = 0; at < n; at += k)
        if (!(k = type_len(s + at, n - at, CTX_TOP, 0, 0))) return 0;
    return 1;
}

static int
sig_single(const char *s, size_t n)
{
    return n && n <= 255 && type_len(s, n, CTX_TOP, 0, 0) == n;
}

static size_t
span(const char *s, size_t n)
{
    size_t i = 0;
    int depth = 0;

    while (i < n) {
        char c = s[i++];

        if (c == 'a') continue;
        if (c == '(' || c == '{') depth++;
        else if (c == ')' || c == '}') depth--;
        if (depth == 0) return i;
    }
    return 0;
}

static int
r_pad(reader *d, size_t a)
{
    size_t p = align_up(d->pos, a);

    if (p > d->len) return 0;
    for (; d->pos < p; d->pos++)
        if (d->b[d->pos]) return 0;
    return 1;
}

static int
r_take(reader *d, size_t n)
{
    if (n > d->len - d->pos) return 0;
    d->pos += n;
    return 1;
}

static int
r_u32(reader *d, uint32_t *v)
{
    if (!r_pad(d, 4) || d->len - d->pos < 4) return 0;
    *v = get32(d->b + d->pos, d->big);
    d->pos += 4;
    return 1;
}

static int
r_str_raw(reader *d, const char **s, size_t *len)
{
    const uint8_t *p;
    uint32_t n;

    if (!r_u32(d, &n) || n >= d->len - d->pos) return 0;
    p = d->b + d->pos;
    if (p[n]) return 0;
    d->pos += (size_t)n + 1;
    *s = (const char *)p;
    *len = n;
    return 1;
}

static int
r_str(reader *d)
{
    const char *s;
    size_t n;

    return r_str_raw(d, &s, &n) && utf8_valid(s, n);
}

static int
r_sig(reader *d, const char **s, size_t *len, int single)
{
    const uint8_t *p;
    size_t n;

    if (d->pos >= d->len) return 0;
    n = d->b[d->pos];
    if (n + 1 > d->len - d->pos - 1) return 0;
    p = d->b + d->pos + 1;
    if (p[n] || !(single ? sig_single((const char *)p, n) : sig_valid((const char *)p, n)))
        return 0;
    d->pos += n + 2;
    if (s) *s = (const char *)p;
    if (len) *len = n;
    return 1;
}

static int
skip_value(reader *d, const char *t, size_t tn)
{
    const char *s;
    size_t k, at, end, sz;
    uint32_t v;
    int ok;

    switch (t[0]) {
    case 'y':
        return r_take(d, 1);
    case 'b':
        return r_u32(d, &v) && v <= 1;
    case 'h':
        return r_u32(d, &v) && v < d->fds;
    case 'n': case 'q': case 'i': case 'u': case 'x': case 't': case 'd':
        sz = fixed_size(t[0]);
        return r_pad(d, sz) && r_take(d, sz);
    case 's':
        return r_str(d);
    case 'o':
        return r_str_raw(d, &s, &k) && valid_path(s, k);
    case 'g':
        return r_sig(d, NULL, NULL, 0);
    case 'v':
        if (!r_sig(d, &s, &k, 1) || ++d->depth > MAX_DEPTH) return 0;
        ok = skip_value(d, s, k);
        d->depth--;
        return ok;
    case 'a':
        if (tn < 2 || !r_u32(d, &v) || v > MAX_ARRAY || !r_pad(d, align_of(t[1])) ||
            v > d->len - d->pos)
            return 0;
        end = d->pos + v;
        sz = tn == 2 ? fixed_size(t[1]) : 0;
        if (sz && t[1] != 'b' && t[1] != 'h') {
            if (v % sz) return 0;
            d->pos = end;
            return 1;
        }
        if (!sz && v && ++d->depth > MAX_DEPTH) return 0;
        while (d->pos < end) {
            at = d->pos;
            if (!skip_value(d, t + 1, tn - 1) || d->pos == at) return 0;
        }
        if (!sz && v) d->depth--;
        return d->pos == end;
    case '(':
    case '{':
        if (!r_pad(d, 8) || ++d->depth > MAX_DEPTH) return 0;
        for (at = 1; at + 1 < tn; at += k)
            if (!(k = span(t + at, tn - 1 - at)) || !skip_value(d, t + at, k)) return 0;
        d->depth--;
        return 1;
    default:
        return 0;
    }
}

static int
step_value(reader *d, const char *t, size_t tn)
{
    size_t at, k, sz;
    uint32_t v;
    int ok;

    switch (t[0]) {
    case 'y': case 'b': case 'h': case 'n': case 'q': case 'i': case 'u': case 'x': case 't':
    case 'd':
        sz = fixed_size(t[0]);
        at = align_up(d->pos, sz);
        if (at > d->len || sz > d->len - at) return 0;
        d->pos = at + sz;
        return 1;
    case 's': case 'o':
        at = align_up(d->pos, 4);
        if (at > d->len || d->len - at < 4) return 0;
        v = get32(d->b + at, d->big);
        if (v >= d->len - at - 4) return 0;
        d->pos = at + 4 + (size_t)v + 1;
        return 1;
    case 'g':
        if (d->pos >= d->len || (size_t)d->b[d->pos] + 2 > d->len - d->pos) return 0;
        d->pos += (size_t)d->b[d->pos] + 2;
        return 1;
    case 'v':
        if (d->pos >= d->len) return 0;
        k = d->b[d->pos];
        if (!k || k + 2 > d->len - d->pos || ++d->depth > MAX_DEPTH) return 0;
        t = (const char *)d->b + d->pos + 1;
        d->pos += k + 2;
        ok = step_value(d, t, k);
        d->depth--;
        return ok;
    case 'a':
        at = align_up(d->pos, 4);
        if (tn < 2 || at > d->len || d->len - at < 4) return 0;
        v = get32(d->b + at, d->big);
        at = align_up(at + 4, align_of(t[1]));
        if (at > d->len || v > d->len - at) return 0;
        d->pos = at + v;
        return 1;
    case '(':
    case '{':
        at = align_up(d->pos, 8);
        if (at > d->len || ++d->depth > MAX_DEPTH) return 0;
        d->pos = at;
        for (at = 1; at + 1 < tn; at += k)
            if (!(k = span(t + at, tn - 1 - at)) || !step_value(d, t + at, k)) return 0;
        d->depth--;
        return 1;
    default:
        return 0;
    }
}

static void
copy_str(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);

    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void
gibus_error_init(gibus_error *e)
{
    if (e) memset(e, 0, sizeof(*e));
}

int
gibus_error_is_set(const gibus_error *e)
{
    return e && e->set;
}

void
gibus_error_free(gibus_error *e)
{
    gibus_error_init(e);
}

void
gibus__error_set(gibus_error *e, const char *name, const char *fmt, ...)
{
    va_list ap;

    if (!e || e->set) return;
    e->set = 1;
    copy_str(e->name, name ? name : GIBUS_ERROR_FAILED, sizeof(e->name));
    va_start(ap, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, ap);
    va_end(ap);
}

static int
fail(gibus_msg *m)
{
    if (m && !m->parsed) m->failed = 1;
    return 0;
}

static int
grow(gibus_msg *m, size_t extra)
{
    size_t need, cap;
    uint8_t *p;

    if (m->failed || m->parsed) return 0;
    if (extra > MAX_MESSAGE - m->body_len) return fail(m);
    need = m->body_len + extra;
    if (need > m->body_cap) {
        cap = m->body_cap ? m->body_cap : 256;
        while (cap < need) cap *= 2;
        if (!(p = (uint8_t *)realloc(m->body, cap))) return fail(m);
        m->body = p;
        m->body_cap = cap;
    }
    m->dirty = 1;
    return 1;
}

static int
w_pad(gibus_msg *m, size_t a)
{
    size_t n = align_up(m->body_len, a) - m->body_len;

    if (!grow(m, n)) return 0;
    if (n) memset(m->body + m->body_len, 0, n);
    m->body_len += n;
    return 1;
}

static uint8_t *
w_reserve(gibus_msg *m, size_t a, size_t n)
{
    size_t pad = align_up(m->body_len, a) - m->body_len;
    uint8_t *p;

    if (!grow(m, pad + n)) return NULL;
    p = m->body + m->body_len;
    if (pad) memset(p, 0, pad);
    m->body_len += pad + n;
    return p + pad;
}

static int
w_uint(gibus_msg *m, uint64_t v, size_t size)
{
    uint8_t *p = w_reserve(m, size, size);
    size_t i;

    if (!p) return 0;
    for (i = 0; i < size; i++) p[i] = (uint8_t)(v >> (8 * i));
    return 1;
}

static int
w_basic(gibus_msg *m, int type, const void *value)
{
    const char *s;
    uint64_t u = 0;
    uint32_t b;
    uint16_t h;
    size_t n;
    uint8_t *p;

    switch (type) {
    case 'y':
        return w_uint(m, *(const uint8_t *)value, 1);
    case 'b':
        memcpy(&b, value, 4);
        return w_uint(m, b ? 1 : 0, 4);
    case 'n': case 'q':
        memcpy(&h, value, 2);
        return w_uint(m, h, 2);
    case 'i': case 'u':
        memcpy(&b, value, 4);
        return w_uint(m, b, 4);
    case 'x': case 't': case 'd':
        memcpy(&u, value, 8);
        return w_uint(m, u, 8);
    case 's': case 'o':
        s = *(const char *const *)value;
        if (!s) return fail(m);
        n = strlen(s);
        if (n > MAX_MESSAGE || !(type == 's' ? utf8_valid(s, n) : valid_path(s, n))) return fail(m);
        if (!(p = w_reserve(m, 4, 4 + n + 1))) return 0;
        put32(p, (uint32_t)n, 0);
        memcpy(p + 4, s, n + 1);
        return 1;
    case 'g':
        s = *(const char *const *)value;
        if (!s) return fail(m);
        n = strlen(s);
        if (!sig_valid(s, n)) return fail(m);
        if (!(p = w_reserve(m, 1, n + 2))) return 0;
        p[0] = (uint8_t)n;
        memcpy(p + 1, s, n + 1);
        return 1;
    default:
        return fail(m);
    }
}

static size_t
name_size(const char *s)
{
    return s ? strlen(s) + 1 : 0;
}

static gibus_msg *
msg_new(int type, size_t names)
{
    gibus_msg *m = (gibus_msg *)calloc(1, sizeof(*m) + names);

    if (m) {
        m->type = type;
        m->dirty = 1;
        m->names = (char *)(m + 1);
    }
    return m;
}

static void
put_name(gibus_msg *m, char **slot, const char *s, size_t n)
{
    memcpy(m->names, s, n + 1);
    *slot = m->names;
    m->names += n + 1;
}

static void
set_name(gibus_msg *m, char **slot, const char *s, int (*ok)(const char *, size_t))
{
    size_t n;

    if (!s || !ok(s, n = strlen(s))) fail(m);
    else put_name(m, slot, s, n);
}

static void
reply_to(gibus_msg *m, const gibus_msg *call)
{
    if (call->type != GIBUS_MESSAGE_METHOD_CALL || !call->serial) fail(m);
    m->reply_serial = call->serial;
    if (call->sender) put_name(m, &m->dest, call->sender, strlen(call->sender));
}

gibus_msg *
gibus_msg_call(const char *dest, const char *path, const char *iface, const char *member)
{
    gibus_msg *m = msg_new(GIBUS_MESSAGE_METHOD_CALL, name_size(dest) + name_size(path) +
                                                          name_size(iface) + name_size(member));

    if (!m) return NULL;
    if (dest) set_name(m, &m->dest, dest, valid_bus);
    set_name(m, &m->path, path, valid_path);
    if (iface) set_name(m, &m->iface, iface, valid_iface);
    set_name(m, &m->member, member, valid_member);
    if ((path && !strcmp(path, LOCAL_PATH)) || (iface && !strcmp(iface, LOCAL_IFACE)))
        fail(m);
    return m;
}

gibus_msg *
gibus_msg_signal(const char *path, const char *iface, const char *member)
{
    gibus_msg *m =
        msg_new(GIBUS_MESSAGE_SIGNAL, name_size(path) + name_size(iface) + name_size(member));

    if (!m) return NULL;
    set_name(m, &m->path, path, valid_path);
    set_name(m, &m->iface, iface, valid_iface);
    set_name(m, &m->member, member, valid_member);
    if ((path && !strcmp(path, LOCAL_PATH)) || (iface && !strcmp(iface, LOCAL_IFACE)))
        fail(m);
    return m;
}

gibus_msg *
gibus_msg_return(const gibus_msg *call)
{
    gibus_msg *m;

    if (!call || !(m = msg_new(GIBUS_MESSAGE_METHOD_RETURN, name_size(call->sender))))
        return NULL;
    reply_to(m, call);
    return m;
}

gibus_msg *
gibus_msg_error(const gibus_msg *call, const char *name, const char *text)
{
    gibus_msg *m;

    if (!call || !(m = msg_new(GIBUS_MESSAGE_ERROR, name_size(call->sender) + name_size(name))))
        return NULL;
    reply_to(m, call);
    set_name(m, &m->error_name, name, valid_iface);
    if (text) gibus_msg_append_args(m, GIBUS_TYPE_STRING, &text, GIBUS_TYPE_INVALID);
    return m;
}

void
gibus_msg_free(gibus_msg *m)
{
    swapped *s, *next;

    if (!m) return;
    if (!m->parsed) free(m->body);
    for (s = m->swapped; s; s = next) {
        next = s->next;
        free(s->data);
        free(s);
    }
    free(m->wire);
    free(m);
}

int
gibus_msg_type(const gibus_msg *m)
{
    return m ? m->type : 0;
}

uint32_t
gibus_msg_serial(const gibus_msg *m)
{
    return m ? m->serial : 0;
}

uint32_t
gibus_msg_reply_serial(const gibus_msg *m)
{
    return m ? m->reply_serial : 0;
}

const char *
gibus_msg_path(const gibus_msg *m)
{
    return m ? m->path : NULL;
}

const char *
gibus_msg_interface(const gibus_msg *m)
{
    return m ? m->iface : NULL;
}

const char *
gibus_msg_member(const gibus_msg *m)
{
    return m ? m->member : NULL;
}

const char *
gibus_msg_error_name(const gibus_msg *m)
{
    return m ? m->error_name : NULL;
}

const char *
gibus_msg_sender(const gibus_msg *m)
{
    return m ? m->sender : NULL;
}

const char *
gibus_msg_destination(const gibus_msg *m)
{
    return m ? m->dest : NULL;
}

const char *
gibus_msg_signature(const gibus_msg *m)
{
    return m ? m->sig : "";
}

int
gibus__msg_no_reply(const gibus_msg *m)
{
    return m && (m->flags & FLAG_NO_REPLY);
}

static int
same(const char *a, const char *b)
{
    return a && b && !strcmp(a, b);
}

int
gibus_msg_is_call(const gibus_msg *m, const char *iface, const char *member)
{
    return m && m->type == GIBUS_MESSAGE_METHOD_CALL && same(m->member, member) &&
           (!iface || !m->iface || same(m->iface, iface));
}

int
gibus_msg_is_signal(const gibus_msg *m, const char *iface, const char *member)
{
    return m && m->type == GIBUS_MESSAGE_SIGNAL && same(m->member, member) &&
           (!iface || !m->iface || same(m->iface, iface));
}

int
gibus_msg_has_member(const gibus_msg *m, const char *member)
{
    return m && same(m->member, member);
}

int
gibus_msg_has_signature(const gibus_msg *m, const char *sig)
{
    return m && sig && !strcmp(m->sig, sig);
}

static void
iter_reset(gibus_iter *it, gibus_msg *m, int mode)
{
    it->msg = m;
    it->mode = mode;
    it->container = it->failed = it->depth = 0;
    it->pos = it->end = it->len_at = it->first = 0;
    it->sig[0] = 0;
    it->sig_at = it->sig_len = 0;
}

static int
it_fail(gibus_iter *it)
{
    it->failed = 1;
    if (it->mode == MODE_WRITE) fail(it->msg);
    return 0;
}

static int
writable(const gibus_iter *it)
{
    return it->mode == MODE_WRITE && it->msg && !it->failed && !it->msg->failed &&
           !it->msg->parsed && it->msg->open == it->depth;
}

static int
take_type(gibus_iter *it, const char *t, size_t n)
{
    gibus_msg *m = it->msg;

    switch (it->container) {
    case 0:
        if (n > sizeof(m->sig) - 1 - m->sig_len || type_len(t, n, CTX_TOP, 0, 0) != n)
            return it_fail(it);
        memcpy(m->sig + m->sig_len, t, n);
        m->sig_len += n;
        m->sig[m->sig_len] = 0;
        m->dirty = 1;
        return 1;
    case GIBUS_TYPE_ARRAY:
        return n == it->sig_len && !memcmp(t, it->sig, n) ? 1 : it_fail(it);
    case GIBUS_TYPE_VARIANT:
        if (it->pos || n != it->sig_len || memcmp(t, it->sig, n)) return it_fail(it);
        it->pos = 1;
        return 1;
    default:
        if (n > sizeof(it->sig) - 3 - it->sig_len) return it_fail(it);
        memcpy(it->sig + it->sig_len, t, n);
        it->sig_len += n;
        it->sig[it->sig_len] = 0;
        return 1;
    }
}

void
gibus_iter_init_append(gibus_msg *m, gibus_iter *it)
{
    if (!it) return;
    memset(it, 0, sizeof(*it));
    it->msg = m;
    it->mode = MODE_WRITE;
    if (!m || m->parsed) it->failed = 1;
}

int
gibus_iter_append_basic(gibus_iter *it, int type, const void *value)
{
    char t = (char)type;

    if (!it) return 0;
    if (!writable(it) || !value || !is_basic(type) || type == 'h' ||
        (it->depth > MAX_DEPTH && !fixed_size(type)) || !w_basic(it->msg, type, value))
        return it_fail(it);
    return take_type(it, &t, 1);
}

int
gibus_iter_open(gibus_iter *it, int type, const char *sig, gibus_iter *sub)
{
    gibus_msg *m;
    size_t n;
    uint8_t *p;

    if (!it) return 0;
    if (!sub) return it_fail(it);
    iter_reset(sub, m = it->msg, MODE_WRITE);
    sub->failed = 1;
    sub->depth = it->depth + 1;
    if (!writable(it) || sub->depth > MAX_DEPTH + (type == GIBUS_TYPE_ARRAY))
        return it_fail(it);
    switch (type) {
    case GIBUS_TYPE_ARRAY:
    case GIBUS_TYPE_VARIANT:
        n = sig ? strlen(sig) : 0;
        if (type == GIBUS_TYPE_ARRAY ? !n || n > 254 || type_len(sig, n, CTX_ELEM, 1, 0) != n
                                     : !sig_single(sig, n))
            return it_fail(it);
        memcpy(sub->sig, sig, n + 1);
        sub->sig_len = n;
        if (type == GIBUS_TYPE_ARRAY) {
            if (!w_pad(m, 4)) return it_fail(it);
            sub->len_at = m->body_len;
            if (!w_uint(m, 0, 4) || !w_pad(m, align_of(sig[0]))) return it_fail(it);
            sub->first = m->body_len;
        } else {
            if (!(p = w_reserve(m, 1, n + 2))) return it_fail(it);
            p[0] = (uint8_t)n;
            memcpy(p + 1, sig, n + 1);
        }
        break;
    case GIBUS_TYPE_DICT_ENTRY:
        if (it->container != GIBUS_TYPE_ARRAY) return it_fail(it);
        if (!w_pad(m, 8)) return it_fail(it);
        break;
    case GIBUS_TYPE_STRUCT:
        if (!w_pad(m, 8)) return it_fail(it);
        break;
    default:
        return it_fail(it);
    }
    sub->container = type;
    sub->failed = 0;
    m->open = sub->depth;
    return 1;
}

int
gibus_iter_close(gibus_iter *it, gibus_iter *sub)
{
    gibus_msg *m;
    char t[258];
    size_t n;

    if (!it) return 0;
    m = it->msg;
    if (!sub || it->mode != MODE_WRITE || sub->mode != MODE_WRITE || !m || sub->msg != m ||
        sub->depth != it->depth + 1 || it->failed || sub->failed || m->failed || m->parsed ||
        m->open != sub->depth)
        return it_fail(it);
    m->open = it->depth;
    sub->mode = MODE_NONE;
    switch (sub->container) {
    case GIBUS_TYPE_ARRAY:
        n = m->body_len - sub->first;
        if (n > MAX_ARRAY) return it_fail(it);
        put32(m->body + sub->len_at, (uint32_t)n, 0);
        t[0] = 'a';
        memcpy(t + 1, sub->sig, sub->sig_len);
        n = sub->sig_len + 1;
        break;
    case GIBUS_TYPE_VARIANT:
        if (!sub->pos) return it_fail(it);
        t[0] = 'v';
        n = 1;
        break;
    case GIBUS_TYPE_STRUCT:
    case GIBUS_TYPE_DICT_ENTRY:
        if (!sub->sig_len) return it_fail(it);
        t[0] = sub->container == GIBUS_TYPE_STRUCT ? '(' : '{';
        memcpy(t + 1, sub->sig, sub->sig_len);
        t[sub->sig_len + 1] = sub->container == GIBUS_TYPE_STRUCT ? ')' : '}';
        n = sub->sig_len + 2;
        break;
    default:
        return it_fail(it);
    }
    return take_type(it, t, n);
}

int
gibus_iter_append_fixed_array(gibus_iter *it, int type, const void *elems, int n)
{
    const uint8_t *p = (const uint8_t *)elems;
    size_t sz = fixed_size(type), i;
    uint64_t v = 0;
    uint32_t v32;
    uint16_t v16;
    gibus_msg *m;
    char t = (char)type;

    if (!it) return 0;
    if (!writable(it) || it->container != GIBUS_TYPE_ARRAY || it->sig_len != 1 ||
        it->sig[0] != type || !sz || type == 'h' || n < 0 || (n && !elems) ||
        (size_t)n > MAX_ARRAY / sz)
        return it_fail(it);
    m = it->msg;
    if (!w_pad(m, sz) || !grow(m, (size_t)n * sz)) return it_fail(it);
    if (!HOST_BIG && type != 'b') {
        if (n) memcpy(m->body + m->body_len, p, (size_t)n * sz);
        m->body_len += (size_t)n * sz;
        return take_type(it, &t, 1);
    }
    for (i = 0; i < (size_t)n; i++, p += sz) {
        switch (sz) {
        case 1:
            v = p[0];
            break;
        case 2:
            memcpy(&v16, p, 2);
            v = v16;
            break;
        case 4:
            memcpy(&v32, p, 4);
            v = type == 'b' ? (v32 ? 1 : 0) : v32;
            break;
        default:
            memcpy(&v, p, 8);
            break;
        }
        if (!w_uint(m, v, sz)) return it_fail(it);
    }
    return take_type(it, &t, 1);
}

int
gibus_msg_append_args(gibus_msg *m, int type, ...)
{
    gibus_iter it;
    va_list ap;
    int ok = 1;

    gibus_iter_init_append(m, &it);
    va_start(ap, type);
    while (ok && type != GIBUS_TYPE_INVALID) {
        const void *value = va_arg(ap, const void *);

        ok = gibus_iter_append_basic(&it, type, value);
        type = va_arg(ap, int);
    }
    va_end(ap);
    return ok && !it.failed;
}

int
gibus_iter_init(gibus_msg *m, gibus_iter *it)
{
    if (!it) return 0;
    memset(it, 0, sizeof(*it));
    it->msg = m;
    it->mode = MODE_READ;
    if (!m || m->failed || m->open) {
        it->failed = 1;
        return 0;
    }
    memcpy(it->sig, m->sig, m->sig_len + 1);
    it->sig_len = m->sig_len;
    it->end = m->body_len;
    return it->sig_len > 0;
}

int
gibus_iter_type(const gibus_iter *it)
{
    int c;

    if (!it || it->mode != MODE_READ || it->failed || !it->msg || it->sig_at >= it->sig_len)
        return GIBUS_TYPE_INVALID;
    if (it->container == GIBUS_TYPE_ARRAY && it->pos >= it->end) return GIBUS_TYPE_INVALID;
    c = it->sig[it->sig_at];
    if (c == '(') return GIBUS_TYPE_STRUCT;
    if (c == '{') return GIBUS_TYPE_DICT_ENTRY;
    return c;
}

void
gibus_iter_get_basic(const gibus_iter *it, void *value)
{
    const uint8_t *p;
    size_t len, at, sz;
    uint64_t u;
    uint32_t u32;
    uint16_t u16;
    int t = gibus_iter_type(it), big;

    if (!value || !is_basic(t)) return;
    len = it->msg->body_len;
    big = it->msg->big;
    at = align_up(it->pos, align_of(t));
    sz = fixed_size(t);
    if (at > len || (sz ? sz : t == 'g' ? 1 : 4) > len - at) return;
    p = it->msg->body + at;
    switch (t) {
    case 's': case 'o':
        u32 = get32(p, big);
        if (u32 >= len - at - 4 || p[4 + u32]) return;
        *(const char **)value = (const char *)p + 4;
        return;
    case 'g':
        if ((size_t)p[0] + 1 >= len - at || p[1 + p[0]]) return;
        *(const char **)value = (const char *)p + 1;
        return;
    default:
        break;
    }
    switch (sz) {
    case 1:
        *(uint8_t *)value = p[0];
        break;
    case 2:
        u16 = get16(p, big);
        memcpy(value, &u16, 2);
        break;
    case 4:
        u32 = get32(p, big);
        memcpy(value, &u32, 4);
        break;
    default:
        u = get64(p, big);
        memcpy(value, &u, 8);
        break;
    }
}

static void
set_sig(gibus_iter *it, const char *s, size_t n)
{
    memcpy(it->sig, s, n);
    it->sig[n] = 0;
    it->sig_len = n;
}

static int
array_at(const gibus_iter *it, size_t *start, size_t *n)
{
    size_t len = it->msg->body_len, at = align_up(it->pos, 4);

    if (at > len || len - at < 4 || it->sig_at + 1 >= it->sig_len) return 0;
    *n = get32(it->msg->body + at, it->msg->big);
    at = align_up(at + 4, align_of(it->sig[it->sig_at + 1]));
    if (at > len || *n > len - at) return 0;
    *start = at;
    return 1;
}

void
gibus_iter_recurse(const gibus_iter *it, gibus_iter *sub)
{
    const uint8_t *b;
    size_t len, at, n, tl;
    int t = gibus_iter_type(it);

    if (!sub) return;
    iter_reset(sub, NULL, MODE_READ);
    sub->failed = 1;
    if (!t) return;
    sub->msg = it->msg;
    b = it->msg->body;
    len = it->msg->body_len;
    tl = span(it->sig + it->sig_at, it->sig_len - it->sig_at);
    switch (t) {
    case GIBUS_TYPE_ARRAY:
        if (tl < 2 || !array_at(it, &at, &n)) return;
        set_sig(sub, it->sig + it->sig_at + 1, tl - 1);
        sub->pos = at;
        sub->end = at + n;
        break;
    case GIBUS_TYPE_STRUCT:
    case GIBUS_TYPE_DICT_ENTRY:
        at = align_up(it->pos, 8);
        if (tl < 3 || at > len) return;
        set_sig(sub, it->sig + it->sig_at + 1, tl - 2);
        sub->pos = at;
        sub->end = len;
        break;
    case GIBUS_TYPE_VARIANT:
        at = it->pos;
        if (at >= len) return;
        n = b[at];
        if (n + 2 > len - at) return;
        set_sig(sub, (const char *)b + at + 1, n);
        sub->pos = at + n + 2;
        sub->end = len;
        break;
    default:
        return;
    }
    sub->container = t;
    sub->depth = it->depth + 1;
    sub->failed = 0;
}

int
gibus_iter_next(gibus_iter *it)
{
    reader d;
    size_t tl;

    if (!gibus_iter_type(it)) return 0;
    tl = span(it->sig + it->sig_at, it->sig_len - it->sig_at);
    d.b = it->msg->body;
    d.len = it->msg->body_len;
    d.pos = it->pos;
    d.big = it->msg->big;
    d.depth = 0;
    d.fds = 0;
    if (!tl || !step_value(&d, it->sig + it->sig_at, tl)) {
        it->failed = 1;
        return 0;
    }
    it->pos = d.pos;
    if (it->container != GIBUS_TYPE_ARRAY) it->sig_at += tl;
    return gibus_iter_type(it) != GIBUS_TYPE_INVALID;
}

static const void *
native_copy(gibus_msg *m, const uint8_t *p, size_t n, size_t sz)
{
    swapped *s;
    uint8_t *q;
    size_t i, j;

    if (!(s = (swapped *)malloc(sizeof(*s)))) return NULL;
    if (!(s->data = malloc(n * sz))) {
        free(s);
        return NULL;
    }
    q = (uint8_t *)s->data;
    for (i = 0; i < n; i++)
        for (j = 0; j < sz; j++) q[i * sz + j] = p[i * sz + sz - 1 - j];
    s->next = m->swapped;
    m->swapped = s;
    return q;
}

int
gibus_msg_get_args(gibus_msg *m, gibus_error *err, int type, ...)
{
    gibus_iter it;
    va_list ap;
    int argn = 0, ok = 1;

    gibus_iter_init(m, &it);
    va_start(ap, type);
    while (type != GIBUS_TYPE_INVALID) {
        int have = gibus_iter_type(&it);

        if (type == GIBUS_TYPE_ARRAY) {
            int elem = va_arg(ap, int);
            void *out = va_arg(ap, void *);
            int *count = va_arg(ap, int *);
            size_t sz = fixed_size(elem), start, n;
            const void *p;

            if (have != GIBUS_TYPE_ARRAY || !sz || elem == 'h' || it.sig_at + 1 >= it.sig_len ||
                it.sig[it.sig_at + 1] != elem) {
                gibus__error_set(err, GIBUS_ERROR_INVALID_ARGS,
                                 "argument %d is not an array of '%c'", argn, elem);
                ok = 0;
                break;
            }
            if (!array_at(&it, &start, &n)) {
                gibus__error_set(err, GIBUS_ERROR_INVALID_ARGS, "argument %d is malformed", argn);
                ok = 0;
                break;
            }
            p = m->body + start;
            n /= sz;
            if (m->big != HOST_BIG && sz > 1 && n && !(p = native_copy(m, m->body + start, n, sz))) {
                gibus__error_set(err, GIBUS_ERROR_FAILED, "out of memory");
                ok = 0;
                break;
            }
            if (out) memcpy(out, &p, sizeof(p));
            if (count) *count = (int)n;
        } else {
            void *out = va_arg(ap, void *);

            if (!is_basic(type) || type == 'h' || have != type) {
                if (have == GIBUS_TYPE_INVALID)
                    gibus__error_set(err, GIBUS_ERROR_INVALID_ARGS,
                                     "message has %d arguments, expected more", argn);
                else
                    gibus__error_set(err, GIBUS_ERROR_INVALID_ARGS,
                                     "argument %d is '%c', expected '%c'", argn, have, type);
                ok = 0;
                break;
            }
            gibus_iter_get_basic(&it, out);
        }
        gibus_iter_next(&it);
        argn++;
        type = va_arg(ap, int);
    }
    va_end(ap);
    return ok;
}

long
gibus__frame_size(const uint8_t *b, size_t have)
{
    uint64_t total;
    uint32_t fields;
    int big;

    if (have < 16) return 0;
    if (!b || (b[0] != 'l' && b[0] != 'B') || !b[1] || b[3] != 1) return -1;
    big = b[0] == 'B';
    if (!get32(b + 8, big)) return -1;
    fields = get32(b + 12, big);
    if (fields > MAX_ARRAY) return -1;
    total = align_up(16 + (size_t)fields, 8) + (uint64_t)get32(b + 4, big);
    if (total > MAX_MESSAGE) return -1;
    return (long)total;
}

static int
field_str(reader *d, const char *vs, size_t vn, char want, char **out,
          int (*ok)(const char *, size_t))
{
    const char *s;
    size_t n;

    if (vn != 1 || vs[0] != want || !r_str_raw(d, &s, &n) || !ok(s, n)) return 0;
    *out = (char *)s;
    return 1;
}

static int
parse_fields(gibus_msg *m, size_t fields_end)
{
    unsigned char seen[256];
    const char *vs, *s;
    size_t vn, n;
    reader d;
    int code;

    memset(seen, 0, sizeof(seen));
    d.b = m->raw;
    d.len = fields_end;
    d.pos = 16;
    d.big = m->big;
    d.depth = 0;
    d.fds = UINT32_MAX;
    while (d.pos < d.len) {
        if (!r_pad(&d, 8) || d.pos >= d.len) return 0;
        code = d.b[d.pos++];
        if (!code || seen[code]) return 0;
        seen[code] = 1;
        if (!r_sig(&d, &vs, &vn, 1)) return 0;
        switch (code) {
        case F_PATH:
            if (!field_str(&d, vs, vn, 'o', &m->path, valid_path)) return 0;
            break;
        case F_INTERFACE:
            if (!field_str(&d, vs, vn, 's', &m->iface, valid_iface)) return 0;
            break;
        case F_MEMBER:
            if (!field_str(&d, vs, vn, 's', &m->member, valid_member)) return 0;
            break;
        case F_ERROR_NAME:
            if (!field_str(&d, vs, vn, 's', &m->error_name, valid_iface)) return 0;
            break;
        case F_DESTINATION:
            if (!field_str(&d, vs, vn, 's', &m->dest, valid_bus)) return 0;
            break;
        case F_SENDER:
            if (!field_str(&d, vs, vn, 's', &m->sender, valid_bus)) return 0;
            break;
        case F_REPLY_SERIAL:
            if (vn != 1 || vs[0] != 'u' || !r_u32(&d, &m->reply_serial) || !m->reply_serial)
                return 0;
            break;
        case F_UNIX_FDS:
            if (vn != 1 || vs[0] != 'u' || !r_u32(&d, &m->fds)) return 0;
            break;
        case F_SIGNATURE:
            if (vn != 1 || vs[0] != 'g' || !r_sig(&d, &s, &n, 0)) return 0;
            memcpy(m->sig, s, n);
            m->sig[n] = 0;
            m->sig_len = n;
            break;
        default:
            if (!skip_value(&d, vs, vn)) return 0;
            break;
        }
    }
    return 1;
}

static int
check_fields(gibus_msg *m)
{
    switch (m->type) {
    case GIBUS_MESSAGE_METHOD_CALL:
        m->reply_serial = 0;
        m->error_name = NULL;
        return m->path && m->member;
    case GIBUS_MESSAGE_SIGNAL:
        m->reply_serial = 0;
        m->error_name = NULL;
        return m->path && m->iface && m->member;
    case GIBUS_MESSAGE_METHOD_RETURN:
        m->error_name = NULL;
        m->path = m->iface = m->member = NULL;
        return m->reply_serial != 0;
    case GIBUS_MESSAGE_ERROR:
        m->path = m->iface = m->member = NULL;
        return m->reply_serial && m->error_name;
    default:
        return 0;
    }
}

static int
check_body(const gibus_msg *m)
{
    reader d;
    size_t at, k;

    if (m->body_len && !m->sig_len) return 0;
    d.b = m->body;
    d.len = m->body_len;
    d.pos = 0;
    d.big = m->big;
    d.depth = 0;
    d.fds = m->fds;
    for (at = 0; at < m->sig_len; at += k)
        if (!(k = type_len(m->sig + at, m->sig_len - at, CTX_TOP, 0, 0)) ||
            !skip_value(&d, m->sig + at, k))
            return 0;
    return d.pos == d.len;
}

gibus_msg *
gibus_msg_parse(const void *bytes, size_t len)
{
    const uint8_t *in = (const uint8_t *)bytes;
    size_t fields_end, body_start, i;
    gibus_msg *m;
    long total;

    if (!in) return NULL;
    total = gibus__frame_size(in, len);
    if (total <= 0 || (size_t)total != len || in[1] > GIBUS_MESSAGE_SIGNAL) return NULL;
    if (!(m = (gibus_msg *)malloc(RAW_AT + len))) return NULL;
    memset(m, 0, sizeof(*m));
    m->raw = (uint8_t *)m + RAW_AT;
    memcpy(m->raw, in, len);
    m->raw_len = len;
    m->parsed = 1;
    m->type = in[1];
    m->flags = in[2];
    m->big = in[0] == 'B';
    m->serial = get32(in + 8, m->big);
    fields_end = 16 + (size_t)get32(in + 12, m->big);
    body_start = align_up(fields_end, 8);
    m->body = m->raw + body_start;
    m->body_len = len - body_start;
    for (i = fields_end; i < body_start; i++)
        if (m->raw[i]) goto bad;
    if (!parse_fields(m, fields_end) || !check_fields(m) || !check_body(m)) goto bad;
    return m;
bad:
    gibus_msg_free(m);
    return NULL;
}

static size_t
w_field(uint8_t *w, size_t at, int code, int t, const char *s, uint32_t u)
{
    size_t n;

    while (at % 8) w[at++] = 0;
    w[at++] = (uint8_t)code;
    w[at++] = 1;
    w[at++] = (uint8_t)t;
    w[at++] = 0;
    if (t == 'u') {
        put32(w + at, u, 0);
        return at + 4;
    }
    n = strlen(s);
    if (t == 'g') {
        w[at++] = (uint8_t)n;
    } else {
        put32(w + at, (uint32_t)n, 0);
        at += 4;
    }
    memcpy(w + at, s, n + 1);
    return at + n + 1;
}

static int
build_wire(gibus_msg *m)
{
    const char *str[6];
    size_t cap, at, i, n;
    uint8_t *w;

    str[0] = m->path;
    str[1] = m->iface;
    str[2] = m->member;
    str[3] = m->error_name;
    str[4] = m->dest;
    str[5] = m->sender;
    cap = 16 + 16 + 272 + 8 + m->body_len;
    for (i = 0; i < COUNT(str); i++) {
        if (!str[i]) continue;
        if ((n = strlen(str[i])) > MAX_MESSAGE) return 0;
        cap += n + 16;
    }
    if (!(w = (uint8_t *)malloc(cap))) return 0;
    w[0] = 'l';
    w[1] = (uint8_t)m->type;
    w[2] = (uint8_t)m->flags;
    w[3] = 1;
    put32(w + 4, (uint32_t)m->body_len, 0);
    put32(w + 8, 0, 0);
    at = 16;
    if (m->path) at = w_field(w, at, F_PATH, 'o', m->path, 0);
    if (m->iface) at = w_field(w, at, F_INTERFACE, 's', m->iface, 0);
    if (m->member) at = w_field(w, at, F_MEMBER, 's', m->member, 0);
    if (m->error_name) at = w_field(w, at, F_ERROR_NAME, 's', m->error_name, 0);
    if (m->reply_serial) at = w_field(w, at, F_REPLY_SERIAL, 'u', NULL, m->reply_serial);
    if (m->dest) at = w_field(w, at, F_DESTINATION, 's', m->dest, 0);
    if (m->sender) at = w_field(w, at, F_SENDER, 's', m->sender, 0);
    if (m->sig_len) at = w_field(w, at, F_SIGNATURE, 'g', m->sig, 0);
    if (at - 16 > MAX_ARRAY) {
        free(w);
        return 0;
    }
    put32(w + 12, (uint32_t)(at - 16), 0);
    while (at % 8) w[at++] = 0;
    if (m->body_len > MAX_MESSAGE - at) {
        free(w);
        return 0;
    }
    if (m->body_len) memcpy(w + at, m->body, m->body_len);
    free(m->wire);
    m->wire = w;
    m->wire_len = at + m->body_len;
    return 1;
}

const uint8_t *
gibus_msg_bytes(gibus_msg *m, uint32_t serial, size_t *len)
{
    if (len) *len = 0;
    if (!m || m->failed || m->open || !serial) return NULL;
    if (m->parsed) {
        put32(m->raw + 8, serial, m->big);
        m->serial = serial;
        if (len) *len = m->raw_len;
        return m->raw;
    }
    if (m->dirty || !m->wire) {
        if (!build_wire(m)) {
            m->failed = 1;
            return NULL;
        }
        m->dirty = 0;
    }
    put32(m->wire + 8, serial, 0);
    m->serial = serial;
    if (len) *len = m->wire_len;
    return m->wire;
}
