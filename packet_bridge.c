/* Bridge two packet-based interfaces.
   E.g. works like socat, but for UDP, TAP, SLIP stream, {packet,4} stream, ...
*/

/*
   Note that this is a testing tool that should probably not be
   operated on a non-trusted network.  Don't shoot yourself in the
   foot, because security measures are minimal.

   For ease of configuration, UDP packets are returned to the first
   peer that sends a message to a listening socket and all other peers
   are ignored afterwards.  E.g. if you bridge TAP and UDP and know
   the UDP address, you can essentially gain unrestricted raw Ethernet
   access to whatever the tap interface is bridged to.
*/

#define _POSIX_C_SOURCE 1

#include "packet_bridge.h"

#include "macros.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <linux/if.h>
#include <linux/if_tun.h>

#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <poll.h>

#include <netdb.h>


//#include "/usr/include/asm-generic/termbits.h"
//#include "/usr/include/asm-generic/ioctls.h"

#include <asm-generic/termbits.h>
#include <asm-generic/ioctls.h>


/***** 1. PACKET INTERFACES */


// https://stackoverflow.com/questions/1003684/how-to-interface-with-the-linux-tun-driver
// https://www.kernel.org/doc/Documentation/networking/tuntap.txt
// https://wiki.wireshark.org/Development/LibpcapFileFormat

static inline void log_hex(uint8_t *buf, ssize_t n) {
    for(ssize_t i=0; i<n; i++) {
        LOG(" %02x", buf[i]);
        if (i%16 == 15) LOG("\n");
    }
    LOG("\n");
}

static void log_addr(struct sockaddr_in *sa) {
    uint8_t *a = (void*)&sa->sin_addr;
    LOG("%d.", a[0]);
    LOG("%d.", a[1]);
    LOG("%d.", a[2]);
    LOG("%d:", a[3]);
    LOG("%d\n", ntohs(sa->sin_port));
}

static void assert_write(int fd, uint8_t *buf, uint32_t len) {
    uint32_t written = 0;
    while(written < len) {
        int rv;
        ASSERT((rv = write(fd, buf, len)) > 0);
        written += rv;
    }
}



/***** 1.1. TAP */

static ssize_t tap_read(struct port *p, uint8_t *buf, ssize_t len) {
    ssize_t rlen;
    ASSERT_ERRNO(rlen = read(p->fd, buf, len));
    return rlen;
}
static ssize_t tap_write(struct port *p, const uint8_t *buf, ssize_t len) {
    // EIO is normal until iface is set up
    return write(p->fd, buf, len);
}

struct port *port_open_tap(const char *dev) {
    int fd;
    ASSERT_ERRNO(fd = open("/dev/net/tun", O_RDWR));
    struct ifreq ifr = { .ifr_flags = IFF_TAP | IFF_NO_PI };
    strncpy(ifr.ifr_name, dev, IFNAMSIZ);
    ASSERT_ERRNO(ioctl(fd, TUNSETIFF, (void *) &ifr));
    LOG("tap: %s\n", dev);
    struct port *port;
    ASSERT(port = malloc(sizeof(*port)));
    port->fd = fd;
    port->fd_out = fd;
    port->read = tap_read;
    port->write = tap_write;
    port->pop = 0;
    return port;
}


/***** 1.2. UDP */

struct udp_port {
    struct port p;
    struct sockaddr_in peer;
};

static ssize_t udp_read(struct udp_port *p, uint8_t *buf, ssize_t len) {
    //LOG("udp_read\n");
    ssize_t rlen = 0;
    int flags = 0;
    struct sockaddr_in peer = {};
    socklen_t addrlen = sizeof(&peer);
    ASSERT_ERRNO(
        rlen = recvfrom(p->p.fd, buf, len, flags,
                        (struct sockaddr*)&peer, &addrlen));
    ASSERT(addrlen == sizeof(peer));

    /* Associate to first peer that sends to us.  This is to make
       setup simpler. */
    if (!p->peer.sin_port) {
        memcpy(&p->peer, &peer, sizeof(peer));
        log_addr(&peer);
        goto done;
    }
    /* After that, drop packets that do not come from peer. */
    if(memcmp(&p->peer, &peer, sizeof(peer))) {
        LOG("WARNING: ununknown sender %d:\n", sizeof(peer));
        log_addr(&peer);
        //log_addr(&p->peer);
        rlen = 0;
        goto done;
    }
  done:
    //LOG("udp_read %d\n", rlen);
    return rlen;

}
static ssize_t udp_write(struct udp_port *p, uint8_t *buf, ssize_t len) {
    if (p->peer.sin_port == 0) {
        /* Drop while not assicated */
        return 0;
    }
    ssize_t wlen;
    int flags = 0;
    ASSERT_ERRNO(
        wlen = sendto(p->p.fd, buf, len, flags,
                      (struct sockaddr*)&p->peer,
                      sizeof(p->peer)));
    return wlen;
}

