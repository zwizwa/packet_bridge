#ifndef PACKET_BRIDGE_H
#define PACKET_BRIDGE_H

#include <stdint.h>
#include <sys/types.h>

// Port read/write access and instantiation is abstract
struct port;
typedef ssize_t (*port_read_fn)(struct port *, uint8_t *, ssize_t);
typedef ssize_t (*port_write_fn)(struct port *, const uint8_t *, ssize_t);
struct port {
    int fd;              // main file descriptor
    int fd_out;          // optional, if different from main fd
    port_read_fn read;
    port_write_fn write;
};
struct port *open_tap(const char *dev);
struct port *open_udp(uint16_t port);
struct port *open_packetn_stream(uint32_t len_bytes, int fd, int fd_out);
struct port *open_packetn_tty(uint32_t len_bytes, const char *dev);
struct port *open_slip_stream(int fd, int fd_out);
struct port *open_slip_tty(const char *dev);
struct port *open_hex_stream(int fd, int fd_out);


// Packet handling is abstracted.  We provide the mainloop, and the
// application provides the handler and port instantiation code.
typedef void (*port_forward_fn)(struct port_forward_ctx *, int src, const uint8_t *, ssize_t);
struct port_forward_ctx {
    int nb_ports;
    struct port **port;
};
void packet_loop(port_forward_fn forward, struct port_forward_ctx *ctx);


// As an example, we provide a handler and instantiator that performs
// simple forwarding between two packet ports.
void packet_forward(struct port_forward_ctx *, int from, const uint8_t *buf, ssize_t len);
int packet_forward_main(int argc, char **argv);


#endif
