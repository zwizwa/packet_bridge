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
// We don't provide processing here, just forwarding.
typedef void (*port_forward_t)(void *, struct port *, const uint8_t *, ssize_t);
void packet_bridge_forward(void * no_context, struct port *out, const uint8_t *buf, ssize_t len);
struct port_forward_method {
    void *object;
    port_forward_t forward;
};

// Instantiation
int packet_bridge_main(struct port_forward_method *forward, int argc, char **argv);


#endif
