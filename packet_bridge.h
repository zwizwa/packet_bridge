#ifndef PACKET_BRIDGE_H
#define PACKET_BRIDGE_H

#include <stdint.h>
#include <sys/types.h>

// Port read/write access and instantiation is abstract
struct port;
struct buf_port;
typedef ssize_t (*port_read_fn)(struct port *, uint8_t *, ssize_t);
typedef ssize_t (*port_write_fn)(struct port *, const uint8_t *, ssize_t);
typedef ssize_t (*port_pop_fn)(struct buf_port *p, uint8_t *buf, ssize_t len);

struct port {
    int fd;              // main file descriptor
    int fd_out;          // optional, if different from main fd
    port_read_fn read;
    port_write_fn write;
    port_pop_fn pop;     // only for buffered ports
};
struct port *port_open_tap(const char *dev);
struct port *port_open_udp(uint16_t port);
struct port *port_open_packetn_stream(uint32_t len_bytes, int fd, int fd_out);
struct port *port_open_packetn_tty(uint32_t len_bytes, const char *dev);
struct port *port_open_slip_stream(int fd, int fd_out);
struct port *port_open_slip_tty(const char *dev);
struct port *port_open_hex_stream(int fd, int fd_out);
struct port *port_open(const char *spec);


// Packet handling is abstracted.  We provide the mainloop, and the
// application provides the handler and port instantiation code.
struct packet_handle_ctx {
    int nb_ports;
    struct port **port;
    int timeout;
};
typedef void (*packet_handle_fn)(struct packet_handle_ctx *, int src, const uint8_t *, ssize_t);
void packet_loop(packet_handle_fn forward, struct packet_handle_ctx *ctx);


// As an example, we provide a handler and instantiator that performs
// simple forwarding between two packet ports.
void packet_forward(struct packet_handle_ctx *, int from, const uint8_t *buf, ssize_t len);
int packet_forward_main(int argc, char **argv);


// FIXME: Don't make buffers static size.
#define PACKET_MAX_SIZE 4096


#endif
