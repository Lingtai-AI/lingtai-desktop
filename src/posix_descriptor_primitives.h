#pragma once
// Internal POSIX descriptor-ownership and no-follow primitives shared by the
// Desktop's descriptor-anchored readers.
//
// This seam is deliberately internal and deliberately small. It owns exactly
// the mechanics that every safe reader must not re-derive: descriptor and
// directory-stream ownership, the required read flags, device+inode identity,
// platform mtime conversion, and strict immediate-leaf validation. It owns no
// domain policy: candidate selection, error mapping, size bounds, observation
// order, race verdicts, and parsing all stay with each reader.
//
// Every definition lives in the separate implementation unit rather than
// inline here, so the contract is one substitutable link-time unit.
#include <dirent.h>
#include <filesystem>
#include <sys/stat.h>

namespace lingtai::desktop::posix_internal {

// Move-only descriptor ownership: exactly one owner closes exactly once, and a
// moved-from owner holds nothing.
class FileDescriptor final {
public:
    explicit FileDescriptor(int value = -1);
    ~FileDescriptor();
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    FileDescriptor(FileDescriptor &&other) noexcept;
    FileDescriptor &operator=(FileDescriptor &&other) noexcept;
    void reset(int value = -1);
    [[nodiscard]] int get() const;

private:
    int value_;
};

// Ownership of a directory stream that has already adopted a descriptor.
// Closing the stream closes exactly that adopted descriptor.
class DirectoryStream final {
public:
    explicit DirectoryStream(DIR *value);
    ~DirectoryStream();
    DirectoryStream(const DirectoryStream &) = delete;
    DirectoryStream &operator=(const DirectoryStream &) = delete;
    [[nodiscard]] DIR *get() const;

private:
    DIR *value_;
};

// The required flags for every descriptor-anchored source read: read-only,
// close-on-exec, no-follow, and nonblocking.
int read_flags();

struct FileIdentity {
    dev_t device = 0;
    ino_t inode = 0;
};

// File identity is device plus inode, never a path or metadata appearance.
bool same_file(const struct stat &value, const FileIdentity &identity);

// Platform-correct modification time, preserving the nanosecond fraction.
double modified_at_seconds(const struct stat &value);

// A safe immediate leaf: one nonempty, non-dot, relative, single-component
// name without an embedded NUL.
bool safe_leaf(const std::filesystem::path &path);

} // namespace lingtai::desktop::posix_internal
