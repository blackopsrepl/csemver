#ifndef CSEMVER_LIFECYCLE_H
#define CSEMVER_LIFECYCLE_H

#include "config.h"

/*
  Lifecycle hooks.

  A configured script runs through the shell with its output captured. A hook
  that exits non-zero aborts the release with status 1, and output beyond the
  configured buffer is treated as a failure rather than truncated silently.
*/
int csemver_run_lifecycle_command(const char *command, char **stdout_output,
                                  char **stderr_output, int *max_buffer_stream,
                                  int *exit_code);
int csemver_run_lifecycle_capture(const CsemverConfig *config, const char *name,
                                  char **stdout_output);
int csemver_run_lifecycle(const CsemverConfig *config, const char *name);

#endif
