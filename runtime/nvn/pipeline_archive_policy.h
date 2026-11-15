// Conservative archive budgets and interrupted-load detection, not Metal timeouts.
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PIPELINE_ARCHIVE_MAX_BYTES (64ull << 20)
#define PIPELINE_ARCHIVE_MAX_DESCRIPTORS 512
#define ARCHIVE_POLICY_MARKER_MAX 256
#define ARCHIVE_POLICY_MARKER_HEADER "MONZAMAC_ARCHIVE_LOAD 1\n"

typedef enum ArchiveLoadDecision {
    ARCHIVE_LOAD_MISSING,
    ARCHIVE_LOAD_READY,
    ARCHIVE_LOAD_OVERSIZED,
    ARCHIVE_LOAD_INTERRUPTED,
    ARCHIVE_LOAD_INVALID
} ArchiveLoadDecision;

typedef struct {
    uintmax_t device, inode, size;
    intmax_t seconds;
    uintmax_t nanoseconds;
} ArchivePolicyFingerprint;

static inline ArchivePolicyFingerprint archive_policy_fingerprint(const struct stat *info) {
#if defined(__APPLE__)
    intmax_t seconds = (intmax_t)info->st_mtimespec.tv_sec;
    uintmax_t nanoseconds = (uintmax_t)info->st_mtimespec.tv_nsec;
#else
    intmax_t seconds = (intmax_t)info->st_mtim.tv_sec;
    uintmax_t nanoseconds = (uintmax_t)info->st_mtim.tv_nsec;
#endif
    return (ArchivePolicyFingerprint){(uintmax_t)info->st_dev, (uintmax_t)info->st_ino,
                                     (uintmax_t)info->st_size, seconds, nanoseconds};
}

static inline bool archive_policy_same(ArchivePolicyFingerprint a, ArchivePolicyFingerprint b) {
    return a.device == b.device && a.inode == b.inode && a.size == b.size &&
           a.seconds == b.seconds && a.nanoseconds == b.nanoseconds;
}

static inline bool archive_policy_number(const char **cursor, const char *end, char delimiter,
                                         bool sign_allowed, bool *negative, uintmax_t *value) {
    const char *p = *cursor;
    *negative = false;
    *value = 0;
    if (sign_allowed && p < end && *p == '-') { *negative = true; p++; }
    if (p == end || *p < '0' || *p > '9') return false;
    do {
        unsigned digit = (unsigned)(*p++ - '0');
        if (*value > (UINTMAX_MAX - digit) / 10) return false;
        *value = *value * 10 + digit;
    } while (p < end && *p >= '0' && *p <= '9');
    if (p == end || *p++ != delimiter) return false;
    *cursor = p;
    return true;
}

static inline bool archive_policy_parse(const char *data, size_t size, ArchivePolicyFingerprint *out) {
    const size_t header = sizeof(ARCHIVE_POLICY_MARKER_HEADER) - 1;
    if (size < header || memcmp(data, ARCHIVE_POLICY_MARKER_HEADER, header)) return false;
    const char *p = data + header, *end = data + size;
    uintmax_t seconds;
    bool negative, ignored;
    if (!archive_policy_number(&p, end, ' ', false, &ignored, &out->device) ||
        !archive_policy_number(&p, end, ' ', false, &ignored, &out->inode) ||
        !archive_policy_number(&p, end, ' ', false, &ignored, &out->size) ||
        !archive_policy_number(&p, end, ' ', true, &negative, &seconds) ||
        seconds > (uintmax_t)INTMAX_MAX + (negative ? 1u : 0u) ||
        !archive_policy_number(&p, end, '\n', false, &ignored, &out->nanoseconds) ||
        p != end || !out->size || out->size > PIPELINE_ARCHIVE_MAX_BYTES || out->nanoseconds >= 1000000000u)
        return false;
    out->seconds = negative ? -(intmax_t)(seconds - (seconds != 0)) - (seconds != 0) : (intmax_t)seconds;
    return true;
}

