// Recent console lines in RAM for the Debug app's Console tab (os_log.h).
// One flat buffer of whole lines, oldest first; a line that doesn't fit drops
// the oldest ones. Writers never wait: a line that arrives while another task
// holds the lock (or from an interrupt) is printed but not kept.

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "os_log.h"

#define PART_MAX 160            // longest line kept; longer ones are cut

static char s_buf[OS_LOG_SIZE];
static size_t s_len;            // bytes used in s_buf, always whole lines
static char s_part[PART_MAX];   // the line being written, until its '\n'
static size_t s_plen;
static uint32_t s_first, s_next, s_gen;
static SemaphoreHandle_t s_lock;

static bool lock(void)
{
#ifdef ESP_PLATFORM
    if (xPortInIsrContext()) {
        return false;
    }
#endif
    return s_lock && xSemaphoreTake(s_lock, 0) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void push_line(void)
{
    size_t need = s_plen + 1;
    while (s_len && s_len + need > sizeof(s_buf)) {
        char *nl = memchr(s_buf, '\n', s_len);
        size_t drop = nl ? (size_t)(nl - s_buf) + 1 : s_len;
        memmove(s_buf, s_buf + drop, s_len - drop);
        s_len -= drop;
        s_first++;
    }
    memcpy(s_buf + s_len, s_part, s_plen);
    s_buf[s_len + s_plen] = '\n';
    s_len += need;
    s_plen = 0;
    s_next++;
    s_gen++;
}

static void add(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\n') {
            push_line();
        } else if (c == '\r') {
            continue;
        } else if (s_plen < PART_MAX) {
            s_part[s_plen++] = c == '\t' ? ' ' : c;
        }
    }
}

// Formats one printf-style line into the log (ap is left untouched).
static void keep(const char *fmt, va_list ap)
{
    if (!lock()) {
        return;
    }
    char line[PART_MAX + 2];
    va_list aq;
    va_copy(aq, ap);
    int n = vsnprintf(line, sizeof(line), fmt, aq);
    va_end(aq);
    if (n > 0) {
        add(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
        if ((size_t)n >= sizeof(line)) {
            add("\n", 1);      // cut: still end the line
        }
    }
    unlock();
}

void os_log_write(const char *text, size_t len)
{
    if (lock()) {
        add(text, len);
        unlock();
    }
}

void os_log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    keep(fmt, ap);
    vprintf(fmt, ap);
    va_end(ap);
}

void os_log_clear(void)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_first = s_next;
        s_len = 0;
        s_gen++;
        unlock();
    }
}

uint32_t os_log_gen(void)   { return s_gen; }
uint32_t os_log_first(void) { return s_first; }
uint32_t os_log_next(void)  { return s_next; }

bool os_log_line(uint32_t n, char *out, size_t size)
{
    bool ok = false;
    if (size == 0 || !s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    if (n >= s_first && n < s_next) {
        const char *p = s_buf, *end = s_buf + s_len;
        for (uint32_t i = s_first; i < n && p < end; i++) {
            const char *nl = memchr(p, '\n', end - p);
            p = nl ? nl + 1 : end;
        }
        const char *nl = memchr(p, '\n', end - p);
        size_t len = (nl ? nl : end) - p;
        if (len >= size) {
            len = size - 1;
        }
        memcpy(out, p, len);
        out[len] = 0;
        ok = true;
    }
    unlock();
    return ok;
}

#ifdef ESP_PLATFORM
// Every ESP_LOGx line goes through here (log v1, no colours: one call per line).
static vprintf_like_t s_prev;

static int log_vprintf(const char *fmt, va_list ap)
{
    keep(fmt, ap);
    return s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
}
#endif

void os_log_init(void)
{
    if (s_lock) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
#ifdef ESP_PLATFORM
    s_prev = esp_log_set_vprintf(log_vprintf);
#endif
}
