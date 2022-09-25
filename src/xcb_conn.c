/* Copyright (C) 2001-2004 Bart Massey and Jamey Sharp.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 * Except as contained in this notice, the names of the authors or their
 * institutions shall not be used in advertising or otherwise to promote the
 * sale, use or other dealings in this Software without prior written
 * authorization from the authors.
 */

/* Connection management: the core of XCB. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#if __STDC_VERSION__ >= 201112L
#include <stdalign.h>
#endif
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>

#include "xcb.h"
#include "xcbint.h"
#if USE_POLL
#include <poll.h>
#elif !defined _WIN32
#include <sys/select.h>
#endif

#ifdef _WIN32
#include "xcb_windefs.h"
#include <io.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#endif /* _WIN32 */

/* SHUT_RDWR is fairly recent and is not available on all platforms */
#if !defined(SHUT_RDWR)
#define SHUT_RDWR 2
#endif

typedef struct {
    uint8_t  status;
    uint8_t  pad0[5];
    uint16_t length;
} xcb_setup_generic_t;

static const xcb_setup_t xcb_error_setup = {
    0,     /* status: failed (but we wouldn't have a xcb_setup_t in this case) */
    0,     /* pad0 */
    0, 0,  /* protocol version, should be 11.0, but isn't */
    0,     /* length, invalid value */
    0,     /* release_number */
    0, 0,  /* resource_id_{base,mask} */
    0,     /* motion_buffer_size */
    0,     /* vendor_len */
    0,     /* maximum_request_length */
    0,     /* roots_len */
    0,     /* pixmap_formats_len */
    0,     /* image_byte_order */
    0,     /* bitmap_format_bit_order */
    0,     /* bitmap_format_scanline_unit */
    0,     /* bitmap_format_scanline_pad */
    0, 0,  /* {min,max}_keycode */
    { 0, 0, 0, 0 } /* pad1 */
};

/* Keep this list in sync with is_static_error_conn()! */
static const int xcb_con_error = XCB_CONN_ERROR;
static const int xcb_con_closed_mem_er = XCB_CONN_CLOSED_MEM_INSUFFICIENT;
static const int xcb_con_closed_parse_er = XCB_CONN_CLOSED_PARSE_ERR;
static const int xcb_con_closed_screen_er = XCB_CONN_CLOSED_INVALID_SCREEN;

static int is_static_error_conn(xcb_connection_t *c)
{
    return c == (xcb_connection_t *) &xcb_con_error ||
           c == (xcb_connection_t *) &xcb_con_closed_mem_er ||
           c == (xcb_connection_t *) &xcb_con_closed_parse_er ||
           c == (xcb_connection_t *) &xcb_con_closed_screen_er;
}

static int set_fd_flags(const int fd)
{
/* Win32 doesn't have file descriptors and the fcntl function. This block sets the socket in non-blocking mode */

#ifdef _WIN32
   u_long iMode = 1; /* non-zero puts it in non-blocking mode, 0 in blocking mode */
   int ret = 0;

   ret = ioctlsocket(fd, FIONBIO, &iMode);
   if(ret != 0)
       return 0;
   return 1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1)
        return 0;
    flags |= O_NONBLOCK;
    if(fcntl(fd, F_SETFL, flags) == -1)
        return 0;
    if(fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
        return 0;
    return 1;
#endif /* _WIN32 */
}

static int write_setup(xcb_connection_t *c, xcb_auth_info_t *auth_info)
{
    static const char pad[3];
    xcb_setup_request_t out;
    struct iovec parts[6];
    int count = 0;
    static const uint32_t endian = 0x01020304;
    int ret;

    memset(&out, 0, sizeof(out));

    /* B = 0x42 = MSB first, l = 0x6c = LSB first */
    if(htonl(endian) == endian)
        out.byte_order = 0x42;
    else
        out.byte_order = 0x6c;
    out.protocol_major_version = X_PROTOCOL;
    out.protocol_minor_version = X_PROTOCOL_REVISION;
    out.authorization_protocol_name_len = 0;
    out.authorization_protocol_data_len = 0;
    parts[count].iov_len = sizeof(xcb_setup_request_t);
    parts[count++].iov_base = &out;
    parts[count].iov_len = XCB_PAD(sizeof(xcb_setup_request_t));
    parts[count++].iov_base = (char *) pad;

    if(auth_info)
    {
        parts[count].iov_len = out.authorization_protocol_name_len = auth_info->namelen;
        parts[count++].iov_base = auth_info->name;
        parts[count].iov_len = XCB_PAD(out.authorization_protocol_name_len);
        parts[count++].iov_base = (char *) pad;
        parts[count].iov_len = out.authorization_protocol_data_len = auth_info->datalen;
        parts[count++].iov_base = auth_info->data;
        parts[count].iov_len = XCB_PAD(out.authorization_protocol_data_len);
        parts[count++].iov_base = (char *) pad;
    }
    assert(count <= (int) (sizeof(parts) / sizeof(*parts)));

    pthread_mutex_lock(&c->iolock);
    ret = _xcb_out_send(c, parts, count);
    pthread_mutex_unlock(&c->iolock);
    return ret;
}

