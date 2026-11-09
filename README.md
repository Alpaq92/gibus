# gibus

A small D-Bus client in C99 for Linux and the BSDs. It has no dependencies
beyond libc and pthreads.

It does:

- EXTERNAL authentication over a Unix socket, by path or, on Linux, by
  abstract name. It finds the session bus through `DBUS_SESSION_BUS_ADDRESS`,
  or `$XDG_RUNTIME_DIR/bus` when that is unset, or opens a `unix:` address
  it is given.
- The wire format in either byte order, with every message validated as it
  arrives.
- Method calls with a timeout, replies, errors and signals, with `AddMatch`
  and `RequestName`.
- Object paths: a handler for one path, a fallback for a path and everything
  below it, and filters that see every message. A method call nobody handles
  is answered with `UnknownMethod`, an `Introspect` nobody handles lists the
  registered children, and `org.freedesktop.DBus.Peer` is answered for you.

It does not do:

- Any authentication other than EXTERNAL.
- TCP or any transport other than `unix:`.
- File descriptor passing.
- X11 autolaunch.

The library is about 3,400 lines: `include/gibus.h` 167, `src/gibus_msg.c`
1,656, `src/gibus_conn.c` 1,585, `src/gibus_internal.h` 12. The test is
another 2,700.

## Building

```sh
cmake -S . -B build
cmake --build build
build/gibustest
```

On Linux and the BSDs, `gibustest` also starts a private `dbus-daemon` for its
live checks and skips them when there is none; `--require-bus` fails instead.
As a subproject:

```cmake
add_subdirectory(gibus)
target_link_libraries(app PRIVATE gibus)
```

`GIBUS_TESTS` builds the test; it is on when gibus is the top-level project.
Elsewhere, Windows and macOS among them, the library holds only the message
half (`gibus_msg.c`), which is portable C99.

## Using it

```c
#include <gibus.h>
#include <stdio.h>

static int
on_ping(gibus_conn *c, gibus_msg *m, void *user)
{
    gibus_msg *r;

    (void)user;
    if (!gibus_msg_is_call(m, "org.example.Demo", "Ping")) return GIBUS_NOT_YET_HANDLED;
    if ((r = gibus_msg_return(m)) != NULL) {
        gibus_send(c, r, NULL);
        gibus_msg_free(r);
    }
    return GIBUS_HANDLED;
}

int
main(void)
{
    gibus_error err;
    gibus_conn *c;
    gibus_msg *m, *r;
    const char *id = NULL;

    gibus_error_init(&err);
    if (!(c = gibus_session(&err))) {
        fprintf(stderr, "%s: %s\n", err.name, err.message);
        return 1;
    }

    m = gibus_msg_call(GIBUS_SERVICE_DBUS, GIBUS_PATH_DBUS, GIBUS_INTERFACE_DBUS, "GetId");
    r = gibus_call(c, m, 5000, &err);
    if (r && gibus_msg_get_args(r, &err, GIBUS_TYPE_STRING, &id, GIBUS_TYPE_INVALID))
        printf("bus %s, and we are %s\n", id, gibus_unique_name(c));
    gibus_msg_free(r);
    gibus_msg_free(m);
    gibus_error_free(&err);

    gibus_register_path(c, "/org/example/Demo", on_ping, NULL);
    while (gibus_read_write_dispatch(c, -1))
        ;
    gibus_unref(c);
    return 0;
}
```

Arguments are read with `gibus_msg_get_args` or an iterator
(`gibus_iter_init`, `gibus_iter_type`, `gibus_iter_get_basic`,
`gibus_iter_recurse`, `gibus_iter_next`) and written with
`gibus_msg_append_args` or `gibus_iter_init_append`, `gibus_iter_open` and
`gibus_iter_close`. Strings read from a message point into it and live as
long as it does.

## Against libdbus

The same work through each: one connection serves an object, another calls
it and walks the whole reply, on a private session bus. Seven alternating
rounds, medians, ranges in brackets.

| | gibus | libdbus |
| --- | --- | --- |
| Call with an empty reply | 269 µs (145–341) | 300 µs (195–313) |
| Call with a 200-item menu reply, walked in full | 1.65 ms (0.98–1.71) | 2.97 ms (2.29–4.51) |
| Connecting to the bus | 533 µs (439–1,229) | 758 µs (528–1,177) |
| Machine code (`.text`) | 35.5 KB | 190.6 KB |

Large replies take about 45% less time; on empty calls and connecting the
ranges overlap. Most of a call is `dbus-daemon` passing it on: in a profiled
run, gibus's own building and reading was about 210 µs of a 1.28 ms call.

Linux Mint 22.3, a KVM guest with 4 cores of a Ryzen 5 4600H, under load
(load average 3); GCC 13.3 at `-O2`, libdbus 1.14.10 as Ubuntu ships it.

## Threads

- Any thread may send, call and flush on a connection.
- One thread at a time reads the socket. A thread that calls while another
  waits for input is woken when its reply arrives, and a call wakes a reader
  that waits without end so its message goes out.
- Handlers and filters run on the thread that dispatches, with no lock held.
  They may send, call, and drop the connection's last reference.
- Messages that arrive while a thread waits for a reply stay queued for
  dispatch. At most 4,096 are kept; past that the oldest is dropped.
- `gibus_session` hands every caller the same connection, each with a
  reference of its own. `gibus_session_private` and `gibus_open` give a
  connection of the caller's own.
- When the bus goes away, every waiting call returns with
  `org.freedesktop.DBus.Error.Disconnected`.

## Origin

gibus is ported from [minidbus](https://codefloe.com/Honken/minidbus), under
its MIT license. It was written as Reaktor's D-Bus client, where it
serves the AT-SPI accessibility bridge, the tray, notifications and the
desktop portal's settings, and was split out from it. See `LICENSE`.