struct port *port_open_udp(uint16_t port) {
    int fd;
    ASSERT_ERRNO(fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if(port) {
        struct sockaddr_in address = {
            .sin_port = htons(port),
            .sin_family = AF_INET
        };
        ASSERT_ERRNO(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int)));
        socklen_t addrlen = sizeof(address);
        ASSERT_ERRNO(bind(fd, (struct sockaddr *)&address, addrlen));
        LOG("udp: port %d\n", port);
    }
    else {
        LOG("udp: not bound\n");
    }
    struct udp_port *p;
    ASSERT(p = malloc(sizeof(*p)));
    memset(p,0,sizeof(*p));
    p->p.fd = fd;
    p->p.fd_out = fd;
    p->p.read  = (port_read_fn)udp_read;
    p->p.write = (port_write_fn)udp_write;
    p->p.pop = 0;

    return &p->p;
}


/***** 1.3. PACKETN */

#if 1  // needs testing

/* For byte streams, some kind of framing is necessary, so use Erlang
 * {packet,N} where every packet is prefixed with a big endian short
 * unsigned int with packet length. */

/* The read method can be shared, parameterized by a protocol-specific
 * "pop" method that attempts to read a packet from the buffer. */

struct buf_port {
    struct port p;
    uint32_t count;
    uint8_t buf[2*PACKET_MAX_SIZE];
};
static ssize_t pop_read(port_pop_fn pop,
                        struct buf_port *p, uint8_t *buf, ssize_t len) {
    ssize_t size;
    /* If we still have a packet, return that first. */
    if ((size = pop(p, buf, len))) return size;

    /* We get only one read() call, so make it count. */
    uint32_t room = sizeof(p->buf) - p->count;
    //LOG("packetn_read %d\n", p->count);
    ssize_t rv = read(p->p.fd, &p->buf[p->count], room);
    if (rv > 0) {
        //log_hex(&p->buf[p->count], rv);
    }
    //LOG("packetn_read done %d\n", rv);
    if (rv == -1) {
        switch (errno) {
        case EAGAIN:
            return 0;
        default:
            ASSERT_ERRNO(-1);
        }
    }
    if (rv == 0) {
        ERROR("eof");
    }
    ASSERT(rv > 0);
    p->count += rv;
    return pop(p, buf, len);
}



struct packetn_port {
    struct buf_port p;
    uint32_t len_bytes;
};

uint32_t packetn_packet_size(struct packetn_port *p) {
    ASSERT(p->len_bytes <= 4);
    ASSERT(p->p.count >= p->len_bytes);
    uint32_t size = 0;
    for (uint32_t i=0; i<p->len_bytes; i++) {
        size = (size << 8) + p->p.buf[i];
    }
    //LOG("size %d\n", size);
    return size;
}
uint32_t packetn_packet_write_size(struct packetn_port *p, uint32_t size, uint8_t *buf) {
    ASSERT(p->len_bytes <= 4);
    for (uint32_t i=0; i<p->len_bytes; i++) {
        buf[p->len_bytes-1-i] = size & 0xFF;
        size = size >> 8;
    }
    return size;
}

