#ifndef CSEMVER_GITIGNORE_H
#define CSEMVER_GITIGNORE_H

/*
  Reports whether a path is ignored by the nearest .gitignore.

  Supports the glob subset Git ignore patterns use here: brace expansion,
  extglob groups including the negated form, and the pattern whitespace rules.
  The platform matcher is used only where a translated pattern cannot represent
  the construct, so behavior stays uniform across libc implementations.
*/
int csemver_path_is_gitignored(const char *filename);

#endif