static int read_setup(xcb_connection_t *c)
{
    uint32_t extra_bytes, total_bytes;
    const char newline = '\n';

    /* Read the server response */
    c->setup = malloc(sizeof(xcb_setup_generic_t));
    if(!c->setup)
        return 0;

    if(_xcb_in_read_block(c, c->setup, sizeof(xcb_setup_generic_t)) != sizeof(xcb_setup_generic_t))
        return 0;

    extra_bytes = c->setup->length * UINT32_C(4);
    total_bytes = extra_bytes + sizeof(xcb_setup_generic_t);
    {
        void *tmp = realloc(c->setup, total_bytes);
        if(!tmp)
            return 0;
        c->setup = tmp;
    }

    if(_xcb_in_read_block(c, (char *) c->setup + sizeof(xcb_setup_generic_t), extra_bytes) <= 0)
        return 0;

    /* 0 = failed, 2 = authenticate, 1 = success */
    switch(c->setup->status)
    {
    case 0: /* failed */
        {
            xcb_setup_failed_t *setup = (xcb_setup_failed_t *) c->setup;
            char *msg = (char *)c->setup + sizeof(*setup);
            if (setup->reason_len > extra_bytes)
                setup->reason_len = (uint8_t)extra_bytes;
            for (size_t i = 0; i < setup->reason_len; ++i) {
                if (msg[i] < 0x20 || msg[i] > 0x7E)
                    msg[i] = 0x20;
            }
            write(STDERR_FILENO, msg, setup->reason_len);
            write(STDERR_FILENO, &newline, 1);
            return 0;
        }

    case 2: /* authenticate */
        {
            char *reason = (char *)c->setup + sizeof(xcb_setup_authenticate_t);
            for (size_t i = 0; i < extra_bytes; ++i) {
                if (reason[i] < 0x20 || reason[i] > 0x7E)
                    reason[i] = 0x20;
            }
            write(STDERR_FILENO, reason, extra_bytes);
            write(STDERR_FILENO, &newline, 1);
            return 0;
        }
    case 1: /* success */
        if (total_bytes < sizeof(xcb_setup_t) + sizeof(xcb_screen_t) + sizeof(xcb_format_t))
            return 0; /* not enough bytes sent, no screens, or no formats */
        else
        {
            size_t i, j;
            const uint8_t *cursor = (const uint8_t *)c->setup,
                          *const end = cursor + total_bytes;
            xcb_setup_t *const setup = (xcb_setup_t *) c->setup;
            const xcb_format_t *formats;

            /*
             * Padded length of the vendor string.  setup->vendor_len is a
             * uint16_t so overflow is impossible.  Cannot exceed 65536.
             */
            uint32_t const pixmap_offset = (setup->vendor_len + UINT32_C(3)) & ~UINT32_C(3);

            /*
             * Length of the pixmap formats.  setup->pixmap_formats_len is
             * uint8_t so overflow is impossible.
             */
            uint32_t const pixmap_formats_len = sizeof(xcb_format_t) * setup->pixmap_formats_len;

            /* Offset of the screens.  Max RHS = 40 + 65536 + 8 * 255 = 67616 so no risk of overflow */
            uint32_t const screen_offset = sizeof(xcb_setup_t) + pixmap_offset + pixmap_formats_len;

            /* 4 zero bytes, for memcmp() */
            uint32_t const zero = 0;

#if __STDC_VERSION__ >= 201112L
            /* A bunch of static assertions */
            static_assert(sizeof(xcb_screen_t) == 40, "bug");
            static_assert(sizeof(xcb_setup_t) == 40, "bug");
            static_assert(sizeof(xcb_setup_generic_t) == 8, "bug");
            static_assert(sizeof(xcb_setup_authenticate_t) == 8, "bug");
            static_assert(sizeof(xcb_setup_failed_t) == 8, "bug");
            static_assert(sizeof(xcb_visualtype_t) == 24, "bug");
            static_assert(sizeof(xcb_depth_t) == 8, "bug");
            static_assert(sizeof(xcb_format_t) == 8, "bug");
            static_assert(alignof(xcb_screen_t) == 4, "bug");
            static_assert(alignof(xcb_visualtype_t) == 4, "bug");
            static_assert(alignof(xcb_depth_t) == 2, "bug");
#endif
            /* Must have at least 1 screen and 1 pixmap format */
            if (setup->roots_len < 1 || setup->pixmap_formats_len < 1)
                return 0;

            /* First screen must fit in the data sent. */
            if (total_bytes < screen_offset + sizeof(xcb_screen_t))
                return 0;

            /*
             * xcb_setup_pixmap_formats() would be safe, but just using pointer
             * arithmetic is simpler.
             */
            formats = (const xcb_format_t *)(cursor + pixmap_offset);

            /* Validate the pixmap formats.  Bounds have been checked already. */
            for (i = 0; i < setup->pixmap_formats_len; ++i) {
                /* Depth must not be zero. */
                if (formats[i].depth == 0)
                   return 0;

                /* Bits per pixel must not be zero and must be a multiple of 8. */
                if ((formats[i].bits_per_pixel < 8) ||
                    (formats[i].bits_per_pixel % 8) != 0)
                    return 0;
            }

            /*
             * Alignment is guaranteed because screen_offset is a multiple of 4
             * and c->setup was allocated by malloc().  cursor is kept 4-byte
             * aligned at all times.
             */
            cursor = (const uint8_t *)setup + screen_offset;

            /* Validate each screen. */
            for (i = 0; i < setup->roots_len; ++i) {
                const struct xcb_screen_t *screen;
                const struct xcb_depth_t *depth;
                const struct xcb_visualtype_t *visuals;

                /*
                 * Screen must fit in the buffer with room for a depth and a
                 * visual type.
                 */
                if ((end - cursor) <
                    (sizeof(*screen) + sizeof(*depth) + sizeof(*visuals)))
                    return 0;

                screen = (const struct xcb_screen_t *)cursor;
                cursor += sizeof(*screen);

                /* Screens must have at least one depth */
                if (screen->allowed_depths_len < 1)
                    return 0;

                for (j = 0; j < screen->allowed_depths_len; ++j) {
                    uint32_t visuals_size, k;

                    /* Depth must fit in the buffer with room for a visual type. */
                    if ((end - cursor) < (sizeof(*depth) + sizeof(*visuals)))
                        return 0;

                    /*
                     * Alignment is guaranteed because xcb_screen_t has greater
                     * alignment (4) than xcb_depth_t (2).  sizeof(xcb_depth_t)
                     * is a multiple of 4 so alignment will be maintained.
                     */
                    depth = (const struct xcb_depth_t *)cursor;
                    cursor += sizeof(*depth);

                    /* Padding must be zero */
                    if ((depth->pad0 != 0) ||
                        (memcmp(depth->pad1, &zero, sizeof(zero)) != 0))
                        return 0;

                    /* Depths must have at least one visual type */
                    if (depth->visuals_len < 1)
                        return 0;

                    /* depth->visuals_len is uint16_t so overflow is impossible. */
                    visuals_size = (uint32_t)depth->visuals_len * sizeof(*visuals);

                    /* Visuals must fit in the buffer. */
                    if ((size_t)(end - cursor) < visuals_size)
                        return 0;

                    /* Cursor is 4-byte aligned, which is sufficient. */
                    visuals = (const xcb_visualtype_t *)cursor;

                    for (k = 0; k < depth->visuals_len; ++k) {
                        /* Padding must be zero. */
                        if (memcmp(visuals[k].pad0, &zero, 4))
                            return 0;

                        /* Bits per RGB value must not be zero. */
                        if (visuals[k].bits_per_rgb_value < 1)
                            return 0;
                    }

                    cursor += visuals_size;
                }
            }
            if (end != cursor)
                return 0; /* trailing junk */
        }
        return 1;
    default:
        return 0;
    }
}