static ssize_t packetn_pop(struct packetn_port *p, uint8_t *buf, ssize_t len) {
    /* Make sure there are enough bytes to get the size field. */
    if (p->p.count < p->len_bytes) return 0;
    uint32_t size = packetn_packet_size(p);

    /* Packets are assumed to fit in the buffer.  An error here is
     * likely a bug or a protocol {packet,N} framing error. */
    if (sizeof(p->p.buf) < p->len_bytes + size) {
        ERROR("buffer overflow for stream packet size=%d\n", size);
    }

    /* Ensure packet is complete and fits in output buffer before
     * copying.  Skip the size prefix, which is used only for stream
     * transport framing. */
    if (p->p.count < p->len_bytes + size) return 0;
    ASSERT(size <= len);
    memcpy(buf, &p->p.buf[p->len_bytes], size);
    //LOG("copied %d:\n", size);
    //log_hex(buf, size);


    /* If there is anything residue, move it to the front. */
    if (p->p.count == p->len_bytes+size) {
        p->p.count = 0;
    }
    else {
        ssize_t head_count = p->len_bytes + size;
        ssize_t tail_count = p->p.count - head_count;
        memmove(&p->p.buf[0], &p->p.buf[head_count], tail_count);
        //LOG("moved %d %d:\n", p->count, tail_count);
        //log_hex(&p->buf[0],tail_count);
        p->p.count = tail_count;
    }
    //LOG("pop: %d %d\n", size, p->count);
    return size;
}

static ssize_t packetn_read(struct packetn_port *p, uint8_t *buf, ssize_t len) {
    return pop_read((port_pop_fn)packetn_pop, &p->p, buf, len);
}

static ssize_t packetn_write(struct packetn_port *p, uint8_t *buf, ssize_t len) {
    int fd = p->p.p.fd_out;

    //LOG("packetn_write %d\n", len);
    uint8_t size[p->len_bytes];
    packetn_packet_write_size(p, len, &size[0]);
    assert_write(fd, &size[0], p->len_bytes);
    assert_write(fd, buf, len);
    //LOG("packetn_write %d (done)\n", len);
    return len + p->len_bytes;
}
struct port *port_open_packetn_stream(uint32_t len_bytes, int fd, int fd_out) {
    struct packetn_port *p;
    ASSERT(p = malloc(sizeof(*p)));
    memset(p,0,sizeof(*p));
    p->p.p.fd = fd;
    p->p.p.fd_out = fd_out;
    p->p.p.read  = (port_read_fn)packetn_read;
    p->p.p.write = (port_write_fn)packetn_write;
    p->p.p.pop   = (port_pop_fn)packetn_pop;
    p->len_bytes = len_bytes;
    return &p->p.p;
}
struct port *port_open_packetn_tty(uint32_t len_bytes, const char *dev) {
    int fd;
    ASSERT_ERRNO(fd = open(dev, O_RDWR | O_NONBLOCK));

    struct termios2 tio;
    ASSERT(0 == ioctl(fd, TCGETS2, &tio));

    // http://www.cs.uleth.ca/~holzmann/C/system/ttyraw.c
    tio.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    tio.c_oflag &= ~(OPOST);
    tio.c_cflag |= (CS8);
    tio.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;

    ASSERT(0 == ioctl(fd, TCSETS2, &tio));

    return port_open_packetn_stream(len_bytes, fd, fd);
}

#endif



/***** 1.4. SLIP */

#if 1  // needs testing

/* See https://en.wikipedia.org/wiki/Serial_Line_Internet_Protocol */
#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

/* For byte streams, some kind of framing is necessary, so use Erlang
 * {packet,N} where every packet is prefixed with a big endian short
 * unsigned int with packet length. */

struct slip_port {
    struct buf_port p;
};

/* Try to pop a frame.  If it's not complete, return 0.  p->buf
 * contains slip-encoded data.  It is allowed to use the output buffer
 * to perform partial decoding. */
static ssize_t slip_pop(struct slip_port *p, uint8_t *buf, ssize_t len) {

    // 1. Go over data and stop at packet boundary, writing partial
    // data to output buffer.  Abort with 0 size when packet is
    // incomplete.
    ssize_t in = 0, out = 0;
    for(;;) {
        ASSERT(out < len);
        if (in >= p->p.count) return 0;
        uint8_t c = p->p.buf[in++];
        if (SLIP_END == c) {
            break;
        }
        else if (SLIP_ESC == c) {
            if (in >= p->p.count) return 0;
            uint8_t c = p->p.buf[in++];
            if (SLIP_ESC_ESC == c) {
                buf[out++] = SLIP_ESC;
            }
            else if (SLIP_ESC_END == c) {
                buf[out++] = SLIP_END;
            }
            else {
                ERROR("bad slip escape %d\n", (int)c);
            }
        }
        else {
            buf[out++] = c;
        }
    }

    //LOG("slip_pop: "); log_hex(&p->p.buf[0], in);


    // 2. Shift the data buffer
    memmove(&p->p.buf[0], &p->p.buf[in], p->p.count-in);
    p->p.count -= in;

    return out;
}
static ssize_t slip_read(struct packetn_port *p, uint8_t *buf, ssize_t len) {
    return pop_read((port_pop_fn)slip_pop, &p->p, buf, len);
}

