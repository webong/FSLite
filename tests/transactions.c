#define _POSIX_C_SOURCE 200809L
#include "fslite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef FSLITE_TEST_SQLITE
#include "sqlite3.h"
#endif
static uint32_t seed, state;
static unsigned batch, step;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "seed=%08x batch=%u step=%u %s:%d: %s\n", seed, batch, step, __FILE__, __LINE__, #x); exit(1); } } while (0)
#define STATUS(call, want) do { fslite_status got = (call); if (got != (want)) { \
    fprintf(stderr, "seed=%08x batch=%u step=%u %s:%d: %s: got %d want %d\n", seed, batch, step, __FILE__, __LINE__, #call, got, (want)); exit(1); } } while (0)
#define OK(call) STATUS(call, FSLITE_OK)
enum { FILES = 4, CAPACITY = 131079 };
struct file { int exists; size_t size; unsigned char bytes[CAPACITY]; };
struct model { int directories[2]; struct file files[2][FILES]; };
static uint32_t next(void) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state;
}
static void path_for(char path[32], unsigned directory, unsigned file) {
    snprintf(path, 32, "/%c/f%u", 'a' + directory, file);
}
static size_t length(void) {
    const size_t edges[] = {0, 1, 65535, 65536, 65537, CAPACITY};
    return edges[next() % (sizeof edges / sizeof *edges)];
}
struct listing { const struct model *model; int directory; unsigned seen; };
static int32_t collect(void *context, const char *name, size_t length, const fslite_info *info) {
    struct listing *listing = context;
    unsigned index;
    if (listing->directory < 0) {
        CHECK(length == 1 && (name[0] == 'a' || name[0] == 'b'));
        index = (unsigned)(name[0] - 'a'); CHECK(listing->model->directories[index]);
        CHECK(info->kind == FSLITE_DIRECTORY && info->size == 0);
    } else {
        CHECK(length == 2 && name[0] == 'f' && name[1] >= '0' && name[1] < '0' + FILES);
        index = (unsigned)(name[1] - '0');
        const struct file *file = &listing->model->files[listing->directory][index];
        CHECK(file->exists && info->kind == FSLITE_FILE && info->size == file->size);
    }
    CHECK(!(listing->seen & (1u << index))); listing->seen |= 1u << index;
    return 0;
}
static void verify(fslite_volume *v, const struct model *model) {
    OK(fslite_check(v));
    struct listing root = {model, -1, 0}; OK(fslite_list(v, "/", collect, &root));
    CHECK(root.seen == (unsigned)(model->directories[0] | (model->directories[1] << 1)));
    for (unsigned d = 0; d < 2; ++d) {
        char directory[] = "/a"; directory[1] += (char)d;
        fslite_info info;
        STATUS(fslite_stat(v, directory, &info), model->directories[d] ? FSLITE_OK : FSLITE_NOT_FOUND);
        if (!model->directories[d]) continue;
        struct listing listing = {model, (int)d, 0}; unsigned wanted = 0;
        OK(fslite_list(v, directory, collect, &listing));
        for (unsigned i = 0; i < FILES; ++i) {
            char path[32]; path_for(path, d, i); const struct file *file = &model->files[d][i];
            STATUS(fslite_stat(v, path, &info), file->exists ? FSLITE_OK : FSLITE_NOT_FOUND);
            if (!file->exists) continue;
            wanted |= 1u << i; CHECK(info.kind == FSLITE_FILE && info.size == file->size);
            unsigned char buffer[8191]; size_t count;
            for (size_t offset = 0; offset < file->size; offset += count) {
                OK(fslite_read(v, path, offset, buffer, sizeof buffer, &count));
                size_t expected = file->size - offset < sizeof buffer ? file->size - offset : sizeof buffer;
                CHECK(count == expected && memcmp(buffer, file->bytes + offset, count) == 0);
            }
            OK(fslite_read(v, path, file->size, buffer, sizeof buffer, &count)); CHECK(count == 0);
        }
        CHECK(listing.seen == wanted);
    }
}
static void mutate(fslite_volume *v, struct model *model, unsigned char *data) {
    unsigned d = next() % 2, i = next() % FILES, op = next() % 11;
    char path[32], directory[] = "/a"; path_for(path, d, i); directory[1] += (char)d;
    struct file *file = &model->files[d][i];
    for (size_t k = 0; k < CAPACITY; ++k) data[k] = (unsigned char)(k * 31 + step * 7 + batch);
    fslite_status missing = model->directories[d] && file->exists ? FSLITE_OK : FSLITE_NOT_FOUND;
    if (op == 0) {
        STATUS(fslite_mkdir(v, directory), model->directories[d] ? FSLITE_ALREADY_EXISTS : FSLITE_OK);
        model->directories[d] = 1;
    } else if (op == 1) {
        size_t n = length();
        STATUS(fslite_write(v, path, data, n), model->directories[d] ? FSLITE_OK : FSLITE_NOT_FOUND);
        if (model->directories[d]) { file->exists = 1; file->size = n; memcpy(file->bytes, data, n); }
    } else if (op == 2) {
        size_t n = length(); STATUS(fslite_truncate(v, path, n), missing);
        if (missing == FSLITE_OK) {
            if (n > file->size) memset(file->bytes + file->size, 0, n - file->size);
            file->size = n;
        }
    } else if (op == 3) {
        size_t offset = length(), n = next() % 97;
        if (n > CAPACITY - offset) n = CAPACITY - offset;
        STATUS(fslite_write_at(v, path, offset, data, n), missing);
        if (missing == FSLITE_OK && n) {
            if (offset > file->size) memset(file->bytes + file->size, 0, offset - file->size);
            memcpy(file->bytes + offset, data, n);
            if (offset + n > file->size) file->size = offset + n;
        }
    } else if (op == 4) {
        STATUS(fslite_remove(v, path), missing);
        if (missing == FSLITE_OK) { file->exists = 0; file->size = 0; }
    } else if (op == 5) {
        unsigned td = next() % 2, ti = next() % FILES; char target[32]; path_for(target, td, ti);
        fslite_status expected = missing == FSLITE_OK && model->directories[td] ? FSLITE_OK : FSLITE_NOT_FOUND;
        STATUS(fslite_rename(v, path, target), expected);
        if (expected == FSLITE_OK && (td != d || ti != i)) {
            model->files[td][ti] = *file; file->exists = 0; file->size = 0;
        }
    } else if (op == 6 || op == 7) {
        fslite_writer *writer = NULL;
        STATUS(fslite_writer_open(v, path, &writer), model->directories[d] ? FSLITE_OK : FSLITE_NOT_FOUND);
        if (!model->directories[d]) return;
        size_t n = length();
        for (size_t offset = 0; offset < n;) {
            size_t fragment = 1 + next() % 17003; if (fragment > n - offset) fragment = n - offset;
            OK(fslite_writer_write(writer, data + offset, fragment)); offset += fragment;
        }
        STATUS(fslite_check(v), FSLITE_INVALID_STATE);
        if (op == 6) { OK(fslite_writer_finish(writer)); file->exists = 1; file->size = n; memcpy(file->bytes, data, n); }
        fslite_writer_close(writer); // op 7 aborts, preserving prior file/transaction work.
    } else if (op == 8) {
        int nonempty = 0; for (unsigned j = 0; j < FILES; ++j) nonempty |= model->files[d][j].exists;
        fslite_status expected = !model->directories[d] ? FSLITE_NOT_FOUND : nonempty ? FSLITE_NOT_EMPTY : FSLITE_OK;
        STATUS(fslite_remove(v, directory), expected);
        if (expected == FSLITE_OK) model->directories[d] = 0;
    } else if (op == 10) {
        unsigned td = 1 - d; char target[] = "/a"; target[1] += (char)td;
        int nonempty = 0; for (unsigned j = 0; j < FILES; ++j) nonempty |= model->files[td][j].exists;
        fslite_status expected = !model->directories[d] ? FSLITE_NOT_FOUND : nonempty ? FSLITE_NOT_EMPTY : FSLITE_OK;
        STATUS(fslite_rename(v, directory, target), expected);
        if (expected == FSLITE_OK) {
            memcpy(model->files[td], model->files[d], sizeof model->files[d]);
            memset(model->files[d], 0, sizeof model->files[d]);
            model->directories[d] = 0; model->directories[td] = 1;
        }
    } else {
        // Guaranteed failure after prior successful operations in the transaction.
        STATUS(fslite_rename(v, directory, "/"), FSLITE_INVALID_ARGUMENT);
        STATUS(fslite_truncate(v, path, UINT64_MAX), FSLITE_TOO_LARGE);
    }
}
static void run_seed(uint32_t value) {
    seed = state = value;
    char path[] = "/tmp/fslite-transactions-XXXXXX"; int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0);
    struct model *working = calloc(1, sizeof *working), *committed = calloc(1, sizeof *committed);
    unsigned char *data = malloc(CAPACITY); CHECK(working && committed && data);
    fslite_volume *v = NULL, *reader = NULL;
    OK(fslite_open(path, FSLITE_OPEN_CREATE, &v)); OK(fslite_open(path, 0, &reader));
    OK(fslite_mkdir(v, "/a")); OK(fslite_mkdir(v, "/b"));
    working->directories[0] = working->directories[1] = 1; *committed = *working;
    for (batch = 0; batch < 24; ++batch) {
        OK(fslite_begin(v));
        for (step = 0; step < 6; ++step) {
            mutate(v, working, data); CHECK(fslite_in_transaction(v)); verify(v, working);
        }
        verify(reader, committed); // No pending edits or aborted streams leak.
        unsigned outcome = batch % 3;
        if (outcome == 0) { OK(fslite_commit(v)); *committed = *working; }
        else if (outcome == 1) { OK(fslite_rollback(v)); *working = *committed; }
        else { fslite_close(v); v = NULL; *working = *committed; }
        if (v) fslite_close(v);
        OK(fslite_open(path, 0, &v)); CHECK(!fslite_in_transaction(v));
        verify(v, committed); verify(reader, committed);
    }
    fslite_close(reader); fslite_close(v); CHECK(unlink(path) == 0);
    free(data); free(working); free(committed);
}
#ifdef FSLITE_TEST_SQLITE
static void injected_failures(void) {
    // Deterministic pseudo-random payloads and failure positions exercise rollback
    // after chunks have already changed, rather than only argument rejection.
    seed = state = 0xc001d00d; batch = step = 0;
    char path[] = "/tmp/fslite-txfault-XXXXXX"; int fd = mkstemp(path);
    CHECK(fd >= 0 && close(fd) == 0);
    unsigned char *data = malloc(CAPACITY); CHECK(data);
    for (size_t i = 0; i < CAPACITY; ++i) data[i] = (unsigned char)next();
    fslite_volume *v = NULL; OK(fslite_open(path, FSLITE_OPEN_CREATE, &v));
    OK(fslite_write(v, "/file", data, CAPACITY));
    sqlite3 *db = NULL; CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    for (step = 0; step < 12; ++step) {
        char query[256]; unsigned position = 1 + next() % 2;
        snprintf(query, sizeof query, "CREATE TRIGGER fail BEFORE INSERT ON chunks WHEN NEW.chunk_index=%u "
            "BEGIN SELECT RAISE(ABORT,'injected failure'); END", position);
        CHECK(sqlite3_exec(db, query, NULL, NULL, NULL) == SQLITE_OK);
        OK(fslite_begin(v)); OK(fslite_mkdir(v, "/prior"));
        unsigned char *replacement = malloc(CAPACITY); CHECK(replacement);
        for (size_t i = 0; i < CAPACITY; ++i) replacement[i] = (unsigned char)next();
        // Alternate replacement (which deletes old chunks) and overlapping patch.
        if (step % 2)
            STATUS(fslite_write_at(v, "/file", 0, replacement, CAPACITY), FSLITE_INTERNAL_ERROR);
        else
            STATUS(fslite_write(v, "/file", replacement, CAPACITY), FSLITE_INTERNAL_ERROR);
        free(replacement);
        CHECK(fslite_in_transaction(v)); OK(fslite_check(v));
        fslite_info info; OK(fslite_stat(v, "/prior", &info));
        unsigned char *actual = malloc(CAPACITY); size_t count; CHECK(actual);
        OK(fslite_read(v, "/file", 0, actual, CAPACITY, &count));
        CHECK(count == CAPACITY && memcmp(actual, data, CAPACITY) == 0); free(actual);
        if (step % 2) { OK(fslite_commit(v)); OK(fslite_remove(v, "/prior")); }
        else { OK(fslite_rollback(v)); STATUS(fslite_stat(v, "/prior", &info), FSLITE_NOT_FOUND); }
        CHECK(sqlite3_exec(db, "DROP TRIGGER fail", NULL, NULL, NULL) == SQLITE_OK);
    }
    CHECK(sqlite3_close(db) == SQLITE_OK); fslite_close(v); CHECK(unlink(path) == 0); free(data);
}
#endif
void test_transactions(void) {
    const uint32_t seeds[] = {0x51f17e, 0xdeadbeef, 0x12345678};
    for (size_t i = 0; i < sizeof seeds / sizeof *seeds; ++i) run_seed(seeds[i]);
#ifdef FSLITE_TEST_SQLITE
    injected_failures();
#endif
}
