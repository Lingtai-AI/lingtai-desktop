#pragma once
// Internal POSIX descriptor-ownership and no-follow primitives shared by the
// Desktop's descriptor-anchored readers.
//
// This seam is deliberately internal and deliberately small. It owns exactly
// the mechanics that every safe reader must not re-derive: descriptor and
// directory-stream ownership, the required read flags, and strict
// immediate-leaf validation. It owns no domain policy: candidate selection,
// error mapping, size bounds, observation order, and parsing all stay with
// each reader.
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

// A safe immediate leaf: one nonempty, non-dot, relative, single-component
// name without an embedded NUL.
bool safe_leaf(const std::filesystem::path &path);

// Opens an already-existing, real directory by absolute path: the sole
// anchor of a per-operation descriptor-relative walk. It is the only open in
// this seam that receives a full path rather than one relative component.
FileDescriptor open_root_directory(const std::filesystem::path &absolute_root);

// Opens one immediate, safe-leaf, non-symlink directory relative to an
// already open directory descriptor, refusing an unsafe leaf or a
// non-directory at that exact name. No component beyond this single leaf is
// ever traversed. When `create` is set, the directory is created first with
// `mkdirat` if it is absent; a pre-existing non-directory or symlink there is
// still refused rather than replaced.
FileDescriptor open_directory_component(
    int parent_fd, const std::filesystem::path &leaf, bool create = false);

// Opens one immediate, safe-leaf, non-symlink regular file relative to an
// already open directory descriptor, read-only and no-follow, refusing an
// unsafe leaf or a non-regular target at that exact name.
FileDescriptor open_regular_file_component(
    int parent_fd, const std::filesystem::path &leaf);

} // namespace lingtai::desktop::posix_internal
