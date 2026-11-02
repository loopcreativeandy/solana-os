#pragma once

#include <stddef.h>

void console_start(void);

// Console line speed: baud, or 0 for the configured one. Wait until what's
// printed so far has gone out first. 0 = done, -1 = this console has no line
// speed (the emulator's).
int console_set_baud(int baud);

// Raw console input, for a command that takes a stream after its line
// (lua recv): up to n bytes, waiting at most timeout_ms for them (0: none
// came), -1 where the console has no input stream (the emulator's). n = 0
// only asks which. Call it only from inside a command.
int console_stream_read(void *buf, size_t n, int timeout_ms);
