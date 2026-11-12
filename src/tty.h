#ifndef CSEMVER_TTY_H
#define CSEMVER_TTY_H

#include <stdio.h>

#include "config.h"

/*
  Terminal output.

  Color detection checks FORCE_COLOR, then CI provider variables, then the
  terminal name, applied to stdout or a given stream. Checkpoint glyphs are
  colored by meaning, and the dry-run tick is yellow where a real release
  tick is green.
*/
int csemver_terminal_supports_color(FILE *stream);
void csemver_print_error_line(const char *text);
void csemver_print_checkpoint_tick(const CsemverConfig *config);
void csemver_print_checkpoint_cross(void);
void csemver_print_checkpoint_info(void);
void csemver_print_bold(const char *text);

#endif
