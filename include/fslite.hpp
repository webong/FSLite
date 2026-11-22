#ifndef FSLITE_HPP
#define FSLITE_HPP

/* Header-only RAII wrappers for the FSLite C ABI.
 *
 * These classes own their handles and release them in destructors. They are
 * move-only and do not add exceptions, allocations, or hidden threads. Each
 * wrapper exposes get() for advanced calls and release() to transfer ownership.
 * The underlying C contract still applies: serialize use of one handle, close
 * each handle exactly once, and do not reenter a volume from a list callback.
 */

#include "fslite.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace fslite {

class Status {
public:
    Status() noexcept : code_(FSLITE_OK) {}
    explicit Status(fslite_status code) noexcept : code_(code) {}

    fslite_status code() const noexcept { return code_; }
    bool ok() const noexcept { return code_ == FSLITE_OK; }
    explicit operator bool() const noexcept { return ok(); }
    const char *message() const noexcept { return fslite_status_string(code_); }

    friend bool operator==(Status left, Status right) noexcept { return left.code_ == right.code_; }
    friend bool operator!=(Status left, Status right) noexcept { return !(left == right); }

private:
    fslite_status code_;
};

class Volume;
class Writer;
class Backup;

class Volume {
public:
    Volume() noexcept : handle_(nullptr) {}
    ~Volume() { reset(); }

    Volume(const Volume &) = delete;
    Volume &operator=(const Volume &) = delete;

    Volume(Volume &&other) noexcept : handle_(other.release()) {}
    Volume &operator=(Volume &&other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.release();
        }
        return *this;
    }

    /* Opens a host path. Returns NOT_FOUND unless FSLITE_OPEN_CREATE is set. */
    Status open(const char *filename, uint32_t flags = 0) noexcept {
        reset();
        fslite_volume *handle = nullptr;
        const fslite_status status = fslite_open(filename, flags, &handle);
        handle_ = handle;
        return Status(status);
    }

    void reset() noexcept {
        if (handle_) {
            fslite_close(handle_);
            handle_ = nullptr;
        }
    }

    fslite_volume *release() noexcept {
        fslite_volume *handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    fslite_volume *get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

    Status begin() noexcept { return Status(fslite_begin(handle_)); }
    Status commit() noexcept { return Status(fslite_commit(handle_)); }
    Status rollback() noexcept { return Status(fslite_rollback(handle_)); }
    bool in_transaction() const noexcept { return fslite_in_transaction(handle_) != 0; }

    Status mkdir(const char *path) noexcept { return Status(fslite_mkdir(handle_, path)); }
    Status rename(const char *source, const char *destination) noexcept {
        return Status(fslite_rename(handle_, source, destination));
    }
    Status remove(const char *path) noexcept { return Status(fslite_remove(handle_, path)); }
    Status remove_recursive(const char *path) noexcept {
        return Status(fslite_remove_recursive(handle_, path));
    }

    Status write(const char *path, const void *data, size_t size) noexcept {
        return Status(fslite_write(handle_, path, data, size));
    }
    Status write_at(const char *path, uint64_t offset, const void *data, size_t size) noexcept {
        return Status(fslite_write_at(handle_, path, offset, data, size));
    }
    Status truncate(const char *path, uint64_t size) noexcept {
        return Status(fslite_truncate(handle_, path, size));
    }

    Status read(const char *path, uint64_t offset, void *buffer, size_t capacity,
                size_t &read_size) noexcept {
        size_t count = 0;
        const fslite_status status =
            fslite_read(handle_, path, offset, buffer, capacity, &count);
        read_size = count;
        return Status(status);
    }

    Status stat(const char *path, fslite_info &info) noexcept {
        return Status(fslite_stat(handle_, path, &info));
    }
    Status inspect(const char *path, fslite_file_info &info) noexcept {
        return Status(fslite_inspect(handle_, path, &info));
    }
    Status set_limits(const fslite_limits *limits) noexcept {
        return Status(fslite_set_limits(handle_, limits));
    }
    Status volume_stats(fslite_volume_stats &stats) noexcept {
        return Status(fslite_get_volume_stats(handle_, &stats));
    }

    Status list(const char *path, fslite_list_callback callback, void *context) noexcept {
        return Status(fslite_list(handle_, path, callback, context));
    }

    Status check() noexcept { return Status(fslite_check(handle_)); }
    Status export_to(const char *destination) noexcept {
        return Status(fslite_export(handle_, destination));
    }

    /* Migrates a volume file in place. Static because it opens its own handle. */
    static Status migrate(const char *filename) noexcept {
        return Status(fslite_migrate(filename));
    }

private:
    fslite_volume *handle_;
};

class Writer {
public:
    Writer() noexcept : handle_(nullptr) {}
    ~Writer() { reset(); }

    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;

    Writer(Writer &&other) noexcept : handle_(other.release()) {}
    Writer &operator=(Writer &&other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.release();
        }
        return *this;
    }

    Status open(Volume &volume, const char *path) noexcept {
        reset();
        fslite_writer *handle = nullptr;
        const fslite_status status = fslite_writer_open(volume.get(), path, &handle);
        handle_ = handle;
        return Status(status);
    }

    Status write(const void *data, size_t size) noexcept {
        return Status(fslite_writer_write(handle_, data, size));
    }
    Status finish() noexcept { return Status(fslite_writer_finish(handle_)); }

    void close() noexcept { reset(); }

    void reset() noexcept {
        if (handle_) {
            fslite_writer_close(handle_);
            handle_ = nullptr;
        }
    }

    fslite_writer *release() noexcept {
        fslite_writer *handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    fslite_writer *get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    fslite_writer *handle_;
};

class Backup {
public:
    Backup() noexcept : handle_(nullptr) {}
    ~Backup() { reset(); }

    Backup(const Backup &) = delete;
    Backup &operator=(const Backup &) = delete;

    Backup(Backup &&other) noexcept : handle_(other.release()) {}
    Backup &operator=(Backup &&other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.release();
        }
        return *this;
    }

    Status open(Volume &volume, const char *destination) noexcept {
        reset();
        fslite_backup *handle = nullptr;
        const fslite_status status = fslite_backup_open(volume.get(), destination, &handle);
        handle_ = handle;
        return Status(status);
    }

    Status step(uint32_t pages, fslite_backup_progress &progress) noexcept {
        return Status(fslite_backup_step(handle_, pages, &progress));
    }
    Status finish() noexcept { return Status(fslite_backup_finish(handle_)); }

    void close() noexcept { reset(); }

    void reset() noexcept {
        if (handle_) {
            fslite_backup_close(handle_);
            handle_ = nullptr;
        }
    }

    fslite_backup *release() noexcept {
        fslite_backup *handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    fslite_backup *get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    fslite_backup *handle_;
};

} // namespace fslite

#endif
