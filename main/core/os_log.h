#pragma once

// The console's recent lines kept in RAM (OS_LOG_SIZE bytes, oldest lines
// dropped first) so the Debug app can show them on the badge itself: the
// system log (ESP_LOGx), Lua print() and Lua app errors. Console command
// replies and the screen mirror's frames aren't kept.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define OS_LOG_SIZE 6144

void os_log_init(void);             // first thing at boot: hooks the system log
void os_log_write(const char *text, size_t len);    // lines end at '\n'
void os_log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));    // printf + keep
void os_log_clear(void);
uint32_t os_log_gen(void);          // changes whenever a line is added or the log is cleared
uint32_t os_log_first(void);        // number of the oldest line still kept
uint32_t os_log_next(void);         // number the next line will get

// Copies line n (without '\n') into out; false once it's gone or not there yet.
bool os_log_line(uint32_t n, char *out, size_t size);