static ssize_t slip_write(struct slip_port *p, uint8_t *buf, ssize_t len) {
    /* Use a temporary buffer to avoid multiple write() calls.  Worst
     * case size is x2, when each character is escaped, plus 2 for
     * double-ended delimiters. */
    uint8_t tmp[len*2 + 2];

    ssize_t out = 0;

    /* Convention: write packet boundary at the beginning and the
     * start.  Receiver needs to throw away empty (or otherwise
     * invalid) packets. */
    tmp[out++] = SLIP_END;
    for(ssize_t in=0; in<len; in++) {
        int c_in = buf[in];
        if (SLIP_END == c_in) {
            tmp[out++] = SLIP_ESC;
            tmp[out++] = SLIP_ESC_END;
        }
        else if (SLIP_ESC == c_in) {
            tmp[out++] = SLIP_ESC;
            tmp[out++] = SLIP_ESC_ESC;
        }
        else {
            tmp[out++] = c_in;
        }
    }
    tmp[out++] = SLIP_END;

    //LOG("slip_write: "); log_hex(tmp, out);

    assert_write(p->p.p.fd_out, tmp, out);
    return out;
}
struct port *port_open_slip_stream(int fd, int fd_out) {
    struct slip_port *p;
    ASSERT(p = malloc(sizeof(*p)));
    memset(p,0,sizeof(*p));
    p->p.p.fd = fd;
    p->p.p.fd_out = fd_out;
    p->p.p.read  = (port_read_fn)slip_read;
    p->p.p.write = (port_write_fn)slip_write;
    p->p.p.pop   = (port_pop_fn)slip_pop;
    return &p->p.p;
}
struct port *port_open_slip_tty(const char *dev) {
    int fd;
    ASSERT_ERRNO(fd = open(dev, O_RDWR | O_NONBLOCK));

    struct termios2 tio;
    ASSERT(0 == ioctl(fd, TCGETS2, &tio));

    // http://www.cs.uleth.ca/~holzmann/C/system/ttyraw.c
    tio.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    tio.c_oflag &= ~(OPOST);
    tio.c_cflag |= (CS8);
    tio.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;

    ASSERT(0 == ioctl(fd, TCSETS2, &tio));

    return port_open_slip_stream(fd, fd);
}

#endif

/***** 1.5. HEX */
struct hex_port {
    struct buf_port p;
    FILE *f_out;
};


static int hexdigit(int c) {
    if ((c >= '0') && (c <= '9')) return c - '0';
    if ((c >= 'A') && (c <= 'F')) return c - 'A' + 10;
    if ((c >= 'a') && (c <= 'f')) return c - 'a' + 10;
    return -1;
}


static ssize_t hex_pop(struct hex_port *p, uint8_t *buf, ssize_t len) {

    // 1. Go over data and stop at packet boundary, writing partial
    // data to output buffer.  Abort with 0 size when packet is
    // incomplete.
    ssize_t in = 0, out = 0;
    for(;;) {
        ASSERT(out < len);

        if (in >= p->p.count) return 0;
        uint8_t c1 = p->p.buf[in++];

        /* Check any control characters. */
        if (c1 == '\n') {
            /* Newline terminates. */
            break;
        };
        if (c1 == ' ') {
            /* Spaces are allowed inbetween hex bytes. */
            continue;
        }

        /* The only legal case left is two valid hex digits. */
        if (in >= p->p.count) return 0;
        uint8_t c2 = p->p.buf[in++];

        int d1, d2;
        ASSERT(-1 != (d1 = hexdigit(c1)));
        ASSERT(-1 != (d2 = hexdigit(c2)));

        buf[out++] = (d1 << 4) + d2;
    }

    // 2. Shift the data buffer
    memmove(&p->p.buf[0], &p->p.buf[in], p->p.count-in);
    p->p.count -= in;

    return out;
}


