// Small wrapper to allow packet_bridge.c to be used as a library.
// We don't provide processing here, just proxying.
#include "packet_bridge.h"
int main(int argc, char **argv) {
    struct port_forward_method fw[] = {
        {.object = 0, .forward = packet_bridge_forward},
        {.object = 0, .forward = packet_bridge_forward}
    };
    return packet_bridge_main(&fw[0], argc, argv);
}
