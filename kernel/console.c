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

typedef struct {
    char line[128];
    unsigned length, escape;
    bool after_cr, discard;
} LINE_EDITOR;

static void reset_line(LINE_EDITOR *editor)
{
    editor->length = 0;
    editor->escape = 0;
    editor->discard = false;
}

static void finish_line(LINE_EDITOR *editor)
{
    output("\n");
    if (editor->discard) output("Input discarded (line too long or serial error).\n");
    else execute(editor->line, editor->length);
    reset_line(editor);
    output("K> ");
}

static bool consume_escape(LINE_EDITOR *editor, uint8_t byte)
{
    /* Ignore terminal CSI/SS3 keys so arrows cannot become commands. */
    if (editor->escape == 1) {
        editor->escape = (byte == '[' || byte == 'O') ? 2 : 0;
        return true;
    }
    if (editor->escape == 2) {
        if (byte >= 0x40 && byte <= 0x7e) editor->escape = 0;
        return true;
    }
    if (byte != 27) return false;
    editor->escape = 1;
    return true;
}

static void edit_character(LINE_EDITOR *editor, uint8_t byte)
{
    if (editor->discard || consume_escape(editor, byte)) return;
    if (byte == 8 || byte == 127) {
        if (editor->length) {
            --editor->length;
            output("\b \b");
        }
        return;
    }
    if (byte != '\t' && (byte < 32 || byte > 126)) return;
    if (editor->length == sizeof(editor->line) - 1) {
        editor->discard = true;
        return;
    }
    /* Tabs occupy one column, just like other editable characters. */
    if (byte == '\t') byte = ' ';
    editor->line[editor->length++] = (char)byte;
    char echo[] = {(char)byte, 0};
    output(echo);
}

static void accept_input(LINE_EDITOR *editor, uint8_t byte)
{
    if (byte == '\n' && editor->after_cr) {
        editor->after_cr = false;
        return;
    }
    editor->after_cr = byte == '\r';
    if (byte == 3) { /* Ctrl-C also cancels overlong lines and escape sequences. */
        reset_line(editor);
        output("^C\nK> ");
        return;
    }
    if (byte == '\r' || byte == '\n') {
        finish_line(editor);
        return;
    }
    edit_character(editor, byte);
}

_Noreturn void console_run(void)
{
    LINE_EDITOR editor;
    reset_line(&editor);
    editor.after_cr = false;
    output("CONSOLE: ready (type 'help')\nK> ");
    for (;;) {
        uint8_t byte;
        int status = serial_read(&byte);
        if (!status) {
            x86_idle();
        } else if (status < 0) {
            editor.discard = true;
            editor.escape = 0;
        } else {
            accept_input(&editor, byte);
        }
    }
}
