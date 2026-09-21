#ifndef KERNEL_RX_BUFFER_H
#define KERNEL_RX_BUFFER_H
#include <stdbool.h>
#include <stdint.h>

#define RX_CAPACITY 256
typedef struct {
    uint8_t data[RX_CAPACITY];
    unsigned head, tail;
    bool failed;
} RX_BUFFER;

/* Single CPU, caller holds interrupts disabled. 255 bytes usable.
 * On loss, discard queued data and reject input until read reports the error.
 */
void rx_reset(RX_BUFFER *buffer);
void rx_fail(RX_BUFFER *buffer);
bool rx_push(RX_BUFFER *buffer, uint8_t byte);
int rx_read(RX_BUFFER *buffer, uint8_t *byte);
#endif
