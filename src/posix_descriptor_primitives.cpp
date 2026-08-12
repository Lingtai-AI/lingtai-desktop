#include "posix_descriptor_primitives.h"

#include <fcntl.h>
#include <string>
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

bool same_file(const struct stat &value, const FileIdentity &identity) {
    return value.st_dev == identity.device && value.st_ino == identity.inode;
}

double modified_at_seconds(const struct stat &value) {
#if defined(__APPLE__)
    return static_cast<double>(value.st_mtimespec.tv_sec)
        + static_cast<double>(value.st_mtimespec.tv_nsec) / 1'000'000'000.0;
#else
    return static_cast<double>(value.st_mtim.tv_sec)
        + static_cast<double>(value.st_mtim.tv_nsec) / 1'000'000'000.0;
#endif
}

bool safe_leaf(const std::filesystem::path &path) {
    return !path.empty() && !path.is_absolute() && !path.has_root_name()
        && !path.has_root_directory() && path == path.filename()
        && path != "." && path != ".."
        && path.native().find('\0') == std::string::npos;
}

} // namespace lingtai::desktop::posix_internal
