#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gibus_internal.h"
#include "test.h"

static int g_require_bus;

#ifdef GIBUSTEST_LIVE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define ok test_ok

#define FRAME_CAP 4096
#define DUMP_CAP  8192
#define BIG_LEN   (1024 * 1024)

typedef struct wr {
    uint8_t *b;
    size_t   n, cap;
    int      big;
} wr;

typedef struct warr {
    size_t len_at, start;
} warr;

static void
w_init(wr *w, uint8_t *buf, size_t cap, int big)
{
    w->b = buf;
    w->n = 0;
    w->cap = cap;
    w->big = big;
}

static void
w_u8(wr *w, unsigned v)
{
    if (w->n < w->cap) w->b[w->n] = (uint8_t)v;
    w->n++;
}

static void
w_pad(wr *w, size_t align)
{
    while (w->n % align) w_u8(w, 0);
}

static void
w_uint(wr *w, uint64_t v, int size)
{
    int i;

    w_pad(w, (size_t)size);
    for (i = 0; i < size; i++) {
        int shift = w->big ? (size - 1 - i) * 8 : i * 8;
        w_u8(w, (unsigned)((v >> shift) & 0xff));
    }
}

static void
w_patch32(wr *w, size_t at, uint32_t v)
{
    int i;

    if (at + 4 > w->cap) return;
    for (i = 0; i < 4; i++) w->b[at + (size_t)i] = (uint8_t)(v >> (w->big ? (3 - i) * 8 : i * 8));
}

static void
w_str(wr *w, const char *s)
{
    size_t i, n = strlen(s);

    w_uint(w, n, 4);
    for (i = 0; i < n; i++) w_u8(w, (unsigned char)s[i]);
    w_u8(w, 0);
}

static void
w_sig(wr *w, const char *s)
{
    size_t i, n = strlen(s);

    w_u8(w, (unsigned)n);
    for (i = 0; i < n; i++) w_u8(w, (unsigned char)s[i]);
    w_u8(w, 0);
}

static warr
w_open(wr *w, size_t align)
{
    warr a;

    w_uint(w, 0, 4);
    a.len_at = w->n - 4;
    w_pad(w, align);
    a.start = w->n;
    return a;
}

static void
w_close(wr *w, warr a)
{
    w_patch32(w, a.len_at, (uint32_t)(w->n - a.start));
}

static uint64_t
dbits(double d)
{
    uint64_t u;

    memcpy(&u, &d, sizeof(u));
    return u;
}

static size_t
copy_str(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src), k;

    if (cap) {
        k = n < cap - 1 ? n : cap - 1;
        memcpy(dst, src, k);
        dst[k] = 0;
    }
    return n;
}

typedef struct field {
    int         code;
    int         type;
    const char *s;
    uint32_t    u;
} field;

#define FP(s)  { 1, 'o', s, 0 }
#define FI(s)  { 2, 's', s, 0 }
#define FM(s)  { 3, 's', s, 0 }
#define FE(s)  { 4, 's', s, 0 }
#define FR(u)  { 5, 'u', NULL, u }
#define FD(s)  { 6, 's', s, 0 }
#define FS(s)  { 7, 's', s, 0 }
#define FG(s)  { 8, 'g', s, 0 }

typedef struct frame {
    uint8_t b[FRAME_CAP];
    size_t  n, fields_end, body_at, first_end;
} frame;

static void
make_frame(frame *f, int big, int type, int flags, uint32_t serial, const field *fl, int nf,
           const uint8_t *body, size_t body_len)
{
    wr w;
    size_t k;
    int i;

    w_init(&w, f->b, sizeof(f->b), big);
    w_u8(&w, big ? 'B' : 'l');
    w_u8(&w, (unsigned)type);
    w_u8(&w, (unsigned)flags);
    w_u8(&w, 1);
    w_uint(&w, body_len, 4);
    w_uint(&w, serial, 4);
    w_uint(&w, 0, 4);
    f->first_end = 0;
    for (i = 0; i < nf; i++) {
        w_pad(&w, 8);
        w_u8(&w, (unsigned)fl[i].code);
        w_u8(&w, 1);
        w_u8(&w, (unsigned)fl[i].type);
        w_u8(&w, 0);
        if (fl[i].type == 'u' || fl[i].type == 'i')
            w_uint(&w, fl[i].u, 4);
        else if (fl[i].type == 'y')
            w_u8(&w, fl[i].u);
        else if (fl[i].type == 'g')
            w_sig(&w, fl[i].s);
        else
            w_str(&w, fl[i].s);
        if (i == 0) f->first_end = w.n;
    }
    f->fields_end = w.n;
    w_patch32(&w, 12, (uint32_t)(w.n - 16));
    w_pad(&w, 8);
    f->body_at = w.n;
    for (k = 0; k < body_len; k++) w_u8(&w, body[k]);
    f->n = w.n > sizeof(f->b) ? sizeof(f->b) : w.n;
}

typedef struct dumpbuf {
    char  *b;
    size_t cap, n;
} dumpbuf;

static void
put(dumpbuf *o, const char *fmt, ...)
{
    va_list ap;
    int k;

    if (o->n + 1 >= o->cap) return;
    va_start(ap, fmt);
    k = vsnprintf(o->b + o->n, o->cap - o->n, fmt, ap);
    va_end(ap);
    if (k > 0) o->n += (size_t)k;
    if (o->n >= o->cap) o->n = o->cap - 1;
}

static void dump_iter(gibus_iter *it, dumpbuf *o, const char *sep, int depth);

static void
dump_value(gibus_iter *it, int t, dumpbuf *o, int depth)
{
    union {
        uint8_t     y;
        gibus_bool  b;
        int16_t     n;
        uint16_t    q;
        int32_t     i;
        uint32_t    u;
        int64_t     x;
        uint64_t    t;
        double      d;
        const char *s;
    } v;
    gibus_iter sub;

    memset(&v, 0, sizeof(v));
    switch (t) {
    case GIBUS_TYPE_BYTE:
        gibus_iter_get_basic(it, &v.y);
        put(o, "y%u", (unsigned)v.y);
        break;
    case GIBUS_TYPE_BOOLEAN:
        gibus_iter_get_basic(it, &v.b);
        put(o, "b%u", (unsigned)v.b);
        break;
    case GIBUS_TYPE_INT16:
        gibus_iter_get_basic(it, &v.n);
        put(o, "n%d", (int)v.n);
        break;
    case GIBUS_TYPE_UINT16:
        gibus_iter_get_basic(it, &v.q);
        put(o, "q%u", (unsigned)v.q);
        break;
    case GIBUS_TYPE_INT32:
        gibus_iter_get_basic(it, &v.i);
        put(o, "i%ld", (long)v.i);
        break;
    case GIBUS_TYPE_UINT32:
        gibus_iter_get_basic(it, &v.u);
        put(o, "u%lu", (unsigned long)v.u);
        break;
    case GIBUS_TYPE_INT64:
        gibus_iter_get_basic(it, &v.x);
        put(o, "x%" PRId64, v.x);
        break;
    case GIBUS_TYPE_UINT64:
        gibus_iter_get_basic(it, &v.t);
        put(o, "t%" PRIu64, v.t);
        break;
    case GIBUS_TYPE_DOUBLE:
        gibus_iter_get_basic(it, &v.d);
        put(o, "d%" PRIx64, dbits(v.d));
        break;
    case GIBUS_TYPE_STRING:
    case GIBUS_TYPE_SIGNATURE:
        gibus_iter_get_basic(it, &v.s);
        put(o, "%c'%s'", t, v.s ? v.s : "(null)");
        break;
    case GIBUS_TYPE_OBJECT_PATH:
        gibus_iter_get_basic(it, &v.s);
        put(o, "o%s", v.s ? v.s : "(null)");
        break;
    case GIBUS_TYPE_ARRAY:
        gibus_iter_recurse(it, &sub);
        put(o, "[");
        dump_iter(&sub, o, ",", depth + 1);
        put(o, "]");
        break;
    case GIBUS_TYPE_STRUCT:
        gibus_iter_recurse(it, &sub);
        put(o, "(");
        dump_iter(&sub, o, ",", depth + 1);
        put(o, ")");
        break;
    case GIBUS_TYPE_DICT_ENTRY:
        gibus_iter_recurse(it, &sub);
        put(o, "{");
        dump_iter(&sub, o, "=", depth + 1);
        put(o, "}");
        break;
    case GIBUS_TYPE_VARIANT:
        gibus_iter_recurse(it, &sub);
        put(o, "<");
        dump_iter(&sub, o, " ", depth + 1);
        put(o, ">");
        break;
    default:
        put(o, "?%d", t);
        break;
    }
}

static void
dump_iter(gibus_iter *it, dumpbuf *o, const char *sep, int depth)
{
    long guard = 0;
    int t, first = 1;

    if (depth > 80) {
        put(o, "...");
        return;
    }
    while ((t = gibus_iter_type(it)) != GIBUS_TYPE_INVALID && guard++ < 100000) {
        if (!first) put(o, "%s", sep);
        first = 0;
        dump_value(it, t, o, depth);
        if (!gibus_iter_next(it)) break;
    }
}

static const char *
dump(gibus_msg *m, char *buf, size_t cap)
{
    gibus_iter it;
    dumpbuf o;

    o.b = buf;
    o.cap = cap;
    o.n = 0;
    buf[0] = 0;
    if (m && gibus_iter_init(m, &it)) dump_iter(&it, &o, " ", 0);
    return buf;
}

static void
drop(gibus_msg *m)
{
    if (m) gibus_msg_free(m);
}

static int
same(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

static gibus_msg *
reparse(gibus_msg *m, uint32_t serial)
{
    const uint8_t *b;
    size_t n = 0;

    if (!m || !(b = gibus_msg_bytes(m, serial, &n))) return NULL;
    return gibus_msg_parse(b, n);
}

static uint32_t
read32(const uint8_t *b, size_t at)
{
    if (b[0] == 'B')
        return ((uint32_t)b[at] << 24) | ((uint32_t)b[at + 1] << 16) |
               ((uint32_t)b[at + 2] << 8) | b[at + 3];
    return ((uint32_t)b[at + 3] << 24) | ((uint32_t)b[at + 2] << 16) |
           ((uint32_t)b[at + 1] << 8) | b[at];
}

#define RICH_STR  "h\xc3\xa9llo"
#define RICH_SIG  "ybnqiuxtdsoga(sb)a{sv}v"
#define RICH_DUMP "y200 b1 n-2 q65535 i-100000 u4000000000 x-9000000000 " \
                  "t18446744073709551615 d3ff8000000000000 s'h\xc3\xa9llo' o/a/b_c g'a{sv}' " \
                  "[(s'one',b1),(s'two',b0)] [{s'k'=<i9>}] <[s'x',s'yz']>"

static void
rich_body(wr *w)
{
    warr a;

    w_u8(w, 200);
    w_uint(w, 1, 4);
    w_uint(w, (uint16_t)(int16_t)-2, 2);
    w_uint(w, 65535, 2);
    w_uint(w, (uint32_t)(int32_t)-100000, 4);
    w_uint(w, 4000000000u, 4);
    w_uint(w, (uint64_t)-INT64_C(9000000000), 8);
    w_uint(w, UINT64_MAX, 8);
    w_uint(w, dbits(1.5), 8);
    w_str(w, RICH_STR);
    w_str(w, "/a/b_c");
    w_sig(w, "a{sv}");
    a = w_open(w, 8);
    w_pad(w, 8);
    w_str(w, "one");
    w_uint(w, 1, 4);
    w_pad(w, 8);
    w_str(w, "two");
    w_uint(w, 0, 4);
    w_close(w, a);
    a = w_open(w, 8);
    w_pad(w, 8);
    w_str(w, "k");
    w_sig(w, "i");
    w_uint(w, 9, 4);
    w_close(w, a);
    w_sig(w, "as");
    a = w_open(w, 4);
    w_str(w, "x");
    w_str(w, "yz");
    w_close(w, a);
}

static void
rich_frame(frame *f, int big, uint32_t serial, int flags)
{
    static const field fl[] = { FP("/org/gibus/Obj"), FI("org.gibus.Iface"), FM("Method"),
                                FD("org.gibus.Dest"), FS(":1.42"), FG(RICH_SIG) };
    uint8_t body[1024];
    wr w;

    w_init(&w, body, sizeof(body), big);
    rich_body(&w);
    make_frame(f, big, GIBUS_MESSAGE_METHOD_CALL, flags, serial, fl, (int)COUNT(fl), body,
               w.n);
}

static gibus_msg *
rich_built(void)
{
    gibus_msg *m = gibus_msg_call("org.gibus.Dest", "/org/gibus/Obj", "org.gibus.Iface",
                                  "Method");
    uint8_t y = 200;
    gibus_bool b = 1, f = 0;
    int16_t n = -2;
    uint16_t q = 65535;
    int32_t i = -100000, nine = 9;
    uint32_t u = 4000000000u;
    int64_t x = -INT64_C(9000000000);
    uint64_t t = UINT64_MAX;
    double d = 1.5;
    const char *s = RICH_STR, *o = "/a/b_c", *g = "a{sv}";
    const char *one = "one", *two = "two", *k = "k", *sx = "x", *syz = "yz";
    gibus_iter it, arr, st, ent, var, sub;

    if (!m) return NULL;
    if (!gibus_msg_append_args(m, GIBUS_TYPE_BYTE, &y, GIBUS_TYPE_BOOLEAN, &b, GIBUS_TYPE_INT16, &n,
                               GIBUS_TYPE_UINT16, &q, GIBUS_TYPE_INT32, &i, GIBUS_TYPE_UINT32, &u,
                               GIBUS_TYPE_INT64, &x, GIBUS_TYPE_UINT64, &t, GIBUS_TYPE_DOUBLE, &d,
                               GIBUS_TYPE_STRING, &s, GIBUS_TYPE_OBJECT_PATH, &o,
                               GIBUS_TYPE_SIGNATURE, &g, GIBUS_TYPE_INVALID)) {
        drop(m);
        return NULL;
    }
    gibus_iter_init_append(m, &it);
    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "(sb)", &arr);
    gibus_iter_open(&arr, GIBUS_TYPE_STRUCT, NULL, &st);
    gibus_iter_append_basic(&st, GIBUS_TYPE_STRING, &one);
    gibus_iter_append_basic(&st, GIBUS_TYPE_BOOLEAN, &b);
    gibus_iter_close(&arr, &st);
    gibus_iter_open(&arr, GIBUS_TYPE_STRUCT, NULL, &st);
    gibus_iter_append_basic(&st, GIBUS_TYPE_STRING, &two);
    gibus_iter_append_basic(&st, GIBUS_TYPE_BOOLEAN, &f);
    gibus_iter_close(&arr, &st);
    gibus_iter_close(&it, &arr);
    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "{sv}", &arr);
    gibus_iter_open(&arr, GIBUS_TYPE_DICT_ENTRY, NULL, &ent);
    gibus_iter_append_basic(&ent, GIBUS_TYPE_STRING, &k);
    gibus_iter_open(&ent, GIBUS_TYPE_VARIANT, "i", &var);
    gibus_iter_append_basic(&var, GIBUS_TYPE_INT32, &nine);
    gibus_iter_close(&ent, &var);
    gibus_iter_close(&arr, &ent);
    gibus_iter_close(&it, &arr);
    gibus_iter_open(&it, GIBUS_TYPE_VARIANT, "as", &var);
    gibus_iter_open(&var, GIBUS_TYPE_ARRAY, "s", &sub);
    gibus_iter_append_basic(&sub, GIBUS_TYPE_STRING, &sx);
    gibus_iter_append_basic(&sub, GIBUS_TYPE_STRING, &syz);
    gibus_iter_close(&var, &sub);
    gibus_iter_close(&it, &var);
    return m;
}

