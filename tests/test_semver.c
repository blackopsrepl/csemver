#include "../src/semver.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_bump(const char *input, const char *type,
                        const char *identifier, const char *expected) {
  Semver version;
  char output[SEMVER_TEXT_MAX];
  assert(semver_parse(input, &version));
  assert(semver_bump(&version, type, identifier, output, sizeof output));
  if (strcmp(output, expected) != 0) {
    fprintf(stderr, "%s %s -> %s (expected %s)\n", input, type, output,
            expected);
    assert(strcmp(output, expected) == 0);
  }
}

int main(void) {
  Semver version;
  char output[SEMVER_TEXT_MAX];

  assert(semver_parse("1.2.3", &version));
  expect_bump("1.2.3", "major", NULL, "2.0.0");
  expect_bump("1.2.3", "minor", NULL, "1.3.0");
  expect_bump("1.2.3", "patch", NULL, "1.2.4");
  expect_bump("0.4.9", "minor", NULL, "0.5.0");
  expect_bump("1.2.3", "prepatch", "alpha", "1.2.4-alpha.0");
  expect_bump("1.2.3", "prepatch", NULL, "1.2.4-0");
  expect_bump("1.2.3", "prerelease", NULL, "1.2.3-0");
  expect_bump("1.2.3-alpha.0", "prerelease", "alpha", "1.2.3-alpha.1");
  expect_bump("1.2.3-alpha", "prerelease", "alpha", "1.2.3-alpha.0");
  expect_bump("1.2.3-alpha.1", "patch", NULL, "1.2.3");

  assert(!semver_parse("01.2.3", &version));
  assert(!semver_parse("1.2", &version));
  assert(!semver_parse("1.2.3-01", &version));
  assert(!semver_parse("1.2.3garbage", &version));
  assert(semver_parse("2.0.0-rc.1+build.5", &version));
  assert(semver_format(&version, output, sizeof output));
  assert(strcmp(output, "2.0.0-rc.1+build.5") == 0);
  assert(semver_compare("2.0.0-rc.1", "2.0.0") < 0);
  assert(semver_compare("2.0.0-rc.2", "2.0.0-rc.1") > 0);
  assert(semver_compare("1.2.3-alpha.999999999999999999999",
                        "1.2.3-alpha.1000000000000000000000") < 0);
  assert(semver_compare("1.0.0-alpha", "1.0.0-alpha.0") < 0);
  assert(semver_compare("1.0.0-alpha.1", "1.0.0-alpha.beta") < 0);
  assert(semver_compare("1.2.3+build.1", "1.2.3+build.2") == 0);
  puts("semver tests passed");
  return 0;
}
