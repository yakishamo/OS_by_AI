#include "console.h"
#include "serial.h"
#include "pmm.h"
#include "main.h"
#include "timer.h"
#include "../include/x86.h"

static void output(const char *text)
{
    if (!serial_write(text)) x86_spin_forever();
}

static bool equal(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static void number(uint64_t value)
{
    char text[21];
    unsigned i = sizeof(text) - 1;
    text[i] = 0;
    do { text[--i] = '0' + value % 10; value /= 10; } while (value);
    output(text + i);
}

static void execute(char *line, unsigned length)
{
    while (length && (line[length - 1] == ' ' || line[length - 1] == '\t')) --length;
    line[length] = 0;
    while (*line == ' ' || *line == '\t') ++line;
    if (!*line) return;
    if (equal(line, "help")) {
        output("help  - list commands\nmem   - physical page counts (4 KiB)\n"
               "ticks - timer interrupt count (~100 Hz)\n"
               "clear - clear terminal\nhalt  - stop CPU (restart QEMU to resume)\n");
    } else if (equal(line, "ticks")) {
        output("ticks: "); number(timer_ticks()); output("\n");
    } else if (equal(line, "mem")) {
        uint64_t total = pmm_total(), free = pmm_available();
        output("pages: total="); number(total);
        output(" used="); number(total - free);
        output(" free="); number(free); output("\n");
    } else if (equal(line, "clear")) {
        output("\033[2J\033[H");
    } else if (equal(line, "halt")) {
        output("KERNEL: halting\n");
        if (!serial_flush()) x86_spin_forever();
        x86_disable_interrupts();
        kernel_halt();
    } else {
        output("Unknown command. Type 'help'.\n");
    }
}

_Noreturn void console_run(void)
{
    char line[128];
    unsigned length = 0, escape = 0;
    bool after_cr = false, discard = false;
    output("CONSOLE: ready (type 'help')\nK> ");
    for (;;) {
        uint8_t byte;
        int status = serial_read(&byte);
        if (!status) { x86_idle(); continue; }
        if (status < 0) { discard = true; escape = 0; continue; }
        if (byte == '\n' && after_cr) { after_cr = false; continue; }
        after_cr = byte == '\r';
        if (byte == 3) { /* Ctrl-C cancels even an overlong line or escape. */
            length = escape = 0; discard = false;
            output("^C\nK> "); continue;
        }
        if (byte == '\r' || byte == '\n') {
            output("\n");
            if (discard) output("Input discarded (line too long or serial error).\n");
            else execute(line, length);
            length = escape = 0; discard = false;
            output("K> "); continue;
        }
        if (discard) continue;
        /* Ignore terminal CSI/SS3 keys so arrows cannot become commands. */
        if (escape) {
            if (escape == 1) escape = (byte == '[' || byte == 'O') ? 2 : 0;
            else if (byte >= 0x40 && byte <= 0x7e) escape = 0;
            continue;
        }
        if (byte == 27) { escape = 1; continue; }
        if (byte == 8 || byte == 127) {
            if (length) { --length; output("\b \b"); }
            continue;
        }
        if (byte != '\t' && (byte < 32 || byte > 126)) continue;
        if (length == sizeof(line) - 1) { discard = true; continue; }
        /* Normalize tabs to spaces, keeping backspace editing one column wide. */
        if (byte == '\t') byte = ' ';
        line[length++] = (char)byte;
        char echo[] = {(char)byte, 0};
        output(echo);
    }
}