static inline ArchiveLoadDecision archive_load_decision(const char *archive, const char *marker, uint64_t *bytes) {
    if (bytes) *bytes = 0;
    if (!archive || !marker || !*archive || !*marker) return ARCHIVE_LOAD_INVALID;
    struct stat source;
    if (lstat(archive, &source)) return errno == ENOENT ? ARCHIVE_LOAD_MISSING : ARCHIVE_LOAD_INVALID;
    if (!S_ISREG(source.st_mode) || source.st_size < 0) return ARCHIVE_LOAD_INVALID;
    if (bytes) *bytes = (uint64_t)source.st_size;
    if (!source.st_size) return ARCHIVE_LOAD_INVALID;
    if ((uintmax_t)source.st_size > PIPELINE_ARCHIVE_MAX_BYTES) return ARCHIVE_LOAD_OVERSIZED;
    struct stat before;
    if (lstat(marker, &before)) return errno == ENOENT ? ARCHIVE_LOAD_READY : ARCHIVE_LOAD_INVALID;
    if (!S_ISREG(before.st_mode) || before.st_size <= 0 || before.st_size > ARCHIVE_POLICY_MARKER_MAX)
        return ARCHIVE_LOAD_INVALID;
    int fd = open(marker, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return ARCHIVE_LOAD_INVALID;
    struct stat opened, after;
    bool valid = !fstat(fd, &opened) && S_ISREG(opened.st_mode) &&
                 archive_policy_same(archive_policy_fingerprint(&before), archive_policy_fingerprint(&opened));
    char data[ARCHIVE_POLICY_MARKER_MAX + 1];
    size_t count = 0;
    while (valid && count < sizeof data) {
        ssize_t n = read(fd, data + count, sizeof data - count);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { valid = false; break; }
        if (!n) break;
        count += (size_t)n;
    }
    valid = valid && count == (size_t)before.st_size && !fstat(fd, &after) &&
            archive_policy_same(archive_policy_fingerprint(&opened), archive_policy_fingerprint(&after));
    if (close(fd)) valid = false;
    struct stat current;
    valid = valid && !lstat(marker, &current) && S_ISREG(current.st_mode) &&
            archive_policy_same(archive_policy_fingerprint(&opened), archive_policy_fingerprint(&current));
    ArchivePolicyFingerprint recorded;
    if (!valid || !archive_policy_parse(data, count, &recorded)) return ARCHIVE_LOAD_INVALID;
    return archive_policy_same(recorded, archive_policy_fingerprint(&source))
        ? ARCHIVE_LOAD_INTERRUPTED : ARCHIVE_LOAD_READY;
}

static inline bool archive_mark_loading(const char *archive, const char *marker) {
    if (!archive || !marker || !*archive || !*marker) return false;
    struct stat source;
    if (lstat(archive, &source) || !S_ISREG(source.st_mode) || source.st_size <= 0 ||
        (uintmax_t)source.st_size > PIPELINE_ARCHIVE_MAX_BYTES) return false;
    ArchivePolicyFingerprint fingerprint = archive_policy_fingerprint(&source);
    char data[ARCHIVE_POLICY_MARKER_MAX];
    int count = snprintf(data, sizeof data, ARCHIVE_POLICY_MARKER_HEADER
                         "%ju %ju %ju %jd %ju\n", fingerprint.device, fingerprint.inode,
                         fingerprint.size, fingerprint.seconds, fingerprint.nanoseconds);
    if (count <= 0 || (size_t)count >= sizeof data) return false;
    // Validate before truncation: a marker must never alias the archive or another hard-linked file.
    int fd = open(marker, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0) return false;
    struct stat opened;
    bool valid = !fstat(fd, &opened) && S_ISREG(opened.st_mode) && opened.st_nlink == 1 &&
                 opened.st_uid == geteuid() &&
                 (opened.st_dev != source.st_dev || opened.st_ino != source.st_ino);
    if (valid) valid = !fchmod(fd, 0600) && !ftruncate(fd, 0);
    size_t done = 0;
    while (valid && done < (size_t)count) {
        ssize_t n = write(fd, data + done, (size_t)count - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { valid = false; break; }
        done += (size_t)n;
    }
    if (valid && fsync(fd)) valid = false;
    if (close(fd)) valid = false;
    return valid;
}

static inline bool archive_descriptor_budget_available(uint64_t descriptors) {
    return descriptors < PIPELINE_ARCHIVE_MAX_DESCRIPTORS;
}