/* precondition: there must be something for us to write. */
static int write_vec(xcb_connection_t *c, struct iovec **vector, int *count)
{
#ifndef _WIN32
    int n;
#endif

    assert(!c->out.queue_len);

#ifdef _WIN32
    /* Could use the WSASend win32 function for scatter/gather i/o but setting up the WSABUF struct from
       an iovec would require more work and I'm not sure of the benefit....works for now */
    while (*count)
    {
        struct iovec *vec = *vector;
        if (vec->iov_len)
        {
            int ret = send(c->fd, vec->iov_base, vec->iov_len, 0);
            if (ret == SOCKET_ERROR)
            {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK)
                {
                    return 1;
                }
            }
            if (ret <= 0)
            {
                _xcb_conn_shutdown(c, XCB_CONN_ERROR);
                return 0;
            }
            c->out.total_written += ret;
            vec->iov_len -= ret;
            vec->iov_base = (char *)vec->iov_base + ret;
        }
        if (vec->iov_len == 0) {
            (*vector)++;
            (*count)--;
        }
    }

    if (!*count)
        *vector = 0;

#else
    n = *count;
    if (n > IOV_MAX)
        n = IOV_MAX;

#if HAVE_SENDMSG
    if (c->out.out_fd.nfd) {
        union {
            struct cmsghdr cmsghdr;
            char buf[CMSG_SPACE(XCB_MAX_PASS_FD * sizeof(int))];
        } cmsgbuf;
        struct msghdr msg = {
            .msg_name = NULL,
            .msg_namelen = 0,
            .msg_iov = *vector,
            .msg_iovlen = n,
            .msg_control = cmsgbuf.buf,
            .msg_controllen = CMSG_LEN(c->out.out_fd.nfd * sizeof (int)),
        };
        int i;
        struct cmsghdr *hdr = CMSG_FIRSTHDR(&msg);

        hdr->cmsg_len = msg.msg_controllen;
        hdr->cmsg_level = SOL_SOCKET;
        hdr->cmsg_type = SCM_RIGHTS;
        memcpy(CMSG_DATA(hdr), c->out.out_fd.fd, c->out.out_fd.nfd * sizeof (int));

        n = sendmsg(c->fd, &msg, 0);
        if(n < 0 && errno == EAGAIN)
            return 1;
        for (i = 0; i < c->out.out_fd.nfd; i++)
            close(c->out.out_fd.fd[i]);
        c->out.out_fd.nfd = 0;
    } else
