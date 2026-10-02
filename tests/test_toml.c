#include "../vendor/tomlc99/toml.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char config[] = "releaseAs = \"minor\"\n"
                    "packageFiles = [{ filename = \"VERSION\", type = \"plain-text\" }]\n"
                    "[skip]\n"
                    "bump = true\n";
    char error[128];
    toml_table_t *root = toml_parse(config, error, sizeof error);
    toml_datum_t release;
    toml_array_t *files;
    toml_table_t *file;
    toml_datum_t filename;
    toml_datum_t skip;
    toml_table_t *skip_table;

    assert(root != NULL);
    release = toml_string_in(root, "releaseAs");
    assert(release.ok && strcmp(release.u.s, "minor") == 0);
    files = toml_array_in(root, "packageFiles");
    assert(files != NULL && toml_array_nelem(files) == 1);
    file = toml_table_at(files, 0);
    assert(file != NULL);
    filename = toml_string_in(file, "filename");
    assert(filename.ok && strcmp(filename.u.s, "VERSION") == 0);
    skip_table = toml_table_in(root, "skip");
    assert(skip_table != NULL);
    skip = toml_bool_in(skip_table, "bump");
    assert(skip.ok && skip.u.b);
    free(release.u.s);
    free(filename.u.s);
    toml_free(root);
    puts("TOML parser tests passed");
    return 0;
}