static void
test_basic_types(void)
{
    static char d1[DUMP_CAP], d2[DUMP_CAP];
    gibus_msg *m = rich_built(), *p, *le, *be, *again;
    frame f;

    puts("");
    puts("every basic type and container survives the round trip");
    ok("the message builds", m != NULL);
    p = reparse(m, 3);
    ok("and parses back from its bytes", p != NULL);
    test_str("its signature", p ? gibus_msg_signature(p) : NULL, RICH_SIG);
    test_str("its body", dump(p, d1, sizeof(d1)), RICH_DUMP);
    test_eq("its serial", p ? (long)gibus_msg_serial(p) : -1, 3);
    test_eq("a call", p ? gibus_msg_type(p) : -1, GIBUS_MESSAGE_METHOD_CALL);
    test_str("its path", p ? gibus_msg_path(p) : NULL, "/org/gibus/Obj");
    test_str("its interface", p ? gibus_msg_interface(p) : NULL, "org.gibus.Iface");
    test_str("its member", p ? gibus_msg_member(p) : NULL, "Method");
    test_str("its destination", p ? gibus_msg_destination(p) : NULL, "org.gibus.Dest");
    ok("no sender, no error name, no reply serial",
       p && !gibus_msg_sender(p) && !gibus_msg_error_name(p) && gibus_msg_reply_serial(p) == 0);
    ok("and replies expected", p && !gibus__msg_no_reply(p));
    drop(p);
    drop(m);

    puts("");
    puts("the same message written by hand, in both byte orders");
    rich_frame(&f, 1, 0x01020304u, 0);
    be = gibus_msg_parse(f.b, f.n);
    ok("big-endian parses", be != NULL);
    test_str("to the same body", dump(be, d1, sizeof(d1)), RICH_DUMP);
    test_eq("its serial", be ? (long)gibus_msg_serial(be) : -1, 0x01020304L);
    test_str("its sender", be ? gibus_msg_sender(be) : NULL, ":1.42");
    test_str("its destination", be ? gibus_msg_destination(be) : NULL, "org.gibus.Dest");
    test_str("its signature", be ? gibus_msg_signature(be) : NULL, RICH_SIG);
    rich_frame(&f, 0, 0x01020304u, 1);
    le = gibus_msg_parse(f.b, f.n);
    ok("little-endian parses", le != NULL);
    test_str("to the same body", dump(le, d2, sizeof(d2)), RICH_DUMP);
    ok("NO_REPLY_EXPECTED read from the flags", le && gibus__msg_no_reply(le));
    ok("and not where it is clear", be && !gibus__msg_no_reply(be));
    again = reparse(be, 9);
    ok("big-endian input serializes again", again != NULL);
    test_str("with the same body", dump(again, d2, sizeof(d2)), RICH_DUMP);
    test_eq("and the new serial", again ? (long)gibus_msg_serial(again) : -1, 9);
    test_str("keeping its sender", again ? gibus_msg_sender(again) : NULL, ":1.42");
    drop(again);

    puts("");
    puts("header predicates");
    ok("is_call with its interface and member",
       be && gibus_msg_is_call(be, "org.gibus.Iface", "Method"));
    ok("not with another member", be && !gibus_msg_is_call(be, "org.gibus.Iface", "Other"));
    ok("not with another interface", be && !gibus_msg_is_call(be, "org.gibus.Other", "Method"));
    ok("and not a signal", be && !gibus_msg_is_signal(be, "org.gibus.Iface", "Method"));
    ok("has_member", be && gibus_msg_has_member(be, "Method") && !gibus_msg_has_member(be, "M"));
    ok("has_signature", be && gibus_msg_has_signature(be, RICH_SIG) &&
                            !gibus_msg_has_signature(be, "y"));
    drop(be);
    drop(le);
}

static void
test_containers(void)
{
    static char d[DUMP_CAP];
    static const int64_t xs[] = { 1, -1 };
    static const uint8_t ys[] = { 1, 2, 3 };
    static const int32_t is[] = { 1, -2, 3 };
    static const double ds[] = { 1.5 };
    gibus_msg *m = gibus_msg_signal("/org/gibus/Obj", "org.gibus.Iface", "Tick"), *p;
    gibus_iter it, a, e, v, v2, s, s2, in;
    const char *kname = "name", *ksize = "size", *knested = "nested", *kdict = "dict";
    const char *kempty = "empty", *kdeep = "deep", *sx = "x", *sz = "z", *root = "/", *px = "/x";
    const char *g0 = "", *g1 = "a{sv}";
    int32_t three = 3, four = 4, one = 1, two = 2, neg1 = -1, neg2 = -2;
    uint32_t seven = 7;
    gibus_bool t = 1, f = 0;
    uint8_t nine = 9, eight = 8, y1 = 1, y2 = 2;
    double q = -0.25;

    puts("");
    puts("nested containers: a{sv} with variants, empty arrays, fixed arrays");
    ok("the message starts", m != NULL);
    if (!m) return;
    gibus_iter_init_append(m, &it);

    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "{sv}", &a);
    gibus_iter_open(&a, GIBUS_TYPE_DICT_ENTRY, NULL, &e);
    gibus_iter_append_basic(&e, GIBUS_TYPE_STRING, &kname);
    gibus_iter_open(&e, GIBUS_TYPE_VARIANT, "s", &v);
    gibus_iter_append_basic(&v, GIBUS_TYPE_STRING, &sx);
    gibus_iter_close(&e, &v);
    gibus_iter_close(&a, &e);
    gibus_iter_open(&a, GIBUS_TYPE_DICT_ENTRY, NULL, &e);
    gibus_iter_append_basic(&e, GIBUS_TYPE_STRING, &ksize);
    gibus_iter_open(&e, GIBUS_TYPE_VARIANT, "(ii)", &v);
    gibus_iter_open(&v, GIBUS_TYPE_STRUCT, NULL, &s);
    gibus_iter_append_basic(&s, GIBUS_TYPE_INT32, &three);
    gibus_iter_append_basic(&s, GIBUS_TYPE_INT32, &four);
    gibus_iter_close(&v, &s);
    gibus_iter_close(&e, &v);
    gibus_iter_close(&a, &e);
    gibus_iter_open(&a, GIBUS_TYPE_DICT_ENTRY, NULL, &e);
    gibus_iter_append_basic(&e, GIBUS_TYPE_STRING, &knested);
    gibus_iter_open(&e, GIBUS_TYPE_VARIANT, "v", &v);
    gibus_iter_open(&v, GIBUS_TYPE_VARIANT, "u", &v2);
    gibus_iter_append_basic(&v2, GIBUS_TYPE_UINT32, &seven);
    gibus_iter_close(&v, &v2);
    gibus_iter_close(&e, &v);
    gibus_iter_close(&a, &e);
    gibus_iter_open(&a, GIBUS_TYPE_DICT_ENTRY, NULL, &e);
    gibus_iter_append_basic(&e, GIBUS_TYPE_STRING, &kdict);
    gibus_iter_open(&e, GIBUS_TYPE_VARIANT, "a{sv}", &v);
    gibus_iter_open(&v, GIBUS_TYPE_ARRAY, "{sv}", &s);
    gibus_iter_open(&s, GIBUS_TYPE_DICT_ENTRY, NULL, &s2);
    gibus_iter_append_basic(&s2, GIBUS_TYPE_STRING, &kdeep);
    gibus_iter_open(&s2, GIBUS_TYPE_VARIANT, "b", &v2);
    gibus_iter_append_basic(&v2, GIBUS_TYPE_BOOLEAN, &t);
    gibus_iter_close(&s2, &v2);
    gibus_iter_close(&s, &s2);
    gibus_iter_close(&v, &s);
    gibus_iter_close(&e, &v);
    gibus_iter_close(&a, &e);
    gibus_iter_open(&a, GIBUS_TYPE_DICT_ENTRY, NULL, &e);
    gibus_iter_append_basic(&e, GIBUS_TYPE_STRING, &kempty);
    gibus_iter_open(&e, GIBUS_TYPE_VARIANT, "a{sv}", &v);
    gibus_iter_open(&v, GIBUS_TYPE_ARRAY, "{sv}", &s);
    gibus_iter_close(&v, &s);
    gibus_iter_close(&e, &v);
    gibus_iter_close(&a, &e);
    gibus_iter_close(&it, &a);

    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "t", &a);
    gibus_iter_close(&it, &a);
    gibus_iter_append_basic(&it, GIBUS_TYPE_BYTE, &nine);
    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "(yy)", &a);
    gibus_iter_close(&it, &a);
    gibus_iter_append_basic(&it, GIBUS_TYPE_BYTE, &eight);

    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "x", &a);
    gibus_iter_append_fixed_array(&a, GIBUS_TYPE_INT64, xs, 2);
    gibus_iter_close(&it, &a);
    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "y", &a);
    gibus_iter_append_fixed_array(&a, GIBUS_TYPE_BYTE, ys, 3);
    gibus_iter_close(&it, &a);
    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "i", &a);
    gibus_iter_append_fixed_array(&a, GIBUS_TYPE_INT32, is, 3);
    gibus_iter_close(&it, &a);
    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "d", &a);
    gibus_iter_append_fixed_array(&a, GIBUS_TYPE_DOUBLE, ds, 1);
    gibus_iter_close(&it, &a);

    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "ai", &a);
    gibus_iter_open(&a, GIBUS_TYPE_ARRAY, "i", &in);
    gibus_iter_append_basic(&in, GIBUS_TYPE_INT32, &one);
    gibus_iter_append_basic(&in, GIBUS_TYPE_INT32, &two);
    gibus_iter_close(&a, &in);
    gibus_iter_open(&a, GIBUS_TYPE_ARRAY, "i", &in);
    gibus_iter_close(&a, &in);
    gibus_iter_open(&a, GIBUS_TYPE_ARRAY, "i", &in);
    gibus_iter_append_basic(&in, GIBUS_TYPE_INT32, &three);
    gibus_iter_close(&a, &in);
    gibus_iter_close(&it, &a);

    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "{yi}", &a);
    gibus_iter_open(&a, GIBUS_TYPE_DICT_ENTRY, NULL, &e);
    gibus_iter_append_basic(&e, GIBUS_TYPE_BYTE, &y1);
    gibus_iter_append_basic(&e, GIBUS_TYPE_INT32, &neg1);
    gibus_iter_close(&a, &e);
    gibus_iter_open(&a, GIBUS_TYPE_DICT_ENTRY, NULL, &e);
    gibus_iter_append_basic(&e, GIBUS_TYPE_BYTE, &y2);
    gibus_iter_append_basic(&e, GIBUS_TYPE_INT32, &neg2);
    gibus_iter_close(&a, &e);
    gibus_iter_close(&it, &a);

    gibus_iter_open(&it, GIBUS_TYPE_STRUCT, NULL, &s);
    gibus_iter_append_basic(&s, GIBUS_TYPE_INT32, &one);
    gibus_iter_open(&s, GIBUS_TYPE_STRUCT, NULL, &s2);
    gibus_iter_append_basic(&s2, GIBUS_TYPE_STRING, &sz);
    gibus_iter_append_basic(&s2, GIBUS_TYPE_BYTE, &y2);
    gibus_iter_close(&s, &s2);
    gibus_iter_append_basic(&s, GIBUS_TYPE_DOUBLE, &q);
    gibus_iter_close(&it, &s);

    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "b", &a);
    gibus_iter_append_basic(&a, GIBUS_TYPE_BOOLEAN, &t);
    gibus_iter_append_basic(&a, GIBUS_TYPE_BOOLEAN, &f);
    gibus_iter_close(&it, &a);
    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "o", &a);
    gibus_iter_append_basic(&a, GIBUS_TYPE_OBJECT_PATH, &root);
    gibus_iter_append_basic(&a, GIBUS_TYPE_OBJECT_PATH, &px);
    gibus_iter_close(&it, &a);
    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "g", &a);
    gibus_iter_append_basic(&a, GIBUS_TYPE_SIGNATURE, &g0);
    gibus_iter_append_basic(&a, GIBUS_TYPE_SIGNATURE, &g1);
    gibus_iter_close(&it, &a);

    p = reparse(m, 4);
    ok("builds and parses back", p != NULL);
    test_str("its signature", p ? gibus_msg_signature(p) : NULL,
             "a{sv}atya(yy)yaxayaiadaaia{yi}(i(sy)d)abaoag");
    test_str("its body", dump(p, d, sizeof(d)),
             "[{s'name'=<s'x'>},{s'size'=<(i3,i4)>},{s'nested'=<<u7>>},"
             "{s'dict'=<[{s'deep'=<b1>}]>},{s'empty'=<[]>}] [] y9 [] y8 [x1,x-1] [y1,y2,y3] "
             "[i1,i-2,i3] [d3ff8000000000000] [[i1,i2],[],[i3]] [{y1=i-1},{y2=i-2}] "
             "(i1,(s'z',y2),dbfd0000000000000) [b1,b0] [o/,o/x] [g'',g'a{sv}']");
    ok("a signal", p && gibus_msg_type(p) == GIBUS_MESSAGE_SIGNAL);
    ok("is_signal with its interface and member",
       p && gibus_msg_is_signal(p, "org.gibus.Iface", "Tick"));
    ok("not with another interface", p && !gibus_msg_is_signal(p, "org.gibus.X", "Tick"));
    ok("and not a call", p && !gibus_msg_is_call(p, "org.gibus.Iface", "Tick"));
    ok("no destination", p && !gibus_msg_destination(p));
    drop(p);
    drop(m);
}

