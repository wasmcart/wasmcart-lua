/*
 * cartconf.c - implementation of Lua's redirected write hooks.
 *
 * Lua emits diagnostics in fragments (print() writes each argument, then a
 * separate newline), so we accumulate into a line buffer and flush to wc_log
 * on newline or when full. That turns N fragment calls into one host log line,
 * which is what a reader actually wants in the event trace.
 */
#include "cartconf.h"
#include <stdio.h>
#include <string.h>

#define LINE_MAX_LEN 512
static char line_buf[LINE_MAX_LEN];
static size_t line_len;

static void flush_line(void) {
    if (line_len == 0) return;
    wc_log(line_buf, (unsigned int)line_len);
    line_len = 0;
}

void wcl_write(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\n') { flush_line(); continue; }
        if (line_len < LINE_MAX_LEN - 1) line_buf[line_len++] = c;
        else { flush_line(); line_buf[line_len++] = c; }
    }
}

/* lua_writestringerror(fmt, arg): always a single "%s"-shaped format */
void wcl_writef(const char *fmt, const char *arg) {
    char tmp[LINE_MAX_LEN];
    int n = snprintf(tmp, sizeof tmp, fmt, arg ? arg : "");
    if (n < 0) return;
    if (n > (int)sizeof tmp - 1) n = (int)sizeof tmp - 1;
    wcl_write(tmp, (size_t)n);
    flush_line();
}
