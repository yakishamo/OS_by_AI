/* Test-only linker wrapper: hold the consumer until a real IRQ fills the queue.
 * The UART handler and queue implementation remain the production code.
 */
#include "../../kernel/rx_buffer.h"

extern int __real_rx_read(RX_BUFFER *buffer, uint8_t *byte);

int __wrap_rx_read(RX_BUFFER *buffer, uint8_t *byte)
{
    static bool released;
    if (!released) {
        if (!buffer->failed) return 0;
        released = true;
    }
    return __real_rx_read(buffer, byte);
}
