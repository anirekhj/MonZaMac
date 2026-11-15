#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include "../runtime/nvn/pipeline_archive_policy.h"

static void write_file(const char *path, const void *data, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, data, size) == (ssize_t)size && !close(fd));
}

static void sized_archive(off_t size) {
    int fd = open("archive", O_CREAT | O_RDWR | O_TRUNC, 0600);
    assert(fd >= 0 && !ftruncate(fd, size));
    if (size >= 8) {
        assert(pwrite(fd, "HEAD", 4, 0) == 4);
        assert(pwrite(fd, "TAIL", 4, size - 4) == 4);
    }
    assert(!close(fd));
}

static void unchanged(const struct stat *before) {
    struct stat after;
    assert(!lstat("archive", &after));
    assert(archive_policy_same(archive_policy_fingerprint(before), archive_policy_fingerprint(&after)));
    char data[4]; int fd = open("archive", O_RDONLY); assert(fd >= 0);
    assert(pread(fd, data, 4, 0) == 4 && !memcmp(data, "HEAD", 4));
    assert(pread(fd, data, 4, after.st_size - 4) == 4 && !memcmp(data, "TAIL", 4));
    assert(!close(fd));
}

static void test_boundaries(void) {
    uint64_t bytes = 99;
    assert(archive_load_decision("archive", "marker", &bytes) == ARCHIVE_LOAD_MISSING && !bytes);
    sized_archive(0);
    assert(archive_load_decision("archive", "marker", &bytes) == ARCHIVE_LOAD_INVALID && !bytes);
    sized_archive((off_t)PIPELINE_ARCHIVE_MAX_BYTES);
    struct stat before; assert(!lstat("archive", &before));
    assert(archive_load_decision("archive", "marker", &bytes) == ARCHIVE_LOAD_READY);
    assert(bytes == PIPELINE_ARCHIVE_MAX_BYTES);
    unchanged(&before);
    sized_archive((off_t)PIPELINE_ARCHIVE_MAX_BYTES + 1);
    assert(!lstat("archive", &before));
    assert(archive_load_decision("archive", "marker", &bytes) == ARCHIVE_LOAD_OVERSIZED);
    assert(bytes == PIPELINE_ARCHIVE_MAX_BYTES + 1 && !archive_mark_loading("archive", "marker"));
    assert(access("marker", F_OK) != 0); unchanged(&before);
}

static void test_interrupted_and_changed(void) {
    sized_archive(32);
    struct stat before; assert(!lstat("archive", &before));
    assert(archive_mark_loading("archive", "marker"));
    assert(archive_load_decision("archive", "marker", NULL) == ARCHIVE_LOAD_INTERRUPTED);
    unchanged(&before);
    struct timespec times[2] = {{.tv_nsec = UTIME_OMIT}, {0}};
    ArchivePolicyFingerprint fp = archive_policy_fingerprint(&before);
    times[1].tv_sec = (time_t)(fp.seconds + 1); times[1].tv_nsec = (long)fp.nanoseconds;
    assert(!utimensat(AT_FDCWD, "archive", times, 0));
    assert(archive_load_decision("archive", "marker", NULL) == ARCHIVE_LOAD_READY);
    assert(archive_mark_loading("archive", "marker"));
    assert(archive_load_decision("archive", "marker", NULL) == ARCHIVE_LOAD_INTERRUPTED);
    assert(!unlink("marker"));
    assert(archive_load_decision("archive", "marker", NULL) == ARCHIVE_LOAD_READY);
}