static void
test_alignment(void)
{
    gibus_msg *m = gibus_msg_signal("/a", "org.gibus.Iface", "Pad");
    gibus_iter it, a;
    const uint8_t *b = NULL;
    uint8_t nine = 9;
    size_t n = 0;

    puts("");
    puts("an empty array still pads to its element");
    if (m) {
        gibus_iter_init_append(m, &it);
        gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "t", &a);
        gibus_iter_close(&it, &a);
        gibus_iter_append_basic(&it, GIBUS_TYPE_BYTE, &nine);
        b = gibus_msg_bytes(m, 1, &n);
    }
    ok("aty builds", b != NULL && n >= 16 + 9);
    if (b && n >= 16 + 9) {
        static const uint8_t want[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 9 };
        test_eq("its body is nine bytes", (long)read32(b, 4), 9);
        ok("length, four of padding, the byte", memcmp(b + n - 9, want, 9) == 0);
        test_eq("the whole message, from its first 16", gibus__frame_size(b, 16), (long)n);
    }
    drop(m);
}

static void
test_get_args(void)
{
    static const int32_t is[] = { 10, -20, 30 };
    static const uint8_t ys[] = { 1, 2, 3, 4 };
    gibus_msg *m = gibus_msg_call(NULL, "/org/gibus/Obj", NULL, "Args"), *p, *e = NULL, *pe;
    gibus_iter it, a;
    gibus_error err;
    const char *s = "text", *gs = NULL;
    const void *pi = NULL, *py = NULL;
    int ni = -1, ny = -1, got;
    uint32_t u = 7, gu = 0;
    uint8_t extra = 0;
    uint64_t t = (uint64_t)1 << 40, gt = 0;
    int32_t gi = 0, v[3] = { 0, 0, 0 };

    puts("");
    puts("get_args reads the leading arguments, int32 arrays included");
    if (!m) {
        ok("the message starts", 0);
        return;
    }
    gibus_iter_init_append(m, &it);
    gibus_iter_append_basic(&it, GIBUS_TYPE_STRING, &s);
    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "i", &a);
    gibus_iter_append_fixed_array(&a, GIBUS_TYPE_INT32, is, 3);
    gibus_iter_close(&it, &a);
    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "y", &a);
    gibus_iter_append_fixed_array(&a, GIBUS_TYPE_BYTE, ys, 4);
    gibus_iter_close(&it, &a);
    gibus_iter_append_basic(&it, GIBUS_TYPE_UINT32, &u);
    gibus_iter_append_basic(&it, GIBUS_TYPE_UINT64, &t);
    p = reparse(m, 5);
    ok("sayut parses back", p != NULL);
    test_str("its signature", p ? gibus_msg_signature(p) : NULL, "saiayut");
    ok("no interface, no destination", p && !gibus_msg_interface(p) && !gibus_msg_destination(p));

    gibus_error_init(&err);
    got = p && gibus_msg_get_args(p, &err, GIBUS_TYPE_STRING, &gs, GIBUS_TYPE_ARRAY,
                                  GIBUS_TYPE_INT32, &pi, &ni, GIBUS_TYPE_ARRAY, GIBUS_TYPE_BYTE,
                                  &py, &ny, GIBUS_TYPE_UINT32, &gu, GIBUS_TYPE_UINT64, &gt,
                                  GIBUS_TYPE_INVALID);
    ok("all five read", got && !gibus_error_is_set(&err));
    test_str("the string", gs, "text");
    test_eq("three int32s", ni, 3);
    if (got && ni == 3 && pi) memcpy(v, pi, sizeof(v));
    ok("10, -20, 30", v[0] == 10 && v[1] == -20 && v[2] == 30);
    test_eq("four bytes", ny, 4);
    ok("1, 2, 3, 4", got && ny == 4 && py && memcmp(py, ys, 4) == 0);
    test_eq("the uint32", (long)gu, 7);
    ok("the uint64", gt == ((uint64_t)1 << 40));
    gibus_error_free(&err);

    gs = NULL;
    ok("just the first one", p && gibus_msg_get_args(p, NULL, GIBUS_TYPE_STRING, &gs,
                                                      GIBUS_TYPE_INVALID) && same(gs, "text"));
    gibus_error_init(&err);
    ok("a wrong first type answers 0",
       p && !gibus_msg_get_args(p, &err, GIBUS_TYPE_INT32, &gi, GIBUS_TYPE_INVALID));
    ok("with the error set", gibus_error_is_set(&err));
    gibus_error_free(&err);
    ok("and freeing clears it", !gibus_error_is_set(&err));
    gibus_error_init(&err);
    ok("the wrong element type answers 0",
       p && !gibus_msg_get_args(p, &err, GIBUS_TYPE_STRING, &gs, GIBUS_TYPE_ARRAY,
                                GIBUS_TYPE_BYTE, &pi, &ni, GIBUS_TYPE_INVALID));
    gibus_error_free(&err);
    ok("asking past the body answers 0",
       p && !gibus_msg_get_args(p, NULL, GIBUS_TYPE_STRING, &gs, GIBUS_TYPE_ARRAY,
                                GIBUS_TYPE_INT32, &pi, &ni, GIBUS_TYPE_ARRAY, GIBUS_TYPE_BYTE,
                                &py, &ny, GIBUS_TYPE_UINT32, &gu, GIBUS_TYPE_UINT64, &gt,
                                GIBUS_TYPE_BYTE, &extra, GIBUS_TYPE_INVALID));
    drop(p);
    drop(m);

    m = gibus_msg_call(NULL, "/org/gibus/Obj", NULL, "Empty");
    if (m) {
        gibus_iter_init_append(m, &it);
        gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "i", &a);
        gibus_iter_close(&it, &a);
    }
    p = reparse(m, 6);
    ni = -1;
    ok("an empty int32 array reads as none",
       p && gibus_msg_get_args(p, NULL, GIBUS_TYPE_ARRAY, GIBUS_TYPE_INT32, &pi, &ni,
                               GIBUS_TYPE_INVALID) && ni == 0);
    drop(p);
    drop(m);

    m = gibus_msg_call(NULL, "/org/gibus/Obj", NULL, "Wide");
    if (m) {
        static const int64_t xs[] = { -1, INT64_C(1) << 40 };
        static const double ds[] = { 0.5, -2.25 };

        gibus_iter_init_append(m, &it);
        gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "x", &a);
        gibus_iter_append_fixed_array(&a, GIBUS_TYPE_INT64, xs, 2);
        gibus_iter_close(&it, &a);
        gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "d", &a);
        gibus_iter_append_fixed_array(&a, GIBUS_TYPE_DOUBLE, ds, 2);
        gibus_iter_close(&it, &a);
    }
    p = reparse(m, 9);
    pi = py = NULL;
    ni = ny = -1;
    got = p && gibus_msg_get_args(p, NULL, GIBUS_TYPE_ARRAY, GIBUS_TYPE_INT64, &pi, &ni,
                                  GIBUS_TYPE_ARRAY, GIBUS_TYPE_DOUBLE, &py, &ny,
                                  GIBUS_TYPE_INVALID);
    ok("axad reads two and two", got && ni == 2 && ny == 2 && pi && py);
    ok("the int64s sit on 8 bytes", pi && (uintptr_t)pi % 8 == 0);
    ok("the doubles sit on 8 bytes", py && (uintptr_t)py % 8 == 0);
    ok("-1, 1 << 40", got && pi && ((const int64_t *)pi)[0] == -1 &&
                          ((const int64_t *)pi)[1] == INT64_C(1) << 40);
    ok("0.5, -2.25", got && py && ((const double *)py)[0] == 0.5 &&
                         ((const double *)py)[1] == -2.25);
    drop(p);
    drop(m);

    puts("");
    puts("the iterator steps and stops");
    m = gibus_msg_call(NULL, "/org/gibus/Obj", NULL, "Two");
    if (m) {
        int32_t five = 5;
        gibus_msg_append_args(m, GIBUS_TYPE_INT32, &five, GIBUS_TYPE_STRING, &s,
                              GIBUS_TYPE_INVALID);
        gibus_iter_init_append(m, &it);
        gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "s", &a);
        gibus_iter_close(&it, &a);
    }
    p = reparse(m, 7);
    if (p && gibus_iter_init(p, &it)) {
        gibus_iter sub;

        test_eq("first an int32", gibus_iter_type(&it), GIBUS_TYPE_INT32);
        gibus_iter_get_basic(&it, &gi);
        test_eq("of 5", gi, 5);
        test_eq("next answers 1", gibus_iter_next(&it), 1);
        test_eq("then a string", gibus_iter_type(&it), GIBUS_TYPE_STRING);
        test_eq("next answers 1", gibus_iter_next(&it), 1);
        test_eq("then an array", gibus_iter_type(&it), GIBUS_TYPE_ARRAY);
        gibus_iter_recurse(&it, &sub);
        test_eq("which is empty", gibus_iter_type(&sub), GIBUS_TYPE_INVALID);
        test_eq("next answers 0 at the end", gibus_iter_next(&it), 0);
        test_eq("and the type is INVALID", gibus_iter_type(&it), GIBUS_TYPE_INVALID);
    } else {
        ok("the message parses with a body", 0);
    }
    drop(p);
    drop(m);
    m = gibus_msg_call(NULL, "/org/gibus/Obj", NULL, "None");
    p = reparse(m, 8);
    ok("an empty body parses", p != NULL);
    test_str("with signature \"\"", p ? gibus_msg_signature(p) : NULL, "");
    ok("and iter_init answers 0", p && !gibus_iter_init(p, &it));
    drop(p);
    drop(m);

    puts("");
    puts("a reply and an error to a call");
    {
        frame f;
        const char *text = NULL;
        static const field fl[] = { FP("/org/gibus/Obj"), FM("Ask"), FS(":1.42") };

        make_frame(&f, 0, GIBUS_MESSAGE_METHOD_CALL, 0, 77, fl, 3, NULL, 0);
        m = gibus_msg_parse(f.b, f.n);
        ok("the call parses", m != NULL);
        p = m ? gibus_msg_return(m) : NULL;
        test_eq("a return", p ? gibus_msg_type(p) : -1, GIBUS_MESSAGE_METHOD_RETURN);
        test_eq("to serial 77", p ? (long)gibus_msg_reply_serial(p) : -1, 77);
        test_str("sent back to the caller", p ? gibus_msg_destination(p) : NULL, ":1.42");
        if (p) gibus_msg_append_args(p, GIBUS_TYPE_STRING, &s, GIBUS_TYPE_INVALID);
        pe = reparse(p, 78);
        ok("which parses back", pe != NULL);
        test_eq("still to serial 77", pe ? (long)gibus_msg_reply_serial(pe) : -1, 77);
        test_eq("its own serial 78", pe ? (long)gibus_msg_serial(pe) : -1, 78);
        test_str("still to the caller", pe ? gibus_msg_destination(pe) : NULL, ":1.42");
        test_str("with its argument", pe ? gibus_msg_signature(pe) : NULL, "s");
        ok("without a path or a member", pe && !gibus_msg_path(pe) && !gibus_msg_member(pe));
        drop(pe);
        drop(p);

        e = m ? gibus_msg_error(m, "org.gibus.Error.Bad", "went wrong") : NULL;
        pe = reparse(e, 79);
        ok("an error parses back", pe != NULL);
        test_eq("an error", pe ? gibus_msg_type(pe) : -1, GIBUS_MESSAGE_ERROR);
        test_str("its name", pe ? gibus_msg_error_name(pe) : NULL, "org.gibus.Error.Bad");
        test_eq("to serial 77", pe ? (long)gibus_msg_reply_serial(pe) : -1, 77);
        test_str("sent back to the caller", pe ? gibus_msg_destination(pe) : NULL, ":1.42");
        test_str("its signature", pe ? gibus_msg_signature(pe) : NULL, "s");
        if (pe) gibus_msg_get_args(pe, NULL, GIBUS_TYPE_STRING, &text, GIBUS_TYPE_INVALID);
        test_str("its text", text, "went wrong");
        drop(pe);
        drop(e);
        drop(m);
    }
}

