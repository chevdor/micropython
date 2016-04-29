/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Paul Sokolovsky
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "py/nlr.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/builtin.h"
#include "extmod/modwebsocket.h"

#if MICROPY_PY_WEBREPL

#if 1 // print debugging info
#define DEBUG_printf DEBUG_printf
#else // don't print debugging info
#define DEBUG_printf(...) (void)0
#endif

struct webrepl_file {
    char sig[2];
    char type;
    char flags;
    uint64_t offset;
    uint32_t size;
    uint16_t fname_len;
    char fname[64];
} __attribute__((packed));

enum { PUT_FILE = 1, GET_FILE, LIST_DIR };

typedef struct _mp_obj_webrepl_t {
    mp_obj_base_t base;
    mp_obj_t sock;
    byte state;
    byte hdr_to_recv;
    uint32_t data_to_recv;
    struct webrepl_file hdr;
    mp_obj_t cur_file;
} mp_obj_webrepl_t;


static inline void close_meth(mp_obj_t stream) {
    mp_obj_t dest[2];
    mp_load_method(stream, MP_QSTR_close, dest);
    mp_call_method_n_kw(0, 0, dest);
}

STATIC void write_webrepl(mp_obj_t websock, const void *buf, size_t len) {
    const mp_stream_p_t *sock_stream = mp_get_stream_raise(websock, MP_STREAM_OP_WRITE | MP_STREAM_OP_IOCTL);
    int err;
    int old_opts = sock_stream->ioctl(websock, MP_STREAM_SET_DATA_OPTS, FRAME_BIN, &err);
    sock_stream->write(websock, buf, len, &err);
    sock_stream->ioctl(websock, MP_STREAM_SET_DATA_OPTS, old_opts, &err);
}

STATIC void write_webrepl_resp(mp_obj_t websock, uint16_t code) {
    char buf[4] = {'W', 'B', code & 0xff, code >> 8};
    write_webrepl(websock, buf, sizeof(buf));
}

STATIC mp_obj_t webrepl_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 2, false);
    mp_get_stream_raise(args[0], MP_STREAM_OP_READ | MP_STREAM_OP_WRITE | MP_STREAM_OP_IOCTL);
    DEBUG_printf("sizeof(struct webrepl_file) = %lu\n", sizeof(struct webrepl_file));
    mp_obj_webrepl_t *o = m_new_obj(mp_obj_webrepl_t);
    o->base.type = type;
    o->sock = args[0];
    o->hdr_to_recv = sizeof(struct webrepl_file);
    o->data_to_recv = 0;
    return o;
}

STATIC void handle_op(mp_obj_webrepl_t *self) {
    mp_obj_t open_args[2] = {
        mp_obj_new_str(self->hdr.fname, strlen(self->hdr.fname), false),
        MP_OBJ_NEW_QSTR(MP_QSTR_rb)
    };

    if (self->hdr.type == PUT_FILE) {
        open_args[1] = MP_OBJ_NEW_QSTR(MP_QSTR_wb);
    }

    self->cur_file = mp_builtin_open(2, open_args, (mp_map_t*)&mp_const_empty_map);
    const mp_stream_p_t *file_stream =
        mp_get_stream_raise(self->cur_file, MP_STREAM_OP_READ | MP_STREAM_OP_WRITE | MP_STREAM_OP_IOCTL);

    #if 0
    struct mp_stream_seek_t seek = { .offset = self->hdr.offset, .whence = 0 };
    int err;
    mp_uint_t res = file_stream->ioctl(self->cur_file, MP_STREAM_SEEK, (uintptr_t)&seek, &err);
    assert(res != MP_STREAM_ERROR);
    #endif

    write_webrepl_resp(self->sock, 0);

    if (self->hdr.type == PUT_FILE) {
        self->data_to_recv = self->hdr.size;
    } else if (self->hdr.type == GET_FILE) {
        byte readbuf[2 + 256];
        int err;
        // TODO: It's not ideal that we block connection while sending file
        // and don't process any input.
        while (1) {
            mp_uint_t out_sz = file_stream->read(self->cur_file, readbuf + 2, sizeof(readbuf) - 2, &err);
            assert(out_sz != MP_STREAM_ERROR);
            readbuf[0] = out_sz;
            readbuf[1] = out_sz >> 8;
            DEBUG_printf("webrepl: Sending %d bytes of file\n", out_sz);
            write_webrepl(self->sock, readbuf, 2 + out_sz);
            if (out_sz == 0) {
                break;
            }
        }

        write_webrepl_resp(self->sock, 0);
        self->hdr_to_recv = sizeof(struct webrepl_file);
    }
}

STATIC mp_uint_t _webrepl_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode);

