/*-------------------------------------------------------------------------
 *
 * logfile.h
 *	  The log file: where every log message is written, to stderr or to
 *	  a file the process rotates itself.
 *
 * Derived from PostgreSQL's postmaster/syslogger.h.
 *
 * Copyright (c) 2004-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 * src/include/utils/logfile.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGFILE_H
#define LOGFILE_H

#include <string>

/*
 * The file logger's settings (PGC_SIGHUP), as the main thread last read them.
 * logfile.c keeps them in its own state, under shmem->log.mutex.
 */
typedef struct logfile_conf_t {
  std::string directory = "log"; /* log_directory */
  /* log_filename, a strftime() pattern */
  std::string filename = "postgresql-%Y-%m-%d_%H%M%S.log";
  int file_mode = 0600;              /* log_file_mode */
  int rotation_age = 24 * 60;        /* log_rotation_age, minutes */
  int rotation_size = 10 * 1024;     /* log_rotation_size, kB */
  bool truncate_on_rotation = false; /* log_truncate_on_rotation */
} logfile_conf_t;

extern logfile_conf_t log_file_settings(void);
extern void logfile_open(void);
extern void logfile_reload(void);
extern bool logfile_rotate(void);
extern void logfile_write(const char *buf, int len);

#endif /* LOGFILE_H */