static void
test_bad_builds(void)
{
    gibus_msg *m;
    gibus_iter it, sub;
    size_t n;
    const char *bad_path = "no/slash", *bad_utf8 = "\xff\xfe", *s = "s";
    int32_t i = 1;

    puts("");
    puts("a message that cannot be valid does not build");
    m = gibus_msg_signal("/x", "org.gibus.Iface", "M");
    if (m) gibus_msg_append_args(m, GIBUS_TYPE_OBJECT_PATH, &bad_path, GIBUS_TYPE_INVALID);
    ok("a relative object path", !m || !gibus_msg_bytes(m, 1, &n));
    drop(m);
    m = gibus_msg_signal("/x", "org.gibus.Iface", "M");
    if (m) gibus_msg_append_args(m, GIBUS_TYPE_STRING, &bad_utf8, GIBUS_TYPE_INVALID);
    ok("a string that is not UTF-8", !m || !gibus_msg_bytes(m, 1, &n));
    drop(m);
    m = gibus_msg_signal("/x", "org.gibus.Iface", "M");
    if (m) {
        gibus_iter_init_append(m, &it);
        gibus_iter_open(&it, GIBUS_TYPE_VARIANT, "i", &sub);
        gibus_iter_append_basic(&sub, GIBUS_TYPE_STRING, &s);
        gibus_iter_close(&it, &sub);
    }
    ok("a variant holding another type", !m || !gibus_msg_bytes(m, 1, &n));
    drop(m);
    m = gibus_msg_signal("/x", "org.gibus.Iface", "M");
    if (m) {
        gibus_iter_init_append(m, &it);
        gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "s", &sub);
        gibus_iter_append_basic(&sub, GIBUS_TYPE_INT32, &i);
        gibus_iter_close(&it, &sub);
    }
    ok("an array element of another type", !m || !gibus_msg_bytes(m, 1, &n));
    drop(m);
    m = gibus_msg_signal("/x", "org.gibus.Iface", "M");
    if (m) {
        gibus_iter_init_append(m, &it);
        gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "{", &sub);
        gibus_iter_close(&it, &sub);
    }
    ok("an array of an invalid signature", !m || !gibus_msg_bytes(m, 1, &n));
    drop(m);
    m = gibus_msg_signal("/x", "nodot", "M");
    ok("a signal on an interface without a dot", !m || !gibus_msg_bytes(m, 1, &n));
    drop(m);
    m = gibus_msg_call(NULL, "relative", NULL, "M");
    ok("a call to a relative path", !m || !gibus_msg_bytes(m, 1, &n));
    drop(m);
}

static void
test_big(void)
{
    uint8_t *big = (uint8_t *)malloc(BIG_LEN);
    gibus_msg *m = gibus_msg_signal("/big", "org.gibus.Iface", "Big"), *p;
    gibus_iter it, a;
    const void *pb = NULL;
    const uint8_t *b = NULL;
    uint32_t tail = 0xfeedfaceu, gt = 0;
    size_t i, n = 0;
    int nb = -1;

    puts("");
    puts("a 1 MiB byte array");
    if (!big || !m) {
        ok("memory for it", 0);
        free(big);
        drop(m);
        return;
    }
    for (i = 0; i < BIG_LEN; i++) big[i] = (uint8_t)(i * 7 + 3);
    gibus_iter_init_append(m, &it);
    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "y", &a);
    gibus_iter_append_fixed_array(&a, GIBUS_TYPE_BYTE, big, BIG_LEN);
    gibus_iter_close(&it, &a);
    gibus_iter_append_basic(&it, GIBUS_TYPE_UINT32, &tail);
    b = gibus_msg_bytes(m, 2, &n);
    ok("builds", b != NULL && n > BIG_LEN);
    test_eq("its frame size from the first 16 bytes", b ? gibus__frame_size(b, 16) : -2, (long)n);
    p = b ? gibus_msg_parse(b, n) : NULL;
    ok("parses back", p != NULL);
    ok("and reads back whole",
       p && gibus_msg_get_args(p, NULL, GIBUS_TYPE_ARRAY, GIBUS_TYPE_BYTE, &pb, &nb,
                               GIBUS_TYPE_UINT32, &gt, GIBUS_TYPE_INVALID) &&
           nb == BIG_LEN && pb && memcmp(pb, big, BIG_LEN) == 0 && gt == tail);
    drop(p);
    drop(m);
    free(big);
}

static void
expect_parse(const char *what, int want, const uint8_t *b, size_t n)
{
    gibus_msg *m = gibus_msg_parse(b, n);
    char line[160];

    snprintf(line, sizeof(line), "%s %s", want ? "takes" : "refuses", what);
    ok(line, want ? m != NULL : m == NULL);
    drop(m);
}

typedef struct hrow {
    const char *what;
    int         want;
    int         type;
    uint32_t    serial;
    int         nf;
    field       f[6];
} hrow;

static const hrow g_headers[] = {
    { "a call with every field", 1, 1, 1, 5,
      { FP("/org/x"), FI("org.x.Y"), FM("Do"), FD("org.x.Dest"), FS(":1.42") } },
    { "a call without an interface", 1, 1, 1, 2, { FP("/org/x"), FM("Do") } },
    { "a call to a unique name", 1, 1, 1, 3, { FP("/org/x"), FM("Do"), FD(":1.42") } },
    { "a call with an unknown field", 1, 1, 1, 3, { FP("/org/x"), FM("Do"), { 100, 's', "x", 0 } } },
    { "a signal", 1, 4, 1, 3, { FP("/org/x"), FI("org.x.Y"), FM("Changed") } },
    { "a return", 1, 2, 1, 1, { FR(5) } },
    { "an error", 1, 3, 1, 3, { FE("org.x.Error.Bad"), FR(5), FD(":1.42") } },
    { "a call without a path", 0, 1, 1, 2, { FI("org.x.Y"), FM("Do") } },
    { "a call without a member", 0, 1, 1, 2, { FP("/org/x"), FI("org.x.Y") } },
    { "a signal without an interface", 0, 4, 1, 2, { FP("/org/x"), FM("Changed") } },
    { "a signal without a path", 0, 4, 1, 2, { FI("org.x.Y"), FM("Changed") } },
    { "a return without a reply serial", 0, 2, 1, 1, { FD(":1.42") } },
    { "a return to serial 0", 0, 2, 1, 1, { FR(0) } },
    { "an error without a name", 0, 3, 1, 1, { FR(5) } },
    { "an error without a reply serial", 0, 3, 1, 1, { FE("org.x.Error.Bad") } },
    { "serial 0", 0, 1, 0, 2, { FP("/org/x"), FM("Do") } },
    { "message type 0", 0, 0, 1, 2, { FP("/org/x"), FM("Do") } },
    { "message type 5", 0, 5, 1, 2, { FP("/org/x"), FM("Do") } },
    { "a path typed as a string", 0, 1, 1, 2, { { 1, 's', "/org/x", 0 }, FM("Do") } },
    { "a member typed as a path", 0, 1, 1, 2, { FP("/org/x"), { 3, 'o', "/Do", 0 } } },
    { "a reply serial typed as a string", 0, 2, 1, 1, { { 5, 's', "5", 0 } } },
    { "header field 0", 0, 1, 1, 3, { FP("/org/x"), FM("Do"), { 0, 's', "x", 0 } } },
    { "an unknown field with bad UTF-8", 0, 1, 1, 3,
      { FP("/org/x"), FM("Do"), { 100, 's', "\xff", 0 } } },
    { "a relative path", 0, 1, 1, 2, { FP("org/x"), FM("Do") } },
    { "a path ending in /", 0, 1, 1, 2, { FP("/org/x/"), FM("Do") } },
    { "a path with //", 0, 1, 1, 2, { FP("/org//x"), FM("Do") } },
    { "a path with a dash", 0, 1, 1, 2, { FP("/org/x-y"), FM("Do") } },
    { "a member starting with a digit", 0, 1, 1, 2, { FP("/org/x"), FM("1Do") } },
    { "a member with a dot", 0, 1, 1, 2, { FP("/org/x"), FM("Do.It") } },
    { "an empty member", 0, 1, 1, 2, { FP("/org/x"), FM("") } },
    { "an interface without a dot", 0, 1, 1, 3, { FP("/org/x"), FI("nodot"), FM("Do") } },
    { "an interface with an empty element", 0, 1, 1, 3, { FP("/org/x"), FI("org..x"), FM("Do") } },
    { "an interface element starting with a digit", 0, 1, 1, 3,
      { FP("/org/x"), FI("org.1x"), FM("Do") } },
    { "an error name without a dot", 0, 3, 1, 2, { FE("Bad"), FR(5) } },
    { "a destination without a dot", 0, 1, 1, 3, { FP("/org/x"), FM("Do"), FD("nodot") } },
    { "a signature with no body", 0, 1, 1, 3, { FP("/org/x"), FM("Do"), FG("i") } },
};

typedef struct brow {
    const char *what;
    int         want;
    const char *sig;
    const char *body;
    size_t      len;
} brow;

#define BODY(s) s, sizeof(s) - 1

static const brow g_bodies[] = {
    { "an int32", 1, "i", BODY("\5\0\0\0") },
    { "a uint32 cut short", 0, "u", BODY("\5\0\0") },
    { "two int32s with one given", 0, "ii", BODY("\1\0\0\0") },
    { "an int32 and four bytes more", 0, "i", BODY("\1\0\0\0\2\0\0\0") },
    { "a byte, padding and an int32", 1, "yi", BODY("\1\0\0\0\5\0\0\0") },
    { "nonzero padding before an int32", 0, "yi", BODY("\1\7\0\0\5\0\0\0") },
    { "boolean 1", 1, "b", BODY("\1\0\0\0") },
    { "boolean 2", 0, "b", BODY("\2\0\0\0") },
    { "a double", 1, "d", BODY("\0\0\0\0\0\0\xf8\x3f") },
    { "a byte array past the end", 0, "ay", BODY("\x64\0\0\0\1\2\3") },
    { "an int32 array of six bytes", 0, "ai", BODY("\6\0\0\0\1\0\0\0\2\0") },
    { "an array over 64 MiB", 0, "ay", BODY("\1\0\0\4") },
    { "an empty uint64 array, padded", 1, "ax", BODY("\0\0\0\0\0\0\0\0") },
    { "an empty uint64 array, unpadded", 0, "ax", BODY("\0\0\0\0") },
    { "an empty uint64 array, nonzero padding", 0, "ax", BODY("\0\0\0\0\1\0\0\0") },
    { "an empty struct array and a byte", 1, "a(yy)y", BODY("\0\0\0\0\0\0\0\0\x09") },
    { "a dict a{sv}", 1, "a{sv}", BODY("\x0a\0\0\0\0\0\0\0\1\0\0\0k\0\1y\0\x09") },
    { "a dict whose length cuts an entry", 0, "a{sv}",
      BODY("\x09\0\0\0\0\0\0\0\1\0\0\0k\0\1y\0\x09") },
    { "a struct of two bytes", 1, "(yy)", BODY("\1\2") },
    { "a string", 1, "s", BODY("\2\0\0\0hi\0") },
    { "a string without its NUL", 0, "s", BODY("\2\0\0\0hix") },
    { "a string past the end", 0, "s", BODY("\x10\0\0\0hi\0") },
    { "a string with a NUL inside", 0, "s", BODY("\3\0\0\0a\0b\0") },
    { "a string with bad UTF-8", 0, "s", BODY("\2\0\0\0\xff\xfe\0") },
    { "a string with an overlong NUL", 0, "s", BODY("\2\0\0\0\xc0\x80\0") },
    { "a string with a surrogate", 0, "s", BODY("\3\0\0\0\xed\xa0\x80\0") },
    { "a string of two-byte UTF-8", 1, "s", BODY("\2\0\0\0\xc3\xa9\0") },
    { "the root path", 1, "o", BODY("\1\0\0\0/\0") },
    { "an empty path", 0, "o", BODY("\0\0\0\0\0") },
    { "a relative path", 0, "o", BODY("\3\0\0\0a/b\0") },
    { "a path ending in /", 0, "o", BODY("\3\0\0\0/a/\0") },
    { "a path with //", 0, "o", BODY("\3\0\0\0//a\0") },
    { "a path with a dash", 0, "o", BODY("\4\0\0\0/a-b\0") },
    { "a signature", 1, "g", BODY("\5a{sv}\0") },
    { "an incomplete signature", 0, "g", BODY("\1a\0") },
    { "a signature without its NUL", 0, "g", BODY("\1ix") },
    { "a variant", 1, "v", BODY("\1y\0\x09") },
    { "a variant of two types", 0, "v", BODY("\2ii\0\1\0\0\0\2\0\0\0") },
    { "a variant of no type", 0, "v", BODY("\0\0") },
    { "a variant holding boolean 2", 0, "v", BODY("\1b\0\0\2\0\0\0") },
    { "a unix fd without a count", 0, "h", BODY("\0\0\0\0") },
    { "signature a", 0, "a", BODY("\0\0\0\0") },
    { "signature (i", 0, "(i", BODY("\1\0\0\0") },
    { "signature i)", 0, "i)", BODY("\1\0\0\0") },
    { "a dict entry outside an array", 0, "{ys}", BODY("\1\0\0\0\0\0\0\0\0") },
    { "a dict keyed by a variant", 0, "a{vs}", BODY("\0\0\0\0\0\0\0\0") },
    { "a dict entry of one type", 0, "a{s}", BODY("\0\0\0\0\0\0\0\0") },
    { "a dict entry of three types", 0, "a{sss}", BODY("\0\0\0\0\0\0\0\0") },
    { "an empty struct", 0, "()", BODY("\0") },
    { "an unknown type code", 0, "z", BODY("\0") },
};

static void
body_case(frame *f, int big, const char *sig, const uint8_t *body, size_t len)
{
    field fl[4] = { FP("/org/x"), FI("org.x.Y"), FM("Do"), FG(NULL) };

    fl[3].s = sig;
    make_frame(f, big, GIBUS_MESSAGE_METHOD_CALL, 0, 1, fl, sig && sig[0] ? 4 : 3, body, len);
}

static void
deep_case(const char *what, int want, const char *open, const char *close, int depth,
          const uint8_t *body, size_t len)
{
    char sig[256];
    size_t n = 0;
    int i;
    frame f;

    for (i = 0; i < depth; i++) n += copy_str(sig + n, open, sizeof(sig) - n);
    n += copy_str(sig + n, "y", sizeof(sig) - n);
    for (i = 0; close && i < depth; i++) n += copy_str(sig + n, close, sizeof(sig) - n);
    body_case(&f, 0, sig, body, len);
    expect_parse(what, want, f.b, f.n);
}