#endif
    {
        n = writev(c->fd, *vector, n);
        if(n < 0 && errno == EAGAIN)
            return 1;
    }

    if(n <= 0)
    {
        _xcb_conn_shutdown(c, XCB_CONN_ERROR);
        return 0;
    }

    c->out.total_written += n;
    for(; *count; --*count, ++*vector)
    {
        int cur = (*vector)->iov_len;
        if(cur > n)
            cur = n;
        if(cur) {
            (*vector)->iov_len -= cur;
            (*vector)->iov_base = (char *) (*vector)->iov_base + cur;
            n -= cur;
        }
        if((*vector)->iov_len)
            break;
    }
    if(!*count)
        *vector = 0;
    assert(n == 0);

#endif /* _WIN32 */

    return 1;
}

/* Public interface */

const xcb_setup_t *xcb_get_setup(xcb_connection_t *c)
{
    if(is_static_error_conn(c))
        return &xcb_error_setup;
    /* doesn't need locking because it's never written to. */
    return c->setup;
}

int xcb_get_file_descriptor(xcb_connection_t *c)
{
    if(is_static_error_conn(c))
        return -1;
    /* doesn't need locking because it's never written to. */
    return c->fd;
}

int xcb_connection_has_error(xcb_connection_t *c)
{
    /* doesn't need locking because it's read and written atomically. */
    return c->has_error;
}

xcb_connection_t *xcb_connect_to_fd(int fd, xcb_auth_info_t *auth_info)
{
    xcb_connection_t* c;

#ifndef _WIN32
#ifndef USE_POLL
    if(fd >= FD_SETSIZE) /* would overflow in FD_SET */
    {
        close(fd);
        return _xcb_conn_ret_error(XCB_CONN_ERROR);
    }
#endif
#endif /* !_WIN32*/

    c = calloc(1, sizeof(xcb_connection_t));
    if(!c) {
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
        return _xcb_conn_ret_error(XCB_CONN_CLOSED_MEM_INSUFFICIENT) ;
    }

    c->fd = fd;

    if(!(
        set_fd_flags(fd) &&
        pthread_mutex_init(&c->iolock, 0) == 0 &&
        _xcb_in_init(&c->in) &&
        _xcb_out_init(&c->out) &&
        write_setup(c, auth_info) &&
        read_setup(c) &&
        _xcb_ext_init(c) &&
        _xcb_xid_init(c)
        ))
    {
        xcb_disconnect(c);
        return _xcb_conn_ret_error(XCB_CONN_ERROR);
    }

    return c;
}

void xcb_disconnect(xcb_connection_t *c)
{
    if(c == NULL || is_static_error_conn(c))
        return;

    free(c->setup);

    /* disallow further sends and receives */
    shutdown(c->fd, SHUT_RDWR);
#ifdef _WIN32
    closesocket(c->fd);
#else
    close(c->fd);
#endif

    pthread_mutex_destroy(&c->iolock);
    _xcb_in_destroy(&c->in);
    _xcb_out_destroy(&c->out);

    _xcb_ext_destroy(c);
    _xcb_xid_destroy(c);

    free(c);

#ifdef _WIN32
    WSACleanup();
#endif
}

