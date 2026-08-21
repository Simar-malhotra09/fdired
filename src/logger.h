#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>
#include <stdio.h>
#define FDIRED_DEBUG
#ifdef FDIRED_DEBUG

static FILE *log_fp = NULL;

/* open path for writing; returns 0 on success, 1 on failure */
static inline int log_init(const char *path) {
  log_fp = fopen(path, "w");
  return log_fp == NULL;
}

/* printf-style write to the log file; no-op if log_init failed/wasn't called */
static inline void log_write(const char *fmt, ...) {
  if (!log_fp)
    return;
  va_list args;
  va_start(args, fmt);
  vfprintf(log_fp, fmt, args);
  va_end(args);
  fflush(log_fp);
}

static inline void log_close(void) {
  if (log_fp) {
    fclose(log_fp);
    log_fp = NULL;
  }
}

#else

static inline int log_init(const char *path) {
  (void)path;
  return 0;
}
static inline void log_write(const char *fmt, ...) { (void)fmt; }
static inline void log_close(void) {}

#endif /* FDIRED_DEBUG */

#endif /* LOGGER_H */