static ssize_t hex_read(struct hex_port *p, uint8_t *buf, ssize_t len) {
    return pop_read((port_pop_fn)hex_pop, &p->p, buf, len);
}
static ssize_t hex_write(struct hex_port *p, uint8_t *buf, ssize_t len) {
    ssize_t out = 0;
    for (ssize_t i=0; i<len; i++) { out += fprintf(p->f_out, " %02x", buf[i]); }
    out += fprintf(p->f_out, "\n");
    fflush(p->f_out);
    return out;
}
struct port *port_open_hex_stream(int fd, int fd_out) {
    struct hex_port *p;
    ASSERT(p = malloc(sizeof(*p)));
    memset(p,0,sizeof(*p));
    p->p.p.fd = fd;
    p->p.p.fd_out = fd_out;
    p->p.p.read  = (port_read_fn)hex_read;
    p->p.p.write = (port_write_fn)hex_write;
    p->p.p.pop   = (port_pop_fn)hex_pop;
    p->f_out = fdopen(fd_out, "w");
    return &p->p.p;
}




/***** 2. PACKET HANDLER */

/* Default behavior for the stand-alone program is to just forward a
 * packet to the other port.  Any other processing behavior is left to
 * application code that uses packet_bridge as a library.. */

void packet_forward(struct packet_handle_ctx *x, int from, const uint8_t *buf, ssize_t len) {
    int to = (from == 0) ? 1 : 0;
    x->port[to]->write(x->port[to], buf, len);
}






/***** 3. FRAMEWORK */

void packet_loop(packet_handle_fn handle,
                 struct packet_handle_ctx *ctx) {
    const char progress[] = "-\\|/";
    uint32_t count = 0;

    struct pollfd pfd[ctx->nb_ports];
    memset(pfd, 0, sizeof(pfd));

    for (int i=0; i<ctx->nb_ports; i++) {
        pfd[i].fd = ctx->port[i]->fd;
        pfd[i].events = POLLERR | POLLIN;
    }
    for(;;) {
        uint8_t buf[PACKET_MAX_SIZE]; // FIXME: Make this configurable
        int rv;
        ASSERT_ERRNO(rv = poll(&pfd[0], 2, ctx->timeout));
        ASSERT(rv >= 0);
        for (int i=0; i<ctx->nb_ports; i++) {
            if(pfd[i].revents & POLLIN) {
                struct port *in  = ctx->port[i];

                /* The read calls the underlying OS read method only
                 * once, so we are guaranteed to not block. */
                int rlen = in->read(in, buf, sizeof(buf));
                if (rlen) {
                    handle(ctx, i, buf, rlen);
                    count++;
                }
                else {
                    /* Port handler read data but dropped it. */
                }

                /* For streaming ports, it is possible that the OS
                 * read method returned multiple packets, so we pop
                 * them one by one. */
                if (in->pop) {
                    while((rlen = in->pop((struct buf_port *)in, buf, sizeof(buf)))) {
                        handle(ctx, i, buf, rlen);
                        count++;
                    }
                }
            }
        }
    }
}

struct port *port_open(const char *spec_ro) {
    char spec[strlen(spec_ro)+1];
    strcpy(spec, spec_ro);

    const char delim[] = ":";
    char *tok;
    ASSERT(tok = strtok(spec, delim));

    if (!strcmp(tok, "TAP")) {
        ASSERT(tok = strtok(NULL, delim));
        const char *tapdev = tok;
        ASSERT(NULL == (tok = strtok(NULL, delim)));
        //LOG("TAP:%s\n", tapdev);
        return port_open_tap(tapdev);
    }

    if (!strcmp(tok, "UDP-LISTEN")) {
        ASSERT(tok = strtok(NULL, delim));
        uint16_t port = atoi(tok);
        ASSERT(NULL == (tok = strtok(NULL, delim)));
        //LOG("UDP-LISTEN:%d\n", port);
        return port_open_udp(port);
    }

