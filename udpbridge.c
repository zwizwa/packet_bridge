/* This is a testing tool.  Don't shoot yourself in the foot.

   For ease of configuration, UDP packets are sent to the first peer
   that sends a message to a listening socket and ignored afterwards.
   If you know the UDP address, you can essentially gain unrestricted
   raw Ethernet access to whatever the tap interface is bridged to.
*/


#include "system.h"

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


// https://stackoverflow.com/questions/1003684/how-to-interface-with-the-linux-tun-driver
// https://www.kernel.org/doc/Documentation/networking/tuntap.txt
// https://wiki.wireshark.org/Development/LibpcapFileFormat

static inline void log_packet(uint8_t *buf, ssize_t n) {
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


// Sockets and taps are different, so wrap behind interface.
struct port;
typedef ssize_t (*port_read)(struct port *, uint8_t *, ssize_t);
typedef ssize_t (*port_write)(struct port *, uint8_t *, ssize_t);
struct port {
    int fd;
    int fd_out;
    port_read read;
    port_write write;
};

static ssize_t tap_read(struct port *p, uint8_t *buf, ssize_t len) {
    ssize_t rlen;
    ASSERT_ERRNO(rlen = read(p->fd, buf, len));
    return rlen;
}
static ssize_t tap_write(struct port *p, uint8_t *buf, ssize_t len) {
    // EIO is normal until iface is set up
    return write(p->fd, buf, len);
}

static inline struct port *open_tap(const char *dev) {
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
    return port;
}

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

static inline struct port *open_udp(uint16_t port) {
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
    p->p.read  = (port_read)udp_read;
    p->p.write = (port_write)udp_write;

    return &p->p;
}

#if 1  // needs testing

/* For byte streams, some kind of framing is necessary, so use Erlang
 * {packet,N} where every packet is prefixed with a big endian short
 * unsigned int with packet length. */

struct stream_port {
    struct port p;
    uint32_t count;
    uint32_t len_bytes;
    uint8_t buf[2048];
};

uint32_t stream_packet_size(struct stream_port *p) {
    ASSERT(p->len_bytes <= 4);
    ASSERT(p->count >= p->len_bytes);
    uint32_t size = 0;
    for (uint32_t i=0; i<p->len_bytes; i++) {
        size = (size << 8) + p->buf[i];
    }
    //LOG("size %d\n", size);
    return size;
}
uint32_t stream_packet_write_size(struct stream_port *p, uint32_t size, uint8_t *buf) {
    ASSERT(p->len_bytes <= 4);
    for (uint32_t i=0; i<p->len_bytes; i++) {
        buf[p->len_bytes-1-i] = size & 0xFF;
        size = size >> 8;
    }
    return size;
}

static ssize_t stream_pop(struct stream_port *p, uint8_t *buf, ssize_t len) {
    /* Make sure there are enough bytes to get the size field. */
    if (p->count < p->len_bytes) return 0;
    uint32_t size = stream_packet_size(p);

    /* Packets are assumed to fit in the buffer.  An error here is
     * likely a bug or a protocol {packet,N} framing error. */
    if (sizeof(p->buf) < p->len_bytes + size) {
        ERROR("buffer overflow for stream packet size=%d\n", size);
    }

    /* Ensure packet is complete and fits in output buffer before
     * copying.  Skip the size prefix, which is used only for stream
     * transport framing. */
    if (p->count < p->len_bytes + size) return 0;
    ASSERT(size <= len);
    memcpy(buf, &p->buf[p->len_bytes], size);
    //LOG("copied %d:\n", size);
    //log_packet(buf, size);


    /* If there is anything residue, move it to the front. */
    if (p->count == p->len_bytes+size) {
        p->count = 0;
    }
    else {
        ssize_t head_count = p->len_bytes + size;
        ssize_t tail_count = p->count - head_count;
        memmove(&p->buf[0], &p->buf[head_count], tail_count);
        //LOG("moved %d %d:\n", p->count, tail_count);
        //log_packet(&p->buf[0],tail_count);
        p->count = tail_count;
    }
    //LOG("pop: %d %d\n", size, p->count);
    return size;
}
static ssize_t stream_read(struct stream_port *p, uint8_t *buf, ssize_t len) {
    ssize_t size;
    /* If we still have a packet, return that first. */
    if ((size = stream_pop(p, buf, len))) return size;

    /* We get only one read() call, so make it count. */
    uint32_t room = sizeof(p->buf) - p->count;
    //LOG("stream_read %d\n", p->count);
    ssize_t rv = read(p->p.fd, &p->buf[p->count], room);
    if (rv > 0) {
        //log_packet(&p->buf[p->count], rv);
    }
    //LOG("stream_read done %d\n", rv);
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
    return stream_pop(p, buf, len);
}
static void assert_write(int fd, uint8_t *buf, uint32_t len) {
    uint32_t written = 0;
    while(written < len) {
        int rv;
        ASSERT((rv = write(fd, buf, len)) > 0);
        written += rv;
    }
}


static ssize_t stream_write(struct stream_port *p, uint8_t *buf, ssize_t len) {
    int fd = p->p.fd_out;

    //LOG("stream_write %d\n", len);
    uint8_t size[p->len_bytes];
    stream_packet_write_size(p, len, &size[0]);
    assert_write(fd, &size[0], p->len_bytes);
    assert_write(fd, buf, len);
    //LOG("stream_write %d (done)\n", len);
    return len + p->len_bytes;
}
static inline struct port *open_stream(uint32_t len_bytes, int fd, int fd_out) {
    struct stream_port *p;
    ASSERT(p = malloc(sizeof(*p)));
    memset(p,0,sizeof(*p));
    p->p.fd = fd;
    p->p.fd_out = fd_out;
    p->p.read  = (port_read)stream_read;
    p->p.write = (port_write)stream_write;
    p->len_bytes = len_bytes;
    return &p->p;
}
static inline struct port *open_tty(uint32_t len_bytes, const char *dev) {
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

    return open_stream(len_bytes, fd, fd);
}

#endif



static inline void proxy(struct port **port) {
    const char progress[] = "-\\|/";
    uint32_t count = 0;

    struct pollfd pfd[2] = {};
    for (int i=0; i<2; i++) {
        pfd[i].fd = port[i]->fd;
        pfd[i].events = POLLERR | POLLIN;
    }
    for(;;) {
        uint8_t buf[4096];
        int rv;
        ASSERT_ERRNO(rv = poll(&pfd[0], 2, -1));
        ASSERT(rv >= 0);
        for (int i=0; i<2; i++) {
            if(pfd[i].revents & POLLIN) {
                struct port *in  = port[i];
                struct port *out = port[1-i];

                int rlen = in->read(in, buf, sizeof(buf));
                if (rlen) {
                    //LOG("%d: %d\n", i, rlen);
                    //log_packet(buf, rlen);

                    out->write(out, buf, rlen);
                    //LOG("\r%c (%d)", progress[count % 4], count);
                    count++;
                }
                else {
                    /* Port handler read data but dropped it. */
                }
            }
        }
    }
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

struct port *from_portspec(char *spec) {
    const char delim[] = ":";
    char *tok;
    ASSERT(tok = strtok(spec, delim));

    if (!strcmp(tok, "TAP")) {
        ASSERT(tok = strtok(NULL, delim));
        const char *tapdev = tok;
        ASSERT(NULL == (tok = strtok(NULL, delim)));
        //LOG("TAP:%s\n", tapdev);
        return open_tap(tapdev);
    }

    if (!strcmp(tok, "UDP-LISTEN")) {
        ASSERT(tok = strtok(NULL, delim));
        uint16_t port = atoi(tok);
        ASSERT(NULL == (tok = strtok(NULL, delim)));
        //LOG("UDP-LISTEN:%d\n", port);
        return open_udp(port);
    }

    if (!strcmp(tok, "UDP")) {
        ASSERT(tok = strtok(NULL, delim));
        const char *host = tok;
        ASSERT(tok = strtok(NULL, delim));
        uint16_t port = atoi(tok);
        ASSERT(NULL == (tok = strtok(NULL, delim)));
        //LOG("UDP-LISTEN:%s:%d\n", host, port);

        struct port *p = open_udp(0); // don't spec port here
        struct udp_port *up = (void*)p;

        struct hostent *hp;
        ASSERT(hp = gethostbyname(host));
        memcpy((char *)&up->peer.sin_addr,
               (char *)hp->h_addr_list[0],
               hp->h_length);
        up->peer.sin_port = htons(port);
        up->peer.sin_family = AF_INET;

        // FIXME: Send some meaningful ethernet packet instead
        uint8_t buf[] = {
            0x55,0x55,0x55,0x55,0x55,0x55,
            0x55,0x55,0x55,0x55,0x55,0x55,
            0x55,0x55
        };
        LOG("udp: hello to ");
        log_addr(&up->peer);
        ASSERT(sizeof(buf) == p->write(p, buf, sizeof(buf)));
        return p;
    }

    if (!strcmp(tok, "TTY")) {
        ASSERT(tok = strtok(NULL, delim));
        uint16_t len_bytes = atoi(tok);
        ASSERT(tok = strtok(NULL, delim));
        const char *dev = tok;
        ASSERT(NULL == (tok = strtok(NULL, delim)));
        return open_tty(len_bytes, dev);
    }

    if (!strcmp(tok, "-")) {
        ASSERT(tok = strtok(NULL, delim));
        uint16_t len_bytes = atoi(tok);
        ASSERT(NULL == (tok = strtok(NULL, delim)));
        return open_stream(len_bytes, 0, 1);
    }

    ERROR("unknown type %s\n", tok);
}


int main(int argc, char **argv) {
    ASSERT(argc > 2);
    struct port *port[2];
    ASSERT(port[0] = from_portspec(argv[1]));
    ASSERT(port[1] = from_portspec(argv[2]));
    proxy(port);
}