static void test_malformed_markers(void) {
    sized_archive(32);
    struct stat before; assert(!lstat("archive", &before));
    const char *bad[] = {"", "unknown", "MONZAMAC_ARCHIVE_LOAD 2\n1 2 3 4 5\n",
        ARCHIVE_POLICY_MARKER_HEADER "1 2 3 4 1000000000\n",
        ARCHIVE_POLICY_MARKER_HEADER "-1 2 3 4 5\n",
        ARCHIVE_POLICY_MARKER_HEADER "1 2 0 4 5\n",
        ARCHIVE_POLICY_MARKER_HEADER "1 2 3 4 5\ntrailing",
        ARCHIVE_POLICY_MARKER_HEADER "1 2 3 99999999999999999999999999999 5\n"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        write_file("marker", bad[i], strlen(bad[i]));
        assert(archive_load_decision("archive", "marker", NULL) == ARCHIVE_LOAD_INVALID);
        unchanged(&before);
    }
    char large[ARCHIVE_POLICY_MARKER_MAX + 1]; memset(large, 'x', sizeof large);
    write_file("marker", large, sizeof large);
    assert(archive_load_decision("archive", "marker", NULL) == ARCHIVE_LOAD_INVALID);
    unchanged(&before);
}

static void test_redirect_guards(void) {
    sized_archive(32);
    struct stat before; assert(!lstat("archive", &before));
    assert(!symlink("archive", "archive-link"));
    assert(archive_load_decision("archive-link", "marker", NULL) == ARCHIVE_LOAD_INVALID);
    assert(!archive_mark_loading("archive-link", "marker"));
    assert(!symlink("archive", "marker"));
    assert(archive_load_decision("archive", "marker", NULL) == ARCHIVE_LOAD_INVALID);
    assert(!archive_mark_loading("archive", "marker")); unchanged(&before);
    assert(!unlink("marker"));
    assert(!archive_mark_loading("archive", "archive")); unchanged(&before);
    assert(!link("archive", "marker"));
    assert(!archive_mark_loading("archive", "marker")); unchanged(&before);
    assert(!unlink("marker"));
    assert(!mkfifo("marker", 0600));
    assert(archive_load_decision("archive", "marker", NULL) == ARCHIVE_LOAD_INVALID);
    assert(!archive_mark_loading("archive", "marker")); unchanged(&before);
    assert(!unlink("marker"));
    assert(!mkdir("marker", 0700));
    assert(archive_load_decision("archive", "marker", NULL) == ARCHIVE_LOAD_INVALID);
    assert(!archive_mark_loading("archive", "marker")); unchanged(&before);
}

static void test_marker_identity_and_budget(void) {
    sized_archive(32);
    write_file("other", "another authored file", 21);
    assert(archive_mark_loading("other", "marker"));
    struct stat source, mark_before, mark_after;
    assert(!lstat("archive", &source) && !lstat("marker", &mark_before));
    assert(archive_load_decision("archive", "marker", NULL) == ARCHIVE_LOAD_READY);
    assert(!lstat("marker", &mark_after));
    assert(archive_policy_same(archive_policy_fingerprint(&mark_before), archive_policy_fingerprint(&mark_after)));
    unchanged(&source);
    assert(archive_descriptor_budget_available(0));
    assert(archive_descriptor_budget_available(PIPELINE_ARCHIVE_MAX_DESCRIPTORS - 1));
    assert(!archive_descriptor_budget_available(PIPELINE_ARCHIVE_MAX_DESCRIPTORS));
    assert(!archive_descriptor_budget_available(UINT64_MAX));
    assert(archive_load_decision(NULL, "marker", NULL) == ARCHIVE_LOAD_INVALID);
    assert(!archive_mark_loading("archive", NULL));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    char root[] = "/tmp/monzamac-archive-policy.XXXXXX";
    assert(mkdtemp(root) && !chdir(root));
    if (!strcmp(argv[1], "boundaries")) test_boundaries();
    else if (!strcmp(argv[1], "interrupted")) test_interrupted_and_changed();
    else if (!strcmp(argv[1], "malformed")) test_malformed_markers();
    else if (!strcmp(argv[1], "redirects")) test_redirect_guards();
    else if (!strcmp(argv[1], "identity-budget")) test_marker_identity_and_budget();
    else abort();
    unlink("marker"); rmdir("marker"); unlink("archive-link"); unlink("archive"); unlink("other");
    assert(!chdir("/") && !rmdir(root));
    puts("PASS authored archive policy scenario");
    return 0;
}