STATIC mp_uint_t webrepl_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    mp_uint_t out_sz;
    do {
        out_sz = _webrepl_read(self_in, buf, size, errcode);
    } while (out_sz == -2);
    return out_sz;
}

STATIC mp_uint_t _webrepl_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    // We know that os.dupterm always calls with size = 1
    assert(size == 1);
    mp_obj_webrepl_t *self = self_in;
    const mp_stream_p_t *sock_stream = mp_get_stream_raise(self->sock, MP_STREAM_OP_READ);
    mp_uint_t out_sz = sock_stream->read(self->sock, buf, size, errcode);
    if (out_sz == 0 || out_sz == MP_STREAM_ERROR) {
        return out_sz;
    }
    // If last read data belonged to text record (== REPL)
    int err;
    if (sock_stream->ioctl(self->sock, MP_STREAM_GET_DATA_OPTS, 0, &err) == 1) {
        return out_sz;
    }

    DEBUG_printf("webrepl: received bin data, hdr_to_recv: %d, data_to_recv=%d\n", self->hdr_to_recv, self->data_to_recv);

    if (self->hdr_to_recv != 0) {
        char *p = (char*)&self->hdr + sizeof(self->hdr) - self->hdr_to_recv;
        *p++ = *(char*)buf;
        if (--self->hdr_to_recv != 0) {
            mp_uint_t hdr_sz = sock_stream->read(self->sock, p, self->hdr_to_recv, errcode);
            if (hdr_sz == MP_STREAM_ERROR) {
                return hdr_sz;
            }
            self->hdr_to_recv -= hdr_sz;
            if (self->hdr_to_recv != 0) {
                return -2;
            }
        }

        DEBUG_printf("webrepl: op: %d, file: %s, chunk @%x, sz=%d\n", self->hdr.type, self->hdr.fname, (uint32_t)self->hdr.offset, self->hdr.size);

        handle_op(self);

        return -2;
    }

    if (self->data_to_recv != 0) {
        static byte filebuf[256];
        filebuf[0] = *(byte*)buf;
        mp_uint_t buf_sz = 1;
        if (--self->data_to_recv != 0) {
            size_t to_read = MIN(sizeof(filebuf) - 1, self->data_to_recv);
            mp_uint_t sz = sock_stream->read(self->sock, filebuf + 1, to_read, errcode);
            if (sz == MP_STREAM_ERROR) {
                return sz;
            }
            self->data_to_recv -= sz;
            buf_sz += sz;
        }

        DEBUG_printf("webrepl: Writing %lu bytes to file\n", buf_sz);
        int err;
        mp_uint_t res = mp_stream_writeall(self->cur_file, filebuf, buf_sz, &err);
        if(res == MP_STREAM_ERROR) {
            assert(0);
        }

        if (self->data_to_recv == 0) {
            close_meth(self->cur_file);
            self->hdr_to_recv = sizeof(struct webrepl_file);
            DEBUG_printf("webrepl: Finished writing file\n");
            write_webrepl_resp(self->sock, 0);
        }
    }

    return -2;
}

STATIC mp_uint_t webrepl_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    mp_obj_webrepl_t *self = self_in;
    const mp_stream_p_t *stream_p = mp_get_stream_raise(self->sock, MP_STREAM_OP_WRITE);
    return stream_p->write(self->sock, buf, size, errcode);
}

STATIC const mp_map_elem_t webrepl_locals_dict_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR_read), (mp_obj_t)&mp_stream_read_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_write), (mp_obj_t)&mp_stream_write_obj },
};
STATIC MP_DEFINE_CONST_DICT(webrepl_locals_dict, webrepl_locals_dict_table);

STATIC const mp_stream_p_t webrepl_stream_p = {
    .read = webrepl_read,
    .write = webrepl_write,
};

STATIC const mp_obj_type_t webrepl_type = {
    { &mp_type_type },
    .name = MP_QSTR__webrepl,
    .make_new = webrepl_make_new,
    .stream_p = &webrepl_stream_p,
    .locals_dict = (mp_obj_t)&webrepl_locals_dict,
};

STATIC const mp_map_elem_t webrepl_module_globals_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__), MP_OBJ_NEW_QSTR(MP_QSTR_websocket) },
    { MP_OBJ_NEW_QSTR(MP_QSTR__webrepl), (mp_obj_t)&webrepl_type },
};

STATIC MP_DEFINE_CONST_DICT(webrepl_module_globals, webrepl_module_globals_table);

const mp_obj_module_t mp_module_webrepl = {
    .base = { &mp_type_module },
    .name = MP_QSTR__webrepl,
    .globals = (mp_obj_dict_t*)&webrepl_module_globals,
};

#endif // MICROPY_PY_WEBREPL
