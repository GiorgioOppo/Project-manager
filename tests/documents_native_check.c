#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "documents.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char key[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char sha[] = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
static const unsigned char pdf[] = "%PDF-1.7\ncontract fixture\n%%EOF\n";

int main(int argc, char **argv) {
    assert(argc == 4);
    const char *scenario = argv[1];
    assert(setenv("DOC_FILES_DIR", argv[2], 1) == 0);
    if (strcmp(scenario, "directory_symlink") == 0) {
        assert(!documents_open(argv[3]));
        assert(!documents_health());
        return 0;
    }
    assert(documents_open(argv[3]));
    assert(documents_health());
    unsigned char *loaded = NULL;
    if (strcmp(scenario, "roundtrip") == 0) {
        assert(documents_store(key, "Contratto città.pdf", pdf, sizeof(pdf), sha));
        assert(documents_load(key, sizeof(pdf), &loaded));
        assert(memcmp(loaded, pdf, sizeof(pdf)) == 0);
        free(loaded);
        assert(documents_remove(key));
        assert(documents_remove(key));
        assert(!documents_load(key, sizeof(pdf), &loaded));
        assert(loaded == NULL);
    } else if (strcmp(scenario, "invalid") == 0) {
        assert(!documents_store("../escape", "x.pdf", pdf, sizeof(pdf), sha));
        assert(!documents_store(key, "../x.pdf", pdf, sizeof(pdf), sha));
        assert(!documents_store(key, "a\r\nx.pdf", pdf, sizeof(pdf), sha));
        assert(!documents_store(key, "x\".pdf", pdf, sizeof(pdf), sha));
        assert(!documents_store(key, "x\xc0\x80.pdf", pdf, sizeof(pdf), sha));
        assert(!documents_store(key, "x\xe2.pdf", pdf, sizeof(pdf), sha));
        assert(!documents_store(key, "x.txt", pdf, sizeof(pdf), sha));
        assert(!documents_store(key, "x.pdf", pdf, 0, sha));
        assert(!documents_store(key, "x.pdf", pdf, DOCUMENTS_MAX_BYTES + 1, sha));
        assert(!documents_store(key, "x.pdf", (const unsigned char *)"hello", 5, sha));
        assert(!documents_store(key, "x.pdf", pdf, sizeof(pdf), "not-a-sha"));
        char long_name[DOCUMENTS_FILENAME_MAX + 2];
        memset(long_name, 'a', sizeof(long_name));
        memcpy(long_name + sizeof(long_name) - 5, ".pdf", 5);
        assert(!documents_store(key, long_name, pdf, sizeof(pdf), sha));
    } else if (strcmp(scenario, "duplicate") == 0) {
        assert(documents_store(key, "x.pdf", pdf, sizeof(pdf), sha));
        const unsigned char replacement[] = "%PDF-other";
        assert(!documents_store(key, "changed.pdf", replacement, sizeof(replacement), sha));
        assert(documents_load(key, sizeof(pdf), &loaded));
        assert(memcmp(loaded, pdf, sizeof(pdf)) == 0);
        free(loaded);
    } else if (strcmp(scenario, "lengths") == 0) {
        assert(documents_store(key, "x.pdf", pdf, sizeof(pdf), sha));
        assert(!documents_load(key, sizeof(pdf) - 1, &loaded));
        assert(loaded == NULL);
        assert(!documents_load(key, sizeof(pdf) + 1, &loaded));
        assert(!documents_load(key, 0, &loaded));
        assert(!documents_load(key, DOCUMENTS_MAX_BYTES + 1, &loaded));
        assert(!documents_load("../escape", sizeof(pdf), &loaded));
        assert(!documents_load(key, sizeof(pdf), NULL));
    } else if (strcmp(scenario, "file_symlink") == 0) {
        char link_path[4096];
        assert(snprintf(link_path, sizeof(link_path), "%s/%s", argv[2], key) > 0);
        assert(symlink(argv[3], link_path) == 0);
        assert(!documents_load(key, sizeof(pdf), &loaded));
        assert(!documents_store(key, "x.pdf", pdf, sizeof(pdf), sha));
        assert(documents_remove(key));
        assert(access(argv[3], F_OK) == 0);
    } else if (strcmp(scenario, "permissions") == 0) {
        assert(documents_store(key, "x.pdf", pdf, sizeof(pdf), sha));
        char file_path[4096];
        assert(snprintf(file_path, sizeof(file_path), "%s/%s", argv[2], key) > 0);
        struct stat info;
        assert(stat(argv[2], &info) == 0 && (info.st_mode & 0777) == 0700);
        assert(stat(file_path, &info) == 0 && (info.st_mode & 0777) == 0600);
        assert(chmod(file_path, 0644) == 0);
        assert(!documents_load(key, sizeof(pdf), &loaded));
        assert(chmod(argv[2], 0755) == 0);
        assert(!documents_health());
        assert(!documents_store(key, "x.pdf", pdf, sizeof(pdf), sha));
    } else {
        assert(!"unknown test scenario");
    }
    documents_close();
    assert(!documents_health());
    return 0;
}