static void
variant_case(const char *what, int want, int depth, const char *sig)
{
    uint8_t body[1024];
    wr w;
    warr a;
    frame f;
    int i;

    w_init(&w, body, sizeof(body), 0);
    for (i = 0; i < depth; i++) w_sig(&w, i + 1 < depth ? "v" : sig);
    if (sig[0] == 'y') {
        w_u8(&w, 5);
    } else {
        a = w_open(&w, sig[1] == '(' ? 8 : sig[1] == 's' ? 4 : 1);
        if (sig[1] == 'y') {
            w_u8(&w, 1);
            w_u8(&w, 2);
        } else if (sig[1] == 's') {
            w_str(&w, "x");
        }
        w_close(&w, a);
    }
    body_case(&f, 0, "v", body, w.n);
    expect_parse(what, want, f.b, f.n);
}

static void
variant_build(const char *what, int want, int depth, const char *elem, int n)
{
    static const uint8_t bytes[2] = { 1, 2 };
    gibus_msg *m = gibus_msg_signal("/org/x", "org.x.Y", "Do"), *p = NULL;
    gibus_iter it[70], arr;
    const char *s = "s";
    char sig[8];
    int i, built = m != NULL;

    snprintf(sig, sizeof(sig), "a%s", elem);
    if (m) gibus_iter_init_append(m, &it[0]);
    for (i = 0; built && i < depth; i++)
        built = gibus_iter_open(&it[i], GIBUS_TYPE_VARIANT, i + 1 < depth ? "v" : sig, &it[i + 1]);
    if (built) built = gibus_iter_open(&it[depth], GIBUS_TYPE_ARRAY, elem, &arr);
    if (built && n && elem[0] == 'y')
        built = gibus_iter_append_fixed_array(&arr, GIBUS_TYPE_BYTE, bytes, n);
    for (i = 0; built && elem[0] == 's' && i < n; i++)
        built = gibus_iter_append_basic(&arr, GIBUS_TYPE_STRING, &s);
    if (built) built = gibus_iter_close(&it[depth], &arr);
    for (i = depth; built && i > 0; i--) built = gibus_iter_close(&it[i - 1], &it[i]);
    if (built) p = reparse(m, 1);
    ok(what, want ? built && p != NULL : !built);
    drop(p);
    drop(m);
}

static void
test_malformed(void)
{
    static const uint8_t zero4[4] = { 0, 0, 0, 0 }, one[1] = { 1 };
    size_t i;
    frame f, g;

    puts("");
    puts("header fields");
    for (i = 0; i < COUNT(g_headers); i++) {
        const hrow *r = &g_headers[i];

        make_frame(&f, 0, r->type, 0, r->serial, r->f, r->nf, NULL, 0);
        expect_parse(r->what, r->want, f.b, f.n);
    }
    {
        static const field fl[] = { FP("/org/x"), FM("Do") };
        make_frame(&f, 0, GIBUS_MESSAGE_METHOD_CALL, 0, 1, fl, 2, zero4, 4);
        expect_parse("a body without a signature", 0, f.b, f.n);
    }

    puts("");
    puts("bodies against their signatures");
    for (i = 0; i < COUNT(g_bodies); i++) {
        const brow *r = &g_bodies[i];

        body_case(&f, 0, r->sig, (const uint8_t *)r->body, r->len);
        expect_parse(r->what, r->want, f.b, f.n);
    }
    body_case(&f, 1, "b", (const uint8_t *)"\0\0\0\1", 4);
    expect_parse("big-endian boolean 1", 1, f.b, f.n);
    body_case(&f, 1, "b", (const uint8_t *)"\0\0\0\2", 4);
    expect_parse("big-endian boolean 2", 0, f.b, f.n);
    body_case(&f, 1, "s", (const uint8_t *)"\0\0\0\2hi\0", 7);
    expect_parse("a big-endian string", 1, f.b, f.n);
    body_case(&f, 1, "s", (const uint8_t *)"\2\0\0\0hi\0", 7);
    expect_parse("a big-endian string of little-endian length", 0, f.b, f.n);

    puts("");
    puts("nesting");
    deep_case("32 nested arrays", 1, "a", NULL, 32, zero4, 4);
    deep_case("33 nested arrays", 0, "a", NULL, 33, zero4, 4);
    deep_case("32 nested structs", 1, "(", ")", 32, one, 1);
    deep_case("33 nested structs", 0, "(", ")", 33, one, 1);
    variant_case("10 nested variants", 1, 10, "y");
    variant_case("64 nested variants", 1, 64, "y");
    variant_case("64 nested variants holding an ay", 1, 64, "ay");
    variant_case("64 nested variants holding an empty a(ii)", 1, 64, "a(ii)");
    variant_case("64 nested variants holding an as", 0, 64, "as");
    variant_case("65 nested variants", 0, 65, "y");
    variant_case("65 nested variants holding an ay", 0, 65, "ay");
    variant_case("100 nested variants", 0, 100, "y");
    variant_build("an ay inside 64 variants builds", 1, 64, "y", 2);
    variant_build("an empty a(ii) inside 64 variants builds", 1, 64, "(ii)", 0);
    variant_build("an empty as inside 64 variants builds", 1, 64, "s", 0);
    variant_build("a string in an as inside 64 variants does not", 0, 64, "s", 1);
    variant_build("an empty ay inside 65 variants does not", 0, 65, "y", 0);

    puts("");
    puts("framing");
    body_case(&g, 0, "i", (const uint8_t *)"\5\0\0\0", 4);
    expect_parse("the frame these are made from", 1, g.b, g.n);
    f = g;
    f.b[0] = 'x';
    expect_parse("byte order x", 0, f.b, f.n);
    f = g;
    f.b[3] = 2;
    expect_parse("protocol version 2", 0, f.b, f.n);
    expect_parse("the frame one byte short", 0, g.b, g.n - 1);
    f = g;
    f.b[f.n] = 0;
    expect_parse("the frame and one byte more", 0, f.b, f.n + 1);
    expect_parse("nothing", 0, g.b, 0);
    expect_parse("16 bytes of it", 0, g.b, 16);
    ok("(the fields end short of the body)", g.fields_end < g.body_at);
    f = g;
    f.b[f.body_at - 1] = 1;
    expect_parse("nonzero padding before the body", 0, f.b, f.n);
    ok("(the first field ends short of the second)", g.first_end % 8 != 0);
    f = g;
    f.b[f.first_end] = 1;
    expect_parse("nonzero padding between fields", 0, f.b, f.n);
    f = g;
    f.b[12] = 0;
    f.b[13] = 0x10;
    expect_parse("fields longer than the frame", 0, f.b, f.n);
    f = g;
    f.b[12] = (uint8_t)(f.fields_end - 16 - 1);
    expect_parse("fields that cut the last one", 0, f.b, f.n);
    f = g;
    f.b[4] = 3;
    expect_parse("a body length one short", 0, f.b, f.n - 1);
}

static void
test_frame_size(void)
{
    frame g, f;
    size_t have;
    int early = 0, late = 0;

    puts("");
    puts("frame size from partial input");
    body_case(&g, 0, "s", (const uint8_t *)"\2\0\0\0hi\0", 7);
    for (have = 0; have < 16; have++)
        if (gibus__frame_size(g.b, have) != 0) early++;
    for (have = 16; have <= g.n; have++)
        if (gibus__frame_size(g.b, have) != (long)g.n) late++;
    ok("0 for each of the first 15 bytes", early == 0);
    ok("the whole length from 16 bytes on", late == 0);
    rich_frame(&f, 1, 7, 0);
    test_eq("big-endian too", gibus__frame_size(f.b, 16), (long)f.n);
    f = g;
    f.b[0] = 'x';
    test_eq("-1 for byte order x", gibus__frame_size(f.b, 16), -1);
    f = g;
    f.b[3] = 2;
    test_eq("-1 for protocol version 2", gibus__frame_size(f.b, 16), -1);
    f = g;
    f.b[1] = 0;
    test_eq("-1 for message type 0", gibus__frame_size(f.b, 16), -1);
    f = g;
    f.b[8] = f.b[9] = f.b[10] = f.b[11] = 0;
    test_eq("-1 for serial 0", gibus__frame_size(f.b, 16), -1);
    f = g;
    f.b[1] = 9;
    test_eq("the size of a frame of an unknown type", gibus__frame_size(f.b, f.n), (long)f.n);
    ok("which does not parse", !gibus_msg_parse(f.b, f.n));
    f = g;
    f.b[4] = 0;
    f.b[5] = 0;
    f.b[6] = 0x40;
    f.b[7] = 6;
    test_eq("a 100 MiB body, before it arrives", gibus__frame_size(f.b, 16),
            (long)(g.body_at + 0x06400000u));
    f = g;
    f.b[4] = f.b[5] = f.b[6] = 0xff;
    f.b[7] = 0x7f;
    test_eq("-1 for a body over 128 MiB", gibus__frame_size(f.b, 16), -1);
    f = g;
    f.b[12] = f.b[13] = f.b[14] = 0;
    f.b[15] = 5;
    test_eq("-1 for fields over 64 MiB", gibus__frame_size(f.b, 16), -1);
}

static uint64_t g_rng = 0x9e3779b97f4a7c15ull;

static uint32_t
rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static int
survive(const uint8_t *buf, size_t len)
{
    static char d1[DUMP_CAP], d2[DUMP_CAP];
    gibus_msg *m = gibus_msg_parse(buf, len), *m2 = NULL;
    const uint8_t *again;
    const char *s = NULL;
    size_t n2 = 0;
    int res = 1;

    if (!m) return 0;
    dump(m, d1, sizeof(d1));
    gibus_msg_get_args(m, NULL, GIBUS_TYPE_STRING, &s, GIBUS_TYPE_INVALID);
    if (gibus__frame_size(buf, len) != (long)len) res = -1;
    again = gibus_msg_bytes(m, 5, &n2);
    if (!again || !(m2 = gibus_msg_parse(again, n2))) {
        res = -1;
    } else {
        dump(m2, d2, sizeof(d2));
        if (strcmp(d1, d2) != 0 || gibus_msg_type(m) != gibus_msg_type(m2) ||
            gibus_msg_serial(m2) != 5 || gibus_msg_reply_serial(m) != gibus_msg_reply_serial(m2) ||
            !same(gibus_msg_signature(m), gibus_msg_signature(m2)) ||
            !same(gibus_msg_path(m), gibus_msg_path(m2)) ||
            !same(gibus_msg_interface(m), gibus_msg_interface(m2)) ||
            !same(gibus_msg_member(m), gibus_msg_member(m2)) ||
            !same(gibus_msg_error_name(m), gibus_msg_error_name(m2)) ||
            !same(gibus_msg_sender(m), gibus_msg_sender(m2)) ||
            !same(gibus_msg_destination(m), gibus_msg_destination(m2)))
            res = -1;
    }
    drop(m2);
    drop(m);
    return res;
}

static void
mutate(const char *what, const uint8_t *base, size_t len, int rounds)
{
    uint8_t buf[FRAME_CAP + 16];
    int r, parsed = 0, refused = 0, wrong = 0;
    char line[160];

    if (len > FRAME_CAP) len = FRAME_CAP;
    for (r = 0; r < rounds; r++) {
        size_t n = len;
        int k, ops = 1 + (int)(rnd() % 3), res;

        memcpy(buf, base, len);
        for (k = 0; k < ops; k++) {
            uint32_t op = rnd() % 8;

            if (op < 4 && n)
                buf[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8));
            else if (op < 6 && n)
                buf[rnd() % n] = (uint8_t)rnd();
            else if (op == 6)
                n = rnd() % (n + 1);
            else if (n < sizeof(buf))
                buf[n++] = (uint8_t)rnd();
        }
        res = survive(buf, n);
        if (res > 0) parsed++;
        if (res == 0) refused++;
        if (res < 0) wrong++;
    }
    snprintf(line, sizeof(line), "%s: %d taken, %d refused", what, parsed, refused);
    ok(line, wrong == 0 && parsed + refused == rounds);
    if (wrong) printf("    %d mutants parsed but did not serialize back the same\n", wrong);
    ok("  some of each", parsed > 0 && refused > 0);
}

static void
test_mutation(void)
{
    frame f;
    gibus_msg *m, *e, *call;
    const uint8_t *b;
    size_t n = 0;
    int r, wrong = 0, parsed = 0;

    puts("");
    puts("mutated messages either parse and serialize back, or are refused");
    rich_frame(&f, 1, 0x01020304u, 0);
    mutate("the big-endian call", f.b, f.n, 3000);
    rich_frame(&f, 0, 0x01020304u, 0);
    mutate("the little-endian call", f.b, f.n, 3000);
    m = rich_built();
    b = m ? gibus_msg_bytes(m, 11, &n) : NULL;
    if (b) mutate("the built call", b, n, 3000);
    drop(m);
    {
        static const field fl[] = { FP("/org/gibus/Obj"), FM("Ask"), FS(":1.42") };

        make_frame(&f, 0, GIBUS_MESSAGE_METHOD_CALL, 0, 77, fl, 3, NULL, 0);
        call = gibus_msg_parse(f.b, f.n);
        e = call ? gibus_msg_error(call, "org.gibus.Error.Bad", "went wrong") : NULL;
        b = e ? gibus_msg_bytes(e, 12, &n) : NULL;
        if (b) mutate("an error reply", b, n, 2000);
        drop(e);
        drop(call);
    }

    for (r = 0; r < 3000; r++) {
        uint8_t buf[320];
        size_t len = rnd() % sizeof(buf), i;
        int res;

        for (i = 0; i < len; i++) buf[i] = (uint8_t)rnd();
        if (len >= 4 && (r & 1)) {
            buf[0] = (r & 2) ? 'B' : 'l';
            buf[1] = (uint8_t)(1 + rnd() % 4);
            buf[3] = 1;
        }
        res = survive(buf, len);
        if (res < 0) wrong++;
        if (res > 0) parsed++;
    }
    ok("3000 random buffers never parse inconsistently", wrong == 0);
    printf("    (%d of them parsed)\n", parsed);
}

#ifdef GIBUSTEST_LIVE

typedef struct thread {
    pthread_t id;
} thread;

