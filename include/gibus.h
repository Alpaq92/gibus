#ifndef GIBUS_H
#define GIBUS_H

/* Ported from minidbus (MIT; see LICENSE). */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GIBUS_TYPE_INVALID     0
#define GIBUS_TYPE_BYTE        'y'
#define GIBUS_TYPE_BOOLEAN     'b'
#define GIBUS_TYPE_INT16       'n'
#define GIBUS_TYPE_UINT16      'q'
#define GIBUS_TYPE_INT32       'i'
#define GIBUS_TYPE_UINT32      'u'
#define GIBUS_TYPE_INT64       'x'
#define GIBUS_TYPE_UINT64      't'
#define GIBUS_TYPE_DOUBLE      'd'
#define GIBUS_TYPE_STRING      's'
#define GIBUS_TYPE_OBJECT_PATH 'o'
#define GIBUS_TYPE_SIGNATURE   'g'
#define GIBUS_TYPE_ARRAY       'a'
#define GIBUS_TYPE_VARIANT     'v'
#define GIBUS_TYPE_STRUCT      'r'
#define GIBUS_TYPE_DICT_ENTRY  'e'

#define GIBUS_MESSAGE_METHOD_CALL   1
#define GIBUS_MESSAGE_METHOD_RETURN 2
#define GIBUS_MESSAGE_ERROR         3
#define GIBUS_MESSAGE_SIGNAL        4

#define GIBUS_SERVICE_DBUS             "org.freedesktop.DBus"
#define GIBUS_PATH_DBUS                "/org/freedesktop/DBus"
#define GIBUS_INTERFACE_DBUS           "org.freedesktop.DBus"
#define GIBUS_INTERFACE_PROPERTIES     "org.freedesktop.DBus.Properties"
#define GIBUS_INTERFACE_INTROSPECTABLE "org.freedesktop.DBus.Introspectable"
#define GIBUS_INTROSPECT_DOCTYPE \
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n" \
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"

#define GIBUS_ERROR_FAILED             "org.freedesktop.DBus.Error.Failed"
#define GIBUS_ERROR_NO_REPLY           "org.freedesktop.DBus.Error.NoReply"
#define GIBUS_ERROR_DISCONNECTED       "org.freedesktop.DBus.Error.Disconnected"
#define GIBUS_ERROR_INVALID_ARGS       "org.freedesktop.DBus.Error.InvalidArgs"
#define GIBUS_ERROR_UNKNOWN_METHOD     "org.freedesktop.DBus.Error.UnknownMethod"
#define GIBUS_ERROR_UNKNOWN_OBJECT     "org.freedesktop.DBus.Error.UnknownObject"
#define GIBUS_ERROR_UNKNOWN_PROPERTY   "org.freedesktop.DBus.Error.UnknownProperty"
#define GIBUS_ERROR_PROPERTY_READ_ONLY "org.freedesktop.DBus.Error.PropertyReadOnly"
#define GIBUS_ERROR_NOT_SUPPORTED      "org.freedesktop.DBus.Error.NotSupported"

#define GIBUS_NAME_FLAG_DO_NOT_QUEUE          4
#define GIBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER 1

#define GIBUS_HANDLED          1
#define GIBUS_NOT_YET_HANDLED  0

typedef uint32_t gibus_bool;

typedef struct gibus_conn gibus_conn;
typedef struct gibus_msg  gibus_msg;

typedef struct gibus_error {
    int  set;
    char name[128];
    char message[256];
} gibus_error;

typedef struct gibus_iter {
    gibus_msg *msg;
    int        mode;
    int        container;
    int        failed;
    int        depth;
    size_t     pos;
    size_t     end;
    size_t     len_at;
    size_t     first;
    char       sig[256];
    size_t     sig_at;
    size_t     sig_len;
} gibus_iter;

/* Runs on the thread that dispatches, with no lock held, and may send. */
typedef int (*gibus_handler)(gibus_conn *c, gibus_msg *m, void *user);

void gibus_error_init(gibus_error *e);
int  gibus_error_is_set(const gibus_error *e);
void gibus_error_free(gibus_error *e);