    if (!strcmp(tok, "UDP")) {
        ASSERT(tok = strtok(NULL, delim));
        const char *host = tok;
        ASSERT(tok = strtok(NULL, delim));
        uint16_t port = atoi(tok);
        ASSERT(NULL == (tok = strtok(NULL, delim)));
        //LOG("UDP-LISTEN:%s:%d\n", host, port);

        struct port *p = port_open_udp(0); // don't spec port here
        struct udp_port *up = (void*)p;

        struct hostent *hp;
        ASSERT(hp = gethostbyname(host));
        memcpy((char *)&up->peer.sin_addr,
               (char *)hp->h_addr_list[0],
               hp->h_length);
        up->peer.sin_port = htons(port);
        up->peer.sin_family = AF_INET;

        // FIXME: Send some meaningful ethernet packet instead
        // FIXME: Make this optional?  Or require application to initiate?
#if 0
        uint8_t buf[] = {
            0x55,0x55,0x55,0x55,0x55,0x55,
            0x55,0x55,0x55,0x55,0x55,0x55,
            0x55,0x55
        };
        LOG("udp: hello to ");
        log_addr(&up->peer);
        ASSERT(sizeof(buf) == p->write(p, buf, sizeof(buf)));
#else
        LOG("udp: not sending hello\n");
#endif
        return p;
    }

    if (!strcmp(tok, "TTY")) {
        ASSERT(tok = strtok(NULL, delim));
        if (!strcmp("slip", tok)) {
            ASSERT(tok = strtok(NULL, delim));
            const char *dev = tok;
            ASSERT(NULL == (tok = strtok(NULL, delim)));
            LOG("port_open_slip_tty(%s)\n", dev);
            return port_open_slip_tty(dev);
        }
        else {
            uint16_t len_bytes = atoi(tok);
            ASSERT(tok = strtok(NULL, delim));
            const char *dev = tok;
            ASSERT(NULL == (tok = strtok(NULL, delim)));
            return port_open_packetn_tty(len_bytes, dev);
        }
    }

    if (!strcmp(tok, "-")) {
        ASSERT(tok = strtok(NULL, delim));
        if (!strcmp("slip", tok)) {
            ASSERT(NULL == (tok = strtok(NULL, delim)));
            return port_open_slip_stream(0, 1);
        }
        else {
            uint16_t len_bytes = atoi(tok);
            ASSERT(NULL == (tok = strtok(NULL, delim)));
            return port_open_packetn_stream(len_bytes, 0, 1);
        }
    }

    if (!strcmp(tok, "HEX")) {
        ASSERT(NULL == (tok = strtok(NULL, delim)));
        return port_open_hex_stream(0, 1);
    }

    // FIXME: debug hex output/input

    ERROR("unknown type %s\n", tok);
}

int packet_forward_main(int argc, char **argv) {
    ASSERT(argc > 2);
    struct port *port[2];
    struct packet_handle_ctx ctx = {
        .nb_ports = 2,
        .port = port,
        .timeout = -1 // infinity
    };
    ASSERT(port[0] = port_open(argv[1]));
    ASSERT(port[1] = port_open(argv[2]));
    packet_loop(packet_forward, &ctx);
}



/* To set up UDP someone needs to send a first packet.  All other
   packets will go back to the first peer.  These are the
   configurations:

   udp2udp <port1> <port2> [<dst_host> <dst_port>]

   tap2udp <tap>   <port>  [<dst_host> <dst_port>]

   dst_host, dst_port are optional.  If present, a first packet is
   sent to initate communication.

   It seems that will solve all topologies.

   EDIT: I need to think about this some more.  Problem is that I
   don't understand UDP very well, i.e.: how can I let the OS pick a
   port?  Because in the a-symmetric situation, I want one listening
   port that is well known, but the other port can be arbitrary since
   it will be chained to the next hop, and that one will reply to any
   port that is specified.  I think I get it now, just figure out how
   to do this.  I guess this just boils down to binding or not.

   So what about this: add L: or C: prefixes to set up the initial
   direction of the socket.

   Follow socat suntax:

   TAP:tap0
   UDP4-LISTEN:port
   UDP4:host:port


   Setup:
   - ssh to the endpoint, start udp-listen A + tap
   - ssh to the midpont,  start udp-listen B + udp-connect to A
   - .. other midpoints ..
   - local: start tap + udp-connect to B
   - gather all pids doing so and close ssh connections
   - monitor connection, if it goes down tear down old and rebuild

*/




