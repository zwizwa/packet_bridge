#ifndef PACKET_BRIDGE_H
#define PACKET_BRIDGE_H

#include <stdint.h>
#include <sys/types.h>

// Sockets and taps are different, so wrap behind interface.
struct port;
typedef ssize_t (*port_read_t)(struct port *, uint8_t *, ssize_t);
typedef ssize_t (*port_write_t)(struct port *, const uint8_t *, ssize_t);
struct port {
    int fd;              // main file descriptor
    int fd_out;          // optional, if different from main fd
    port_read_t read;
    port_write_t write;
};


// Processing functions
struct port_forward_ctx {
    int nb_ports;
    struct port **port;
};
typedef void (*port_forward_t)(struct port_forward_ctx *, int from, const uint8_t *, ssize_t);
// We don't provide processing here, just forwarding.
void packet_bridge_forward(struct port_forward_ctx *, int from, const uint8_t *buf, ssize_t len);

void packet_bridge_loop(port_forward_t forward,
                        struct port_forward_ctx *ctx);


// Default instantiation.
// For derivative applications, see this function's implementation.
int packet_bridge_forward_main(int argc, char **argv);


#endif