gibus_conn *gibus_session(gibus_error *err);
gibus_conn *gibus_session_private(gibus_error *err);
gibus_conn *gibus_open(const char *address, gibus_error *err);
void        gibus_unref(gibus_conn *c);

const char *gibus_unique_name(gibus_conn *c);
int         gibus_fd(gibus_conn *c);
int         gibus_connected(gibus_conn *c);

int         gibus_send(gibus_conn *c, gibus_msg *m, uint32_t *serial);
int         gibus_flush(gibus_conn *c);
gibus_msg  *gibus_call(gibus_conn *c, gibus_msg *m, int timeout_ms, gibus_error *err);

int         gibus_read_write(gibus_conn *c, int timeout_ms);
int         gibus_dispatch(gibus_conn *c);
int         gibus_read_write_dispatch(gibus_conn *c, int timeout_ms);

int gibus_add_filter(gibus_conn *c, gibus_handler fn, void *user);
int gibus_register_path(gibus_conn *c, const char *path, gibus_handler fn, void *user);
int gibus_register_fallback(gibus_conn *c, const char *path, gibus_handler fn, void *user);

int gibus_request_name(gibus_conn *c, const char *name, unsigned flags, gibus_error *err);
int gibus_add_match(gibus_conn *c, const char *rule, gibus_error *err);

gibus_msg *gibus_msg_call(const char *dest, const char *path, const char *iface,
                          const char *member);
gibus_msg *gibus_msg_return(const gibus_msg *call);
gibus_msg *gibus_msg_error(const gibus_msg *call, const char *name, const char *text);
gibus_msg *gibus_msg_signal(const char *path, const char *iface, const char *member);
void       gibus_msg_free(gibus_msg *m);

int         gibus_msg_type(const gibus_msg *m);
uint32_t    gibus_msg_serial(const gibus_msg *m);
uint32_t    gibus_msg_reply_serial(const gibus_msg *m);
const char *gibus_msg_path(const gibus_msg *m);
const char *gibus_msg_interface(const gibus_msg *m);
const char *gibus_msg_member(const gibus_msg *m);
const char *gibus_msg_error_name(const gibus_msg *m);
const char *gibus_msg_sender(const gibus_msg *m);
const char *gibus_msg_destination(const gibus_msg *m);
const char *gibus_msg_signature(const gibus_msg *m);

int gibus_msg_is_call(const gibus_msg *m, const char *iface, const char *member);
int gibus_msg_is_signal(const gibus_msg *m, const char *iface, const char *member);
int gibus_msg_has_member(const gibus_msg *m, const char *member);
int gibus_msg_has_signature(const gibus_msg *m, const char *sig);

/* Pairs of a type and a pointer to where it goes, ended by
   GIBUS_TYPE_INVALID. Strings, object paths and signatures point into the
   message. GIBUS_TYPE_ARRAY takes a fixed element type, a pointer to a const
   pointer to the elements and an int count. */
int gibus_msg_get_args(gibus_msg *m, gibus_error *err, int type, ...);
int gibus_msg_append_args(gibus_msg *m, int type, ...);

void gibus_iter_init_append(gibus_msg *m, gibus_iter *it);
int  gibus_iter_append_basic(gibus_iter *it, int type, const void *value);
int  gibus_iter_open(gibus_iter *it, int type, const char *sig, gibus_iter *sub);
int  gibus_iter_close(gibus_iter *it, gibus_iter *sub);
int  gibus_iter_append_fixed_array(gibus_iter *it, int type, const void *elems, int n);

int  gibus_iter_init(gibus_msg *m, gibus_iter *it);
int  gibus_iter_type(const gibus_iter *it);
void gibus_iter_get_basic(const gibus_iter *it, void *value);
void gibus_iter_recurse(const gibus_iter *it, gibus_iter *sub);
int  gibus_iter_next(gibus_iter *it);

gibus_msg *gibus_msg_parse(const void *bytes, size_t len);
const uint8_t *gibus_msg_bytes(gibus_msg *m, uint32_t serial, size_t *len);

#ifdef __cplusplus
}
#endif

#endif