/* Private interface */

void _xcb_conn_shutdown(xcb_connection_t *c, int err)
{
    c->has_error = err;
}

/* Return connection error state.
 * To make thread-safe, I need a seperate static
 * variable for every possible error.
 * has_error is the first field in xcb_connection_t, so just
 * return a casted int here; checking has_error (and only
 * has_error) will be safe.
 */
xcb_connection_t *_xcb_conn_ret_error(int err)
{

    switch(err)
    {
        case XCB_CONN_CLOSED_MEM_INSUFFICIENT:
        {
            return (xcb_connection_t *) &xcb_con_closed_mem_er;
        }
        case XCB_CONN_CLOSED_PARSE_ERR:
        {
            return (xcb_connection_t *) &xcb_con_closed_parse_er;
        }
        case XCB_CONN_CLOSED_INVALID_SCREEN:
        {
            return (xcb_connection_t *) &xcb_con_closed_screen_er;
        }
        case XCB_CONN_ERROR:
        default:
        {
            return (xcb_connection_t *) &xcb_con_error;
        }
    }
}

int _xcb_conn_wait(xcb_connection_t *c, pthread_cond_t *cond, struct iovec **vector, int *count)
{
    int ret;
#if USE_POLL
    struct pollfd fd;
#else
    fd_set rfds, wfds;
#endif

    /* If the thing I should be doing is already being done, wait for it. */
    if(count ? c->out.writing : c->in.reading)
    {
        pthread_cond_wait(cond, &c->iolock);
        return 1;
    }

#if USE_POLL
    memset(&fd, 0, sizeof(fd));
    fd.fd = c->fd;
    fd.events = POLLIN;
#else
    FD_ZERO(&rfds);
    FD_SET(c->fd, &rfds);
#endif
    ++c->in.reading;

#if USE_POLL
    if(count)
    {
        fd.events |= POLLOUT;
        ++c->out.writing;
    }
#else
    FD_ZERO(&wfds);
    if(count)
    {
        FD_SET(c->fd, &wfds);
        ++c->out.writing;
    }
#endif

    pthread_mutex_unlock(&c->iolock);
    do {
#if USE_POLL
        ret = poll(&fd, 1, -1);
        /* If poll() returns an event we didn't expect, such as POLLNVAL, treat
         * it as if it failed. */
        if(ret >= 0 && (fd.revents & ~fd.events))
        {
            ret = -1;
            break;
        }
#else
        ret = select(c->fd + 1, &rfds, &wfds, 0, 0);
#endif
    } while (ret == -1 && errno == EINTR);
    if(ret < 0)
    {
        _xcb_conn_shutdown(c, XCB_CONN_ERROR);
        ret = 0;
    }
    pthread_mutex_lock(&c->iolock);

    if(ret)
    {
        /* The code allows two threads to call select()/poll() at the same time.
         * First thread just wants to read, a second thread wants to write, too.
         * We have to make sure that we don't steal the reading thread's reply
         * and let it get stuck in select()/poll().
         * So a thread may read if either:
         * - There is no other thread that wants to read (the above situation
         *   did not occur).
         * - It is the reading thread (above situation occurred).
         */
        int may_read = c->in.reading == 1 || !count;
#if USE_POLL
        if(may_read && (fd.revents & POLLIN) != 0)
#else
        if(may_read && FD_ISSET(c->fd, &rfds))
#endif
            ret = ret && _xcb_in_read(c);

#if USE_POLL
        if((fd.revents & POLLOUT) != 0)
#else
        if(FD_ISSET(c->fd, &wfds))
#endif
            ret = ret && write_vec(c, vector, count);
    }

    if(count)
        --c->out.writing;
    --c->in.reading;

    return ret;
}

uint64_t xcb_total_read(xcb_connection_t *c)
{
    uint64_t n;

    if (xcb_connection_has_error(c))
        return 0;

    pthread_mutex_lock(&c->iolock);
    n = c->in.total_read;
    pthread_mutex_unlock(&c->iolock);
    return n;
}

uint64_t xcb_total_written(xcb_connection_t *c)
{
    uint64_t n;

    if (xcb_connection_has_error(c))
        return 0;

    pthread_mutex_lock(&c->iolock);
    n = c->out.total_written;
    pthread_mutex_unlock(&c->iolock);

    return n;
}
