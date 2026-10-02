#include <stdio.h>
#include <string.h>

#define CSEMVER_VERSION "0.1.0"

static void print_help(void) {
  puts("Usage: csemver [options]\n"
       "\n"
       "Options:\n"
       "  -h, --help                 Show this help\n"
       "  -v, --version              Show version\n"
       "  -r, --release-as VERSION   Set the release type or version\n"
       "  -p, --prerelease [ID]      Create a pre-release\n"
       "  -f, --first-release        Do not bump the version\n"
       "  -t, --tag-prefix PREFIX    Prefix the created tag (default: v)\n"
       "  -i, --infile FILE          Changelog path (default: CHANGELOG.md)\n"
       "      --dry-run              Preview without modifying the repository\n"
       "  -n, --no-verify            Bypass git commit hooks\n"
       "  -a, --commit-all           Commit all staged files\n"
       "      --skip STEP            Skip bump, changelog, commit, or tag\n"
       "  -c, --config FILE          Read configuration from TOML");
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_help();
      return 0;
    }
    if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
      puts("csemver " CSEMVER_VERSION);
      return 0;
    }
    if (argv[i][0] == '-') {
      fprintf(stderr, "csemver: unknown option '%s'\n", argv[i]);
      return 2;
    }
  }
  fprintf(stderr, "csemver: release engine is not initialized\n");
  return 1;
}
