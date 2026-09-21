#include "rx_buffer.h"

void rx_reset(RX_BUFFER *buffer)
{
    buffer->head = buffer->tail = 0;
    buffer->failed = false;
}

void rx_fail(RX_BUFFER *buffer)
{
    buffer->head = buffer->tail = 0;
    buffer->failed = true;
}

bool rx_push(RX_BUFFER *buffer, uint8_t byte)
{
    if (buffer->failed) return false;
    unsigned next = (buffer->head + 1) % RX_CAPACITY;
    if (next == buffer->tail) {
        rx_fail(buffer);
        return false;
    }
    buffer->data[buffer->head] = byte;
    buffer->head = next;
    return true;
}

int rx_read(RX_BUFFER *buffer, uint8_t *byte)
{
    if (buffer->failed) {
        rx_reset(buffer);
        return -1;
    }
    if (buffer->head == buffer->tail) return 0;
    *byte = buffer->data[buffer->tail];
    buffer->tail = (buffer->tail + 1) % RX_CAPACITY;
    return 1;
}
