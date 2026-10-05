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
  assert(semver_parse("v1.2.3", &version));
  assert(semver_format(&version, output, sizeof output));
  assert(strcmp(output, "1.2.3") == 0);
  assert(semver_clean(" \t=v1.2.3 \t\n", &version));
  assert(semver_format(&version, output, sizeof output));
  assert(strcmp(output, "1.2.3") == 0);
  assert(semver_clean("=v1.2.3+build.5", &version));
  assert(semver_format(&version, output, sizeof output));
  assert(strcmp(output, "1.2.3") == 0);
  assert(semver_parse(" \t1.2.3 \t\n", &version));
  assert(semver_format(&version, output, sizeof output));
  assert(strcmp(output, "1.2.3") == 0);
  assert(semver_parse("\xC2\xA0\xEF\xBB\xBF"
                      "1.2.3"
                      "\xE2\x80\x83",
                      &version));
  assert(semver_format(&version, output, sizeof output));
  assert(strcmp(output, "1.2.3") == 0);
  expect_bump("1.2.3", "major", NULL, "2.0.0");
  expect_bump("1.2.3", "minor", NULL, "1.3.0");
  expect_bump("1.2.3", "patch", NULL, "1.2.4");
  expect_bump("0.4.9", "minor", NULL, "0.5.0");
  expect_bump("1.2.3", "prepatch", "alpha", "1.2.4-alpha.0");
  expect_bump("1.2.3", "prepatch", NULL, "1.2.4-0");
  expect_bump("1.2.3", "prerelease", NULL, "1.2.3-0");
  expect_bump("1.2.3-alpha.0", "prerelease", "alpha", "1.2.3-alpha.1");
  expect_bump("1.2.3-alpha", "prerelease", "alpha", "1.2.3-alpha.0");
  expect_bump("1.2.3-beta.0.foo", "prerelease", "beta", "1.2.3-beta.1.foo");
  expect_bump("1.2.3-beta.foo", "prerelease", "beta", "1.2.3-beta.0");
  expect_bump("1.2.3-beta.foo", "prerelease", NULL, "1.2.3-beta.foo.0");
  expect_bump("1.2.3-beta.alpha.0.foo", "prerelease", "beta.alpha",
              "1.2.3-beta.alpha.1.foo");
  expect_bump("1.2.3-alpha.1", "patch", NULL, "1.2.3");
  expect_bump("1.3.0-alpha.0", "minor", NULL, "1.3.0");
  expect_bump("1.3.2-alpha.0", "minor", NULL, "1.4.0");
  expect_bump("0.0.0-alpha.0", "minor", NULL, "0.0.0");
  expect_bump("1.2.0-beta.0", "patch", NULL, "1.2.0");
  expect_bump("1.2.1-beta.0", "patch", NULL, "1.2.1");
  expect_bump("2.0.0-alpha.0", "major", NULL, "2.0.0");
  expect_bump("2.0.1-alpha.0", "major", NULL, "3.0.0");
  expect_bump("1.3.0-alpha.0", "preminor", "alpha", "1.4.0-alpha.0");
  expect_bump("1.3.0-alpha.0", "premajor", "alpha", "2.0.0-alpha.0");

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