static thread *
spawn(thread *t, void *(*fn)(void *), void *data)
{
    return pthread_create(&t->id, NULL, fn, data) == 0 ? t : NULL;
}

static uint64_t
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void
sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static int
aget(int *v)
{
    return __atomic_load_n(v, __ATOMIC_SEQ_CST);
}

static void
aset(int *v, int x)
{
    __atomic_store_n(v, x, __ATOMIC_SEQ_CST);
}

static void
aadd(int *v, int x)
{
    __atomic_fetch_add(v, x, __ATOMIC_SEQ_CST);
}

#define LIVE_NAME   "org.gibus.DbusTest"
#define LIVE_IFACE  "org.gibus.Test"
#define LIVE_PATH   "/org/gibus/Echo"
#define LIVE_TREE   "/org/gibus/Tree"
#define LIVE_BOOM   "org.gibus.Test.Error.Boom"
#define LIVE_GREEDY "/org/gibus/Greedy"
#define LIVE_DROP   "/org/gibus/Drop"
#define LIVE_PEER   "org.freedesktop.DBus.Peer"
#define SEND_ROUNDS 200

static pid_t g_daemon = -1;

static int
start_daemon(char *addr, size_t cap)
{
    int fds[2], status = 0;
    size_t n = 0;
    uint64_t deadline;
    char *nl;
    pid_t pid;

    addr[0] = 0;
    if (pipe(fds) != 0) return -1;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        char arg[64];

        close(fds[0]);
        snprintf(arg, sizeof(arg), "--print-address=%d", fds[1]);
        execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork", arg, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    deadline = now_ms() + 10000;
    while (n + 1 < cap) {
        struct pollfd p;
        uint64_t now = now_ms();
        ssize_t k;
        int pr;

        if (now >= deadline) break;
        p.fd = fds[0];
        p.events = POLLIN;
        p.revents = 0;
        pr = poll(&p, 1, (int)(deadline - now));
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) break;
        k = read(fds[0], addr + n, cap - 1 - n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        n += (size_t)k;
        addr[n] = 0;
        if (strchr(addr, '\n')) break;
    }
    close(fds[0]);
    addr[n] = 0;
    if ((nl = strchr(addr, '\n')) != NULL) *nl = 0;
    if (addr[0]) {
        g_daemon = pid;
        return 1;
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 127 ? 0 : -1;
}

static void
stop_daemon(void)
{
    int status;

    if (g_daemon <= 0) return;
    kill(g_daemon, SIGKILL);
    while (waitpid(g_daemon, &status, 0) < 0 && errno == EINTR)
        ;
    g_daemon = -1;
}

typedef struct live {
    gibus_conn   *a, *b;
    int           stop_a, stop_b, a_done, b_done;
    int           ticks, others, tocks, last_tick;
    int           greedy, dropped_a, dropped_b;
} live;

static live L;

static void
send_reply(gibus_conn *c, gibus_msg *r)
{
    if (!r) return;
    gibus_send(c, r, NULL);
    drop(r);
}

static int
on_echo(gibus_conn *c, gibus_msg *m, void *user)
{
    gibus_msg *r = NULL;

    (void)user;
    if (gibus_msg_is_call(m, LIVE_IFACE, "Echo")) {
        const char *s = NULL;
        int32_t i = 0, j = 0;
        const void *p = NULL;
        int n = 0;

        if (gibus_msg_has_signature(m, "si") &&
            gibus_msg_get_args(m, NULL, GIBUS_TYPE_STRING, &s, GIBUS_TYPE_INT32, &i,
                               GIBUS_TYPE_INVALID)) {
            if ((r = gibus_msg_return(m)) != NULL)
                gibus_msg_append_args(r, GIBUS_TYPE_STRING, &s, GIBUS_TYPE_INT32, &i,
                                      GIBUS_TYPE_INVALID);
        } else if (gibus_msg_has_signature(m, "ii") &&
                   gibus_msg_get_args(m, NULL, GIBUS_TYPE_INT32, &i, GIBUS_TYPE_INT32, &j,
                                      GIBUS_TYPE_INVALID)) {
            if ((r = gibus_msg_return(m)) != NULL)
                gibus_msg_append_args(r, GIBUS_TYPE_INT32, &i, GIBUS_TYPE_INT32, &j,
                                      GIBUS_TYPE_INVALID);
        } else if (gibus_msg_has_signature(m, "ay") &&
                   gibus_msg_get_args(m, NULL, GIBUS_TYPE_ARRAY, GIBUS_TYPE_BYTE, &p, &n,
                                      GIBUS_TYPE_INVALID)) {
            gibus_iter it, a;

            if ((r = gibus_msg_return(m)) != NULL) {
                gibus_iter_init_append(r, &it);
                gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "y", &a);
                gibus_iter_append_fixed_array(&a, GIBUS_TYPE_BYTE, p, n);
                gibus_iter_close(&it, &a);
            }
        } else {
            r = gibus_msg_error(m, GIBUS_ERROR_INVALID_ARGS, "Echo takes si, ii or ay");
        }
        send_reply(c, r);
        return GIBUS_HANDLED;
    }
    if (gibus_msg_is_call(m, LIVE_IFACE, "Fail")) {
        send_reply(c, gibus_msg_error(m, LIVE_BOOM, "boom"));
        return GIBUS_HANDLED;
    }
    if (gibus_msg_is_call(m, LIVE_IFACE, "Hang")) return GIBUS_HANDLED;
    return GIBUS_NOT_YET_HANDLED;
}

static int
on_tree(gibus_conn *c, gibus_msg *m, void *user)
{
    gibus_msg *r;
    const char *path = gibus_msg_path(m);

    (void)user;
    if (!gibus_msg_is_call(m, LIVE_IFACE, "Where")) return GIBUS_NOT_YET_HANDLED;
    if ((r = gibus_msg_return(m)) != NULL)
        gibus_msg_append_args(r, GIBUS_TYPE_STRING, &path, GIBUS_TYPE_INVALID);
    send_reply(c, r);
    return GIBUS_HANDLED;
}

static int
on_b_signal(gibus_conn *c, gibus_msg *m, void *user)
{
    int32_t v = 0;

    (void)c;
    (void)user;
    if (gibus_msg_is_signal(m, LIVE_IFACE, "Tick")) {
        if (gibus_msg_get_args(m, NULL, GIBUS_TYPE_INT32, &v, GIBUS_TYPE_INVALID))
            aset(&L.last_tick, v);
        aadd(&L.ticks, 1);
    } else if (gibus_msg_is_signal(m, LIVE_IFACE, "Other")) {
        aadd(&L.others, 1);
    }
    return GIBUS_NOT_YET_HANDLED;
}

static int
on_a_signal(gibus_conn *c, gibus_msg *m, void *user)
{
    (void)c;
    (void)user;
    if (gibus_msg_is_signal(m, LIVE_IFACE, "Tock")) aadd(&L.tocks, 1);
    return GIBUS_NOT_YET_HANDLED;
}

static int
on_greedy(gibus_conn *c, gibus_msg *m, void *user)
{
    (void)c;
    (void)m;
    (void)user;
    aadd(&L.greedy, 1);
    return GIBUS_HANDLED;
}

static int
on_drop(gibus_conn *c, gibus_msg *m, void *user)
{
    if (!gibus_msg_is_call(m, LIVE_IFACE, "Drop")) return GIBUS_NOT_YET_HANDLED;
    gibus_unref(c);
    aadd((int *)user, 1);
    return GIBUS_NOT_YET_HANDLED;
}

static int
introspect_b(const char *path, char *out, size_t cap)
{
    gibus_msg *m = gibus_msg_call(LIVE_NAME, path, GIBUS_INTERFACE_INTROSPECTABLE, "Introspect");
    gibus_msg *r;
    gibus_error err;
    const char *s = NULL;
    int got;

    out[0] = 0;
    gibus_error_init(&err);
    r = m ? gibus_call(L.a, m, 5000, &err) : NULL;
    got = r && gibus_msg_get_args(r, NULL, GIBUS_TYPE_STRING, &s, GIBUS_TYPE_INVALID) && s;
    if (got) copy_str(out, s, cap);
    drop(r);
    drop(m);
    gibus_error_free(&err);
    return got;
}

static int
count_of(const char *s, const char *needle)
{
    int n = 0;

    while ((s = strstr(s, needle)) != NULL) {
        n++;
        s += strlen(needle);
    }
    return n;
}

static gibus_conn *
open_dropper(const char *addr, int *dropped)
{
    gibus_error err;
    gibus_conn *c;
    gibus_msg *m;

    gibus_error_init(&err);
    c = gibus_open(addr, &err);
    gibus_error_free(&err);
    if (!c) return NULL;
    if (!gibus_register_path(c, LIVE_DROP, on_drop, dropped) ||
        !(m = gibus_msg_call(gibus_unique_name(c), LIVE_DROP, LIVE_IFACE, "Drop"))) {
        gibus_unref(c);
        return NULL;
    }
    gibus_send(L.a, m, NULL);
    gibus_flush(L.a);
    drop(m);
    return c;
}

#define FAKE_ID "0123456789abcdef0123456789abcdef"

typedef struct fake {
    int           listen_fd;
    int           go, done, release;
    int           reply_type;
    uint32_t      reply_serial;
} fake;

static void
bad_frame(frame *f)
{
    static const field fl[] = { FP("/org/gibus/Fake"), FI(LIVE_IFACE), FM("Poke"),
                                FS(":1.0"), FD(":1.1"), FG("s") };
    uint8_t body[16];
    wr w;

    w_init(&w, body, sizeof(body), 0);
    w_uint(&w, 1000, 4);
    w_u8(&w, 'a');
    w_u8(&w, 0);
    make_frame(f, 0, GIBUS_MESSAGE_METHOD_CALL, 0, 2, fl, (int)COUNT(fl), body, w.n);
}

static int
fake_read(int fd, void *buf, size_t n, uint64_t end)
{
    uint8_t *b = (uint8_t *)buf;
    size_t got = 0;

    while (got < n) {
        struct pollfd p;
        uint64_t now = now_ms();
        ssize_t k;
        int pr;

        if (now >= end) return 0;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        pr = poll(&p, 1, (int)(end - now));
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) return 0;
        k = read(fd, b + got, n - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        got += (size_t)k;
    }
    return 1;
}

static int
fake_line(int fd, char *line, size_t cap, uint64_t end)
{
    size_t n = 0;

    while (n + 1 < cap) {
        if (!fake_read(fd, line + n, 1, end)) return 0;
        n++;
        if (n >= 2 && line[n - 2] == '\r' && line[n - 1] == '\n') {
            line[n - 2] = 0;
            return 1;
        }
    }
    return 0;
}

static int
fake_write(int fd, const void *buf, size_t n)
{
    const uint8_t *b = (const uint8_t *)buf;

    while (n) {
        ssize_t k = write(fd, b, n);

        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        b += k;
        n -= (size_t)k;
    }
    return 1;
}

static gibus_msg *
fake_recv(int fd, uint64_t end)
{
    uint8_t b[FRAME_CAP];
    long n;

    if (!fake_read(fd, b, 16, end)) return NULL;
    n = gibus__frame_size(b, 16);
    if (n < 16 || n > FRAME_CAP) return NULL;
    if (!fake_read(fd, b + 16, (size_t)n - 16, end)) return NULL;
    return gibus_msg_parse(b, (size_t)n);
}

static void *
fake_bus(void *data)
{
    static const char ok_line[] = "OK " FAKE_ID "\r\n";
    static const field poke[] = { FP("/org/gibus/Fake"), FI(LIVE_IFACE), FM("Poke"),
                                  FS(":1.0"), FD(":1.1") };
    fake *f = (fake *)data;
    uint64_t end = now_ms() + 10000;
    struct pollfd p;
    char line[256];
    uint8_t body[16];
    frame fr;
    gibus_msg *m;
    uint32_t hello;
    wr w;
    int fd = -1;

    p.fd = f->listen_fd;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, 10000) <= 0 || (fd = accept(f->listen_fd, NULL, NULL)) < 0) goto out;
    if (!fake_read(fd, line, 1, end) || line[0] != 0) goto out;
    if (!fake_line(fd, line, sizeof(line), end) || strncmp(line, "AUTH EXTERNAL ", 14) != 0)
        goto out;
    if (!fake_write(fd, ok_line, sizeof(ok_line) - 1)) goto out;
    if (!fake_line(fd, line, sizeof(line), end) || strcmp(line, "BEGIN") != 0) goto out;
    if (!(m = fake_recv(fd, end))) goto out;
    hello = gibus_msg_is_call(m, GIBUS_INTERFACE_DBUS, "Hello") ? gibus_msg_serial(m) : 0;
    drop(m);
    if (!hello) goto out;
    {
        field fl[] = { FR(0), FD(":1.1"), FS(GIBUS_SERVICE_DBUS), FG("s") };

        fl[0].u = hello;
        w_init(&w, body, sizeof(body), 0);
        w_str(&w, ":1.1");
        make_frame(&fr, 0, GIBUS_MESSAGE_METHOD_RETURN, 0, 1, fl, (int)COUNT(fl), body,
                   w.n);
    }
    if (!fake_write(fd, fr.b, fr.n)) goto out;
    while (!aget(&f->go) && now_ms() < end) sleep_ms(5);
    bad_frame(&fr);
    if (!fake_write(fd, fr.b, fr.n)) goto out;
    make_frame(&fr, 0, GIBUS_MESSAGE_METHOD_CALL, 0, 3, poke, (int)COUNT(poke), NULL, 0);
    if (!fake_write(fd, fr.b, fr.n)) goto out;
    if ((m = fake_recv(fd, end)) != NULL) {
        f->reply_type = gibus_msg_type(m);
        f->reply_serial = gibus_msg_reply_serial(m);
        drop(m);
    }
out:
    aset(&f->done, 1);
    while (!aget(&f->release) && now_ms() < end) sleep_ms(5);
    if (fd >= 0) close(fd);
    return NULL;
}

