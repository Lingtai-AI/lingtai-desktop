#include "posix_descriptor_primitives.h"

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace lingtai::desktop::posix_internal {

FileDescriptor::FileDescriptor(int value) : value_(value) {}

FileDescriptor::~FileDescriptor() { reset(); }

FileDescriptor::FileDescriptor(FileDescriptor &&other) noexcept
: value_(std::exchange(other.value_, -1)) {}

FileDescriptor &FileDescriptor::operator=(FileDescriptor &&other) noexcept {
    if (this != &other) reset(std::exchange(other.value_, -1));
    return *this;
}

void FileDescriptor::reset(int value) {
    if (value_ >= 0) ::close(value_);
    value_ = value;
}

int FileDescriptor::get() const { return value_; }

DirectoryStream::DirectoryStream(DIR *value) : value_(value) {}

DirectoryStream::~DirectoryStream() { if (value_) ::closedir(value_); }

DIR *DirectoryStream::get() const { return value_; }

int read_flags() {
    auto flags = O_RDONLY;
    flags |= O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    return flags;
}

bool safe_leaf(const std::filesystem::path &path) {
    return !path.empty() && !path.is_absolute() && !path.has_root_name()
        && !path.has_root_directory() && path == path.filename()
        && path != "." && path != ".."
        && path.native().find('\0') == std::string::npos;
}

FileDescriptor open_root_directory(const std::filesystem::path &absolute_root) {
    return FileDescriptor(::open(absolute_root.c_str(), read_flags() | O_DIRECTORY));
}

FileDescriptor open_directory_component(
        int parent_fd, const std::filesystem::path &leaf, bool create) {
    if (!safe_leaf(leaf)) return FileDescriptor();
    if (create && ::mkdirat(parent_fd, leaf.c_str(), 0700) != 0
            && errno != EEXIST) {
        return FileDescriptor();
    }
    // O_DIRECTORY is kernel-enforced: a symlink at this exact leaf fails with
    // ELOOP (O_NOFOLLOW), and any other non-directory fails with ENOTDIR, so
    // no separate post-open type check is needed here.
    return FileDescriptor(
        ::openat(parent_fd, leaf.c_str(), read_flags() | O_DIRECTORY));
}

FileDescriptor open_regular_file_component(
        int parent_fd, const std::filesystem::path &leaf) {
    if (!safe_leaf(leaf)) return FileDescriptor();
    FileDescriptor file(::openat(parent_fd, leaf.c_str(), read_flags()));
    if (file.get() < 0) return file;
    struct stat opened {};
    if (::fstat(file.get(), &opened) != 0 || !S_ISREG(opened.st_mode)) {
        return FileDescriptor();
    }
    return file;
}

} // namespace lingtai::desktop::posix_internal
