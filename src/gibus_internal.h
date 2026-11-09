#ifndef GIBUS_INTERNAL_H
#define GIBUS_INTERNAL_H

#include "gibus.h"

long gibus__frame_size(const uint8_t *b, size_t have);

void gibus__error_set(gibus_error *e, const char *name, const char *fmt, ...);

int  gibus__msg_no_reply(const gibus_msg *m);

#endif