static void
test_fake_bus(void)
{
    char path[64], addr[96];
    struct sockaddr_un sa;
    gibus_error err;
    gibus_conn *c = NULL;
    thread fake_bus_t, *t = NULL;
    gibus_msg *m;
    frame bad;
    fake f;
    uint64_t end;

    puts("");
    puts("a bus that sends a frame whose body does not parse");
    bad_frame(&bad);
    m = gibus_msg_parse(bad.b, bad.n);
    ok("the frame has a size but does not parse",
       gibus__frame_size(bad.b, bad.n) == (long)bad.n && m == NULL);
    drop(m);

    memset(&f, 0, sizeof(f));
    memset(&sa, 0, sizeof(sa));
    snprintf(path, sizeof(path), "/tmp/gibus-test-%ld", (long)getpid());
    snprintf(addr, sizeof(addr), "unix:path=%s", path);
    sa.sun_family = AF_UNIX;
    copy_str(sa.sun_path, path, sizeof(sa.sun_path));
    unlink(path);
    f.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (f.listen_fd < 0 || bind(f.listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(f.listen_fd, 1) != 0) {
        printf("  skipped: cannot listen on %s\n", path);
        if (f.listen_fd >= 0) close(f.listen_fd);
        unlink(path);
        return;
    }
    t = spawn(&fake_bus_t, fake_bus, &f);
    ok("the bus thread starts", t != NULL);
    gibus_error_init(&err);
    if (t) c = gibus_open(addr, &err);
    ok("the connection opens", c != NULL);
    if (t && !c) printf("    %s: %s\n", err.name, err.message);
    gibus_error_free(&err);
    if (c) {
        aset(&f.go, 1);
        end = now_ms() + 5000;
        while (!aget(&f.done) && now_ms() < end) gibus_read_write_dispatch(c, 10);
        test_eq("the call after it is answered", f.reply_type, GIBUS_MESSAGE_ERROR);
        test_eq("with its serial", (long)f.reply_serial, 3);
        ok("and the connection stays", gibus_connected(c));
    }
    aset(&f.go, 1);
    aset(&f.release, 1);
    if (t) pthread_join(t->id, NULL);
    if (c) gibus_unref(c);
    close(f.listen_fd);
    unlink(path);
}

static void *
pump_a(void *data)
{
    (void)data;
    while (!aget(&L.stop_a))
        if (!gibus_read_write_dispatch(L.a, 20)) break;
    aset(&L.a_done, 1);
    return NULL;
}

static void *
pump_b(void *data)
{
    (void)data;
    while (!aget(&L.stop_b))
        if (!gibus_read_write_dispatch(L.b, 20)) break;
    aset(&L.b_done, 1);
    return NULL;
}

static int
wait_for(int *v, int want, int ms, gibus_conn *pump)
{
    uint64_t end = now_ms() + (uint64_t)ms;

    while (aget(v) < want) {
        if (now_ms() >= end) return 0;
        if (pump)
            gibus_read_write_dispatch(pump, 10);
        else
            sleep_ms(5);
    }
    return 1;
}

static gibus_msg *
call_b(const char *path, const char *member, gibus_error *err)
{
    gibus_msg *m = gibus_msg_call(LIVE_NAME, path, LIVE_IFACE, member), *r;

    gibus_error_init(err);
    if (!m) return NULL;
    r = gibus_call(L.a, m, 5000, err);
    drop(m);
    return r;
}

static void
send_signal(gibus_conn *c, const char *member, int32_t v)
{
    gibus_msg *m = gibus_msg_signal("/org/gibus/Src", LIVE_IFACE, member);

    if (!m) return;
    gibus_msg_append_args(m, GIBUS_TYPE_INT32, &v, GIBUS_TYPE_INVALID);
    gibus_send(c, m, NULL);
    drop(m);
}

typedef struct sender {
    int32_t id;
    int     bad;
} sender;

static void *
send_many(void *data)
{
    sender *s = (sender *)data;
    int32_t i;

    for (i = 0; i < SEND_ROUNDS; i++) {
        gibus_msg *m = gibus_msg_call(LIVE_NAME, LIVE_PATH, LIVE_IFACE, "Echo"), *r = NULL;
        gibus_msg *sig;
        gibus_error err;
        int32_t ra = -1, rb = -1;

        gibus_error_init(&err);
        if (m && gibus_msg_append_args(m, GIBUS_TYPE_INT32, &s->id, GIBUS_TYPE_INT32, &i,
                                       GIBUS_TYPE_INVALID))
            r = gibus_call(L.a, m, 10000, &err);
        if (!r || !gibus_msg_get_args(r, NULL, GIBUS_TYPE_INT32, &ra, GIBUS_TYPE_INT32, &rb,
                                      GIBUS_TYPE_INVALID) || ra != s->id || rb != i)
            s->bad++;
        drop(r);
        drop(m);
        gibus_error_free(&err);
        sig = gibus_msg_signal("/org/gibus/Src", LIVE_IFACE, "Tick");
        if (!sig || !gibus_msg_append_args(sig, GIBUS_TYPE_INT32, &i, GIBUS_TYPE_INVALID) ||
            !gibus_send(L.a, sig, NULL))
            s->bad++;
        drop(sig);
    }
    return NULL;
}

typedef struct hang {
    int           done;
    gibus_msg    *reply;
    char          name[128];
} hang;

static void *
hang_call(void *data)
{
    hang *h = (hang *)data;
    gibus_msg *m = gibus_msg_call(LIVE_NAME, LIVE_PATH, LIVE_IFACE, "Hang");
    gibus_error err;

    gibus_error_init(&err);
    if (m) h->reply = gibus_call(L.a, m, 30000, &err);
    drop(m);
    copy_str(h->name, gibus_error_is_set(&err) ? err.name : "", sizeof(h->name));
    gibus_error_free(&err);
    aset(&h->done, 1);
    return NULL;
}

static int
joined_within(thread *t, int *done, int ms)
{
    uint64_t end = now_ms() + (uint64_t)ms;

    while (!aget(done) && now_ms() < end) sleep_ms(5);
    if (!aget(done)) {
        pthread_detach(t->id);
        return 0;
    }
    pthread_join(t->id, NULL);
    return 1;
}

typedef struct forever {
    gibus_conn   *c;
    int           done;
} forever;

static void *
forever_thread(void *data)
{
    forever *f = (forever *)data;

    while (gibus_read_write_dispatch(f->c, -1))
        ;
    aset(&f->done, 1);
    return NULL;
}

static gibus_msg *
big_echo(const uint8_t *bytes)
{
    gibus_msg *m = gibus_msg_call(LIVE_NAME, LIVE_PATH, LIVE_IFACE, "Echo");
    gibus_iter it, a;

    if (!m) return NULL;
    gibus_iter_init_append(m, &it);
    gibus_iter_open(&it, GIBUS_TYPE_ARRAY, "y", &a);
    gibus_iter_append_fixed_array(&a, GIBUS_TYPE_BYTE, bytes, BIG_LEN);
    gibus_iter_close(&it, &a);
    return m;
}

static int
echoed(gibus_msg *r, const uint8_t *bytes)
{
    const uint8_t *p = NULL;
    int n = 0;

    return r && gibus_msg_get_args(r, NULL, GIBUS_TYPE_ARRAY, GIBUS_TYPE_BYTE, &p, &n,
                                   GIBUS_TYPE_INVALID) &&
           n == BIG_LEN && p && memcmp(p, bytes, BIG_LEN) == 0;
}

#define FLOOD_KEEP  4096
#define FLOOD_SENT  (FLOOD_KEEP + 300)

typedef struct flood {
    int     got, bad;
    int32_t first, last;
} flood;

static int
on_flood(gibus_conn *c, gibus_msg *m, void *user)
{
    flood *f = (flood *)user;
    int32_t v = -1;

    (void)c;
    if (!gibus_msg_is_signal(m, LIVE_IFACE, "Flood")) return GIBUS_NOT_YET_HANDLED;
    if (!gibus_msg_get_args(m, NULL, GIBUS_TYPE_INT32, &v, GIBUS_TYPE_INVALID)) f->bad++;
    if (!f->got)
        f->first = v;
    else if (v != f->last + 1)
        f->bad++;
    f->last = v;
    f->got++;
    return GIBUS_HANDLED;
}

static int
bus_round_trip(gibus_conn *c)
{
    gibus_msg *m = gibus_msg_call(GIBUS_SERVICE_DBUS, GIBUS_PATH_DBUS, GIBUS_INTERFACE_DBUS,
                                  "GetId");
    gibus_msg *r;
    gibus_error err;

    gibus_error_init(&err);
    r = m ? gibus_call(c, m, 10000, &err) : NULL;
    gibus_error_free(&err);
    drop(m);
    if (!r) return 0;
    drop(r);
    return 1;
}

static void
test_flood(const char *addr)
{
    gibus_error err;
    gibus_conn *q;
    flood f;
    int32_t v;
    int ready;

    puts("");
    puts("more signals than are kept, received before any dispatch");
    memset(&f, 0, sizeof(f));
    gibus_error_init(&err);
    q = gibus_open(addr, &err);
    gibus_error_free(&err);
    gibus_error_init(&err);
    ready = q && gibus_add_filter(q, on_flood, &f) &&
            gibus_add_match(q, "type='signal',interface='" LIVE_IFACE "',member='Flood'", &err);
    gibus_error_free(&err);
    ok("a connection opens with a match and a filter", ready);
    if (!ready) {
        if (q) gibus_unref(q);
        return;
    }
    for (v = 0; v < FLOOD_SENT; v++) send_signal(L.a, "Flood", v);
    ok("A sends them all", gibus_flush(L.a) && bus_round_trip(L.a));
    ok("the connection reads them while it waits for a reply", bus_round_trip(q));
    while (gibus_dispatch(q))
        ;
    test_eq("dispatch finds as many as are kept", f.got, FLOOD_KEEP);
    test_eq("the oldest dropped", f.first, FLOOD_SENT - FLOOD_KEEP);
    test_eq("up to the newest", f.last, FLOOD_SENT - 1);
    test_eq("in order", f.bad, 0);
    ok("still connected", gibus_connected(q));

    memset(&f, 0, sizeof(f));
    for (v = 0; v < 3; v++) send_signal(L.a, "Flood", v);
    ok("after it, three more", gibus_flush(L.a) && bus_round_trip(L.a) && bus_round_trip(q));
    while (gibus_dispatch(q))
        ;
    ok("arrive as sent", f.got == 3 && f.first == 0 && f.last == 2 && !f.bad);
    gibus_unref(q);
}

static void
test_live(void)
{
    static char addr[1024];
    static uint8_t big[BIG_LEN];
    gibus_error err;
    gibus_msg *r, *m;
    gibus_conn *silent, *d;
    thread pump_b_t, pump_a_t, send_1_t, send_2_t, hang_t, forever_t;
    thread *tb, *ta, *t1, *t2, *th, *td;
    sender s1, s2;
    hang h;
    forever fv;
    const char *s = NULL;
    int32_t i = 0;
    int started, hung = 0;
    uint64_t t0, dt;
    size_t k;

    signal(SIGPIPE, SIG_IGN);
    test_fake_bus();
    puts("");
    puts("a private dbus-daemon");
    started = start_daemon(addr, sizeof(addr));
    if (started == 0) {
        puts("  skipped: no dbus-daemon");
        ok("a bus where --require-bus asks for one", !g_require_bus);
        return;
    }
    ok("starts and prints its address", started > 0);
    if (started < 0) return;
    printf("    %s\n", addr);

    memset(&L, 0, sizeof(L));
    gibus_error_init(&err);
    L.a = gibus_open(addr, &err);
    ok("the first connection opens", L.a != NULL);
    if (!L.a) printf("    %s: %s\n", err.name, err.message);
    gibus_error_free(&err);
    gibus_error_init(&err);
    L.b = gibus_open(addr, &err);
    ok("the second connection opens", L.b != NULL);
    if (!L.b) printf("    %s: %s\n", err.name, err.message);
    gibus_error_free(&err);
    if (!L.a || !L.b) {
        if (L.a) gibus_unref(L.a);
        if (L.b) gibus_unref(L.b);
        stop_daemon();
        return;
    }
    ok("each has a unique name", gibus_unique_name(L.a) && gibus_unique_name(L.a)[0] == ':' &&
                                     gibus_unique_name(L.b) && gibus_unique_name(L.b)[0] == ':');
    ok("and they differ", strcmp(gibus_unique_name(L.a), gibus_unique_name(L.b)) != 0);
    ok("both connected, with sockets", gibus_connected(L.a) && gibus_connected(L.b) &&
                                           gibus_fd(L.a) >= 0 && gibus_fd(L.b) >= 0);

    puts("");
    puts("names, handlers and matches");
    gibus_error_init(&err);
    test_eq("B asks for a name and owns it",
            gibus_request_name(L.b, LIVE_NAME, GIBUS_NAME_FLAG_DO_NOT_QUEUE, &err),
            GIBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
    gibus_error_free(&err);
    gibus_error_init(&err);
    test_eq("asking again, it already does",
            gibus_request_name(L.b, LIVE_NAME, GIBUS_NAME_FLAG_DO_NOT_QUEUE, &err), 4);
    gibus_error_free(&err);
    gibus_error_init(&err);
    test_eq("A asking for it without queueing finds it taken",
            gibus_request_name(L.a, LIVE_NAME, GIBUS_NAME_FLAG_DO_NOT_QUEUE, &err), 3);
    gibus_error_free(&err);
    gibus_error_init(&err);
    test_eq("an invalid name answers -1",
            gibus_request_name(L.a, "not a name", GIBUS_NAME_FLAG_DO_NOT_QUEUE, &err), -1);
    ok("with the error set", gibus_error_is_set(&err));
    gibus_error_free(&err);
    ok("B registers a path, a fallback and a filter",
       gibus_register_path(L.b, LIVE_PATH, on_echo, NULL) &&
           gibus_register_fallback(L.b, LIVE_TREE, on_tree, NULL) &&
           gibus_add_filter(L.b, on_b_signal, NULL));
    ok("and a path whose handler claims every call",
       gibus_register_path(L.b, LIVE_GREEDY, on_greedy, NULL));
    ok("A registers a filter", gibus_add_filter(L.a, on_a_signal, NULL));
    ok("a second handler for one path is refused",
       !gibus_register_path(L.b, LIVE_PATH, on_echo, NULL));
    gibus_error_init(&err);
    ok("B adds a match", gibus_add_match(L.b, "type='signal',interface='" LIVE_IFACE
                                              "',member='Tick'", &err));
    gibus_error_free(&err);
    gibus_error_init(&err);
    ok("A adds a match", gibus_add_match(L.a, "type='signal',interface='" LIVE_IFACE
                                              "',member='Tock'", &err));
    gibus_error_free(&err);
    gibus_error_init(&err);
    ok("an invalid rule is refused", !gibus_add_match(L.a, "type='nonsense'", &err) &&
                                         gibus_error_is_set(&err));
    gibus_error_free(&err);
    tb = spawn(&pump_b_t, pump_b, NULL);
    ok("B's dispatch thread starts", tb != NULL);
    if (!tb) {
        gibus_unref(L.a);
        gibus_unref(L.b);
        stop_daemon();
        return;
    }

    puts("");
    puts("calls, replies and errors");
    m = gibus_msg_call(LIVE_NAME, LIVE_PATH, LIVE_IFACE, "Echo");
    s = "hello";
    i = 42;
    if (m) gibus_msg_append_args(m, GIBUS_TYPE_STRING, &s, GIBUS_TYPE_INT32, &i, GIBUS_TYPE_INVALID);
    gibus_error_init(&err);
    r = m ? gibus_call(L.a, m, 5000, &err) : NULL;
    drop(m);
    ok("Echo is answered", r != NULL);
    if (!r && gibus_error_is_set(&err)) printf("    %s: %s\n", err.name, err.message);
    gibus_error_free(&err);
    s = NULL;
    i = 0;
    ok("with what it was sent", r && gibus_msg_get_args(r, NULL, GIBUS_TYPE_STRING, &s,
                                                        GIBUS_TYPE_INT32, &i,
                                                        GIBUS_TYPE_INVALID) &&
                                    same(s, "hello") && i == 42);
    test_str("from B's unique name", r ? gibus_msg_sender(r) : NULL, gibus_unique_name(L.b));
    test_str("to A's", r ? gibus_msg_destination(r) : NULL, gibus_unique_name(L.a));
    drop(r);

    r = call_b(LIVE_PATH, "Fail", &err);
    ok("Fail answers NULL", r == NULL);
    test_str("with its error name", err.name, LIVE_BOOM);
    test_str("and its text", err.message, "boom");
    gibus_error_free(&err);
    drop(r);

    r = call_b(LIVE_PATH, "Nope", &err);
    ok("a method B does not handle", r == NULL);
    test_str("is UnknownMethod", err.name, GIBUS_ERROR_UNKNOWN_METHOD);
    gibus_error_free(&err);
    drop(r);
    r = call_b("/nowhere", "Echo", &err);
    ok("a path B has no handler for", r == NULL);
    test_str("is UnknownMethod", err.name, GIBUS_ERROR_UNKNOWN_METHOD);
    gibus_error_free(&err);
    drop(r);

    m = gibus_msg_call(LIVE_NAME, "/any", LIVE_PEER, "Ping");
    gibus_error_init(&err);
    r = m ? gibus_call(L.a, m, 5000, &err) : NULL;
    ok("Peer.Ping is answered", r != NULL);
    gibus_error_free(&err);
    drop(r);
    drop(m);

    puts("");
    puts("Peer before the handlers, Introspect after them");
    m = gibus_msg_call(LIVE_NAME, LIVE_GREEDY, LIVE_PEER, "Ping");
    gibus_error_init(&err);
    r = m ? gibus_call(L.a, m, 5000, &err) : NULL;
    ok("Ping on a path whose handler claims every call", r != NULL);
    gibus_error_free(&err);
    drop(r);
    drop(m);
    m = gibus_msg_call(LIVE_NAME, LIVE_GREEDY, LIVE_PEER, "GetMachineId");
    gibus_error_init(&err);
    r = m ? gibus_call(L.a, m, 5000, &err) : NULL;
    s = NULL;
    if (r) gibus_msg_get_args(r, NULL, GIBUS_TYPE_STRING, &s, GIBUS_TYPE_INVALID);
    ok("and GetMachineId, with an ID or an error",
       r ? s && strlen(s) == 32
         : gibus_error_is_set(&err) && strcmp(err.name, GIBUS_ERROR_NO_REPLY) != 0);
    gibus_error_free(&err);
    drop(r);
    drop(m);
    test_eq("neither reaches the handler", aget(&L.greedy), 0);
    {
        static char xml[4096];

        ok("Introspect on a parent path is answered", introspect_b("/org", xml, sizeof(xml)));
        test_str("with its one child", xml,
                 GIBUS_INTROSPECT_DOCTYPE "<node>\n  <node name=\"gibus\"/>\n</node>\n");
        introspect_b("/org/gibus", xml, sizeof(xml));
        ok("one level down, every registered child",
           count_of(xml, "<node name=\"Echo\"/>") == 1 &&
               count_of(xml, "<node name=\"Tree\"/>") == 1 &&
               count_of(xml, "<node name=\"Greedy\"/>") == 1 && count_of(xml, "<node name=") == 3);
        introspect_b("/", xml, sizeof(xml));
        test_eq("at the root, org once", count_of(xml, "<node name=\"org\"/>"), 1);
        introspect_b(LIVE_PATH, xml, sizeof(xml));
        ok("a handled path with nothing below lists no child",
           xml[0] && count_of(xml, "<node name=") == 0);
    }

    puts("");
    puts("a handler that drops the connection's last reference");
    {
        gibus_conn *e1 = open_dropper(addr, &L.dropped_a);
        gibus_conn *e2 = open_dropper(addr, &L.dropped_b);
        uint64_t end = now_ms() + 5000;
        int gone;

        ok("two connections open, each called", e1 && e2);
        gone = e1 && wait_for(&L.dropped_a, 1, 5000, e1);
        ok("gibus_read_write_dispatch returns from it", gone);
        if (!gone && e1) gibus_unref(e1);
        while (!aget(&L.dropped_b) && e2 && now_ms() < end) {
            gibus_read_write(e2, 10);
            while (!aget(&L.dropped_b) && gibus_dispatch(e2))
                ;
        }
        gone = aget(&L.dropped_b) != 0;
        ok("and gibus_dispatch", gone);
        if (!gone && e2) gibus_unref(e2);
    }

    puts("");
    puts("a fallback handles its path and every path below");
    r = call_b(LIVE_TREE "/a/b", "Where", &err);
    s = NULL;
    if (r) gibus_msg_get_args(r, NULL, GIBUS_TYPE_STRING, &s, GIBUS_TYPE_INVALID);
    test_str("below it", s, LIVE_TREE "/a/b");
    gibus_error_free(&err);
    drop(r);
    r = call_b(LIVE_TREE, "Where", &err);
    s = NULL;
    if (r) gibus_msg_get_args(r, NULL, GIBUS_TYPE_STRING, &s, GIBUS_TYPE_INVALID);
    test_str("at it", s, LIVE_TREE);
    gibus_error_free(&err);
    drop(r);
    r = call_b(LIVE_TREE "house", "Where", &err);
    ok("but not a sibling that shares its prefix", r == NULL);
    test_str("which is UnknownMethod", err.name, GIBUS_ERROR_UNKNOWN_METHOD);
    gibus_error_free(&err);
    drop(r);

    puts("");
    puts("signals through a match and a filter");
    send_signal(L.a, "Other", 1);
    send_signal(L.a, "Tick", 7);
    gibus_flush(L.a);
    ok("B gets the Tick it matched", wait_for(&L.ticks, 1, 5000, NULL));
    test_eq("with its argument", aget(&L.last_tick), 7);
    test_eq("and not the signal it did not match", aget(&L.others), 0);
    send_signal(L.b, "Tock", 1);
    gibus_flush(L.b);
    ok("A gets B's Tock", wait_for(&L.tocks, 1, 5000, L.a));

    puts("");
    puts("a 1 MiB message");
    for (k = 0; k < BIG_LEN; k++) big[k] = (uint8_t)(k * 13 + 1);
    m = big_echo(big);
    gibus_error_init(&err);
    r = m ? gibus_call(L.a, m, 20000, &err) : NULL;
    drop(m);
    ok("goes there and back", r != NULL);
    if (!r && gibus_error_is_set(&err)) printf("    %s: %s\n", err.name, err.message);
    gibus_error_free(&err);
    ok("unchanged", echoed(r, big));
    drop(r);

    puts("");
    puts("a call to a peer that never answers");
    gibus_error_init(&err);
    silent = gibus_open(addr, &err);
    gibus_error_free(&err);
    ok("a third connection opens and stays silent", silent != NULL);
    if (silent) {
        m = gibus_msg_call(gibus_unique_name(silent), "/org/gibus/Silent", LIVE_IFACE, "Echo");
        gibus_error_init(&err);
        t0 = now_ms();
        r = m ? gibus_call(L.a, m, 300, &err) : NULL;
        dt = now_ms() - t0;
        ok("gibus_call answers NULL", r == NULL);
        test_str("with NoReply", err.name, GIBUS_ERROR_NO_REPLY);
        printf("    after %lu ms\n", (unsigned long)dt);
        ok("after its timeout, not long after", dt >= 290 && dt < 3000);
        gibus_error_free(&err);
        drop(r);
        drop(m);
        ok("the connection survives it", gibus_connected(L.a));
        gibus_unref(silent);
    }

    puts("");
    puts("two threads send on one connection while a third dispatches it");
    {
        int before = aget(&L.ticks);

        s1.id = 1;
        s1.bad = 0;
        s2.id = 2;
        s2.bad = 0;
        ta = spawn(&pump_a_t, pump_a, NULL);
        t1 = spawn(&send_1_t, send_many, &s1);
        t2 = spawn(&send_2_t, send_many, &s2);
        ok("the threads start", ta && t1 && t2);
        if (t1) pthread_join(t1->id, NULL);
        if (t2) pthread_join(t2->id, NULL);
        test_eq("every call answered with its own reply", s1.bad + s2.bad, 0);
        ok("every signal arrives", wait_for(&L.ticks, before + 2 * SEND_ROUNDS, 10000, NULL));
        aset(&L.stop_a, 1);
        if (ta) ok("the dispatch thread stops", joined_within(ta, &L.a_done, 5000));
    }
    test_flood(addr);

    puts("");
    puts("the daemon killed under waiting threads");
    memset(&h, 0, sizeof(h));
    memset(&fv, 0, sizeof(fv));
    gibus_error_init(&err);
    d = gibus_open(addr, &err);
    gibus_error_free(&err);
    fv.c = d;
    td = d ? spawn(&forever_t, forever_thread, &fv) : NULL;
    if (td) {
        sleep_ms(100);
        m = big_echo(big);
        gibus_error_init(&err);
        t0 = now_ms();
        r = m ? gibus_call(d, m, 20000, &err) : NULL;
        dt = now_ms() - t0;
        drop(m);
        ok("a 1 MiB call on a connection another thread waits on without end",
           echoed(r, big));
        if (!r && gibus_error_is_set(&err)) printf("    %s: %s\n", err.name, err.message);
        printf("    answered in %lu ms\n", (unsigned long)dt);
        gibus_error_free(&err);
        drop(r);
    }
    th = spawn(&hang_t, hang_call, &h);
    ok("a call that B never answers is waiting", th != NULL);
    ok("and a fourth connection waits with no timeout", td != NULL);
    sleep_ms(300);
    ok("(still waiting)", !aget(&h.done));
    stop_daemon();
    t0 = now_ms();
    if (th) {
        hung = !joined_within(th, &h.done, 5000);
        ok("the call returns", !hung);
        ok("with NULL", !hung && h.reply == NULL);
        test_str("and Disconnected", hung ? NULL : h.name, GIBUS_ERROR_DISCONNECTED);
        if (!hung && h.reply) drop(h.reply);
    }
    if (td) {
        int stuck = !joined_within(td, &fv.done, 5000);

        ok("the wait with no timeout returns", !stuck);
        hung |= stuck;
    }
    {
        int stuck = !joined_within(tb, &L.b_done, 5000);

        ok("B's dispatch thread returns", !stuck);
        hung |= stuck;
    }
    printf("    within %lu ms\n", (unsigned long)(now_ms() - t0));
    if (hung) {
        puts("    threads still wait; not freeing the connections");
        return;
    }
    ok("A is no longer connected", !gibus_connected(L.a));
    ok("nor is B", !gibus_connected(L.b));
    test_eq("A's socket is -1", gibus_fd(L.a), -1);
    t0 = now_ms();
    ok("gibus_read_write answers 0", !gibus_read_write(L.a, 2000));
    ok("at once", now_ms() - t0 < 1000);
    m = gibus_msg_signal("/org/gibus/Src", LIVE_IFACE, "Tick");
    ok("gibus_send answers 0", m && !gibus_send(L.a, m, NULL));
    drop(m);
    m = gibus_msg_call(LIVE_NAME, LIVE_PATH, LIVE_IFACE, "Echo");
    gibus_error_init(&err);
    t0 = now_ms();
    r = m ? gibus_call(L.a, m, 5000, &err) : NULL;
    ok("gibus_call answers NULL at once", !r && now_ms() - t0 < 1000);
    ok("with the error set", gibus_error_is_set(&err));
    gibus_error_free(&err);
    drop(r);
    drop(m);
    if (d) gibus_unref(d);
    gibus_unref(L.a);
    gibus_unref(L.b);
}

#endif

int
main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--require-bus") == 0) g_require_bus = 1;
    puts("gibustest: the message format, and a private bus where there is one");
    test_basic_types();
    test_containers();
    test_alignment();
    test_get_args();
    test_bad_builds();
    test_big();
    test_malformed();
    test_frame_size();
    test_mutation();
#ifdef GIBUSTEST_LIVE
    test_live();
#endif
    return test_done("gibustest");
}
