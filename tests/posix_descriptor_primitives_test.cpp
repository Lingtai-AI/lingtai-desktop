#ifdef QT_CORE_LIB
#error "Qt Core usage requirements must not propagate to this internal seam"
#endif
#include "posix_descriptor_primitives.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;
using namespace lingtai::desktop::posix_internal;

namespace {
int failures = 0;
void expect(bool condition, const std::string &message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n', ++failures;
}

// A closed descriptor number is observable without touching the file itself.
bool closed(int value) {
    errno = 0;
    return ::fcntl(value, F_GETFD) == -1 && errno == EBADF;
}
bool open_still(int value) { return ::fcntl(value, F_GETFD) != -1; }

void write_file(const fs::path &path, std::string_view contents) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    expect(!error, "fixture parent is created: " + path.string());
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
    expect(stream.good(), "fixture file is written: " + path.string());
}

// Exactly-once ownership. The recycled-descriptor probe is the real
// double-close detector: POSIX hands back the lowest free number, so a
// moved-from owner that still closed would silently take the new descriptor.
void check_descriptor_ownership(const fs::path &root) {
    const auto file = root / "owned";
    write_file(file, "owned");

    const FileDescriptor empty;
    expect(empty.get() == -1, "a default-constructed owner holds no descriptor");

    const int raw = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    expect(raw >= 0, "fixture descriptor opens");
    int recycled = -1;
    {
        FileDescriptor source(raw);
        expect(source.get() == raw, "construction adopts the descriptor");
        {
            const FileDescriptor sink(std::move(source));
            expect(sink.get() == raw, "move construction preserves the descriptor");
            expect(source.get() == -1, "a moved-from owner releases ownership");
            expect(open_still(raw), "moving does not close the descriptor");
        }
        expect(closed(raw), "the new owner closes the descriptor once it dies");
        recycled = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
        expect(recycled == raw,
            "the released number is recycled, so the double-close probe is real");
    }
    expect(open_still(recycled),
        "a moved-from owner never closes the recycled descriptor");
    if (recycled >= 0) ::close(recycled);

    const int replaced = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    const int adopted = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    expect(replaced >= 0 && adopted >= 0 && replaced != adopted,
        "two distinct fixture descriptors open");
    {
        FileDescriptor holder(replaced);
        holder.reset(adopted);
        expect(closed(replaced), "reset closes the previously owned descriptor");
        expect(holder.get() == adopted, "reset adopts the replacement");
        holder.reset();
        expect(closed(adopted), "an argument-free reset closes and releases");
        expect(holder.get() == -1, "an argument-free reset holds nothing");
    }

    const int first = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    const int second = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    expect(first >= 0 && second >= 0 && first != second,
        "two distinct assignment descriptors open");
    {
        FileDescriptor holder(first);
        FileDescriptor other(second);
        holder = std::move(other);
        expect(holder.get() == second, "move assignment adopts the source");
        expect(other.get() == -1, "move assignment empties the source");
        expect(closed(first), "move assignment closes the previously owned one");
        expect(open_still(second), "move assignment keeps the adopted one open");
    }
    expect(closed(second), "the surviving owner closes at scope exit");
}

// Real flag probes, not a restatement of the constant. Every open below is
// guarded so that a missing O_NONBLOCK fails an assertion instead of hanging.
void check_read_flags(const fs::path &root) {
    const auto flags = read_flags();
    expect((flags & O_ACCMODE) == O_RDONLY, "read flags are read-only");
    expect((flags & O_CLOEXEC) != 0, "read flags request close-on-exec");
    expect((flags & O_NOFOLLOW) != 0, "read flags request no-follow");
    expect((flags & O_NONBLOCK) != 0, "read flags request nonblocking access");

    const auto target = root / "target";
    write_file(target, "target");
    const FileDescriptor regular(::open(target.c_str(), read_flags()));
    expect(regular.get() >= 0, "a regular file opens with the shared read flags");
    expect((::fcntl(regular.get(), F_GETFD) & FD_CLOEXEC) != 0,
        "the opened descriptor really carries close-on-exec");
    const auto description = ::fcntl(regular.get(), F_GETFL);
    expect((description & O_NONBLOCK) != 0,
        "the opened description really carries nonblocking access");
    expect((description & O_ACCMODE) == O_RDONLY,
        "the opened description really is read-only");

    std::error_code error;
    const auto link = root / "link";
    fs::create_symlink(target.filename(), link, error);
    expect(!error, "fixture symlink is created");
    errno = 0;
    const FileDescriptor followed(::open(link.c_str(), read_flags()));
    expect(followed.get() < 0 && errno == ELOOP,
        "the shared read flags refuse to follow a symlink");

    // A FIFO is the portable nonblocking probe. The keeper holds both ends
    // open first, so no open below can block even for a mutant.
    const auto pipe = root / "pipe";
    expect(::mkfifo(pipe.c_str(), 0600) == 0, "fixture FIFO is created");
    const FileDescriptor keeper(
        ::open(pipe.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
    expect(keeper.get() >= 0, "fixture FIFO keeper opens");
    const FileDescriptor special(::open(pipe.c_str(), read_flags()));
    expect(special.get() >= 0, "a FIFO opens with the shared read flags");
    const auto nonblocking =
        (::fcntl(special.get(), F_GETFL) & O_NONBLOCK) != 0;
    expect(nonblocking, "the FIFO description really is nonblocking");
    const char payload = 'p';
    expect(::write(keeper.get(), &payload, 1) == 1, "the FIFO accepts one byte");
    char received = 0;
    expect(::read(special.get(), &received, 1) == 1 && received == payload,
        "a nonblocking FIFO read still returns available bytes");
    if (nonblocking) {
        char spare = 0;
        errno = 0;
        expect(::read(special.get(), &spare, 1) == -1 && errno == EAGAIN,
            "an empty nonblocking FIFO reports EAGAIN instead of blocking");
    } else {
        expect(false,
            "an empty FIFO read was skipped because it would have blocked");
    }
}

// The adopted descriptor is the stream's, and only the stream's.
void check_directory_stream_ownership(const fs::path &root) {
    write_file(root / "listed" / "entry", "entry");
    const FileDescriptor directory(
        ::open((root / "listed").c_str(), read_flags() | O_DIRECTORY));
    expect(directory.get() >= 0, "the fixture directory opens");
    const int adopted = ::dup(directory.get());
    expect(adopted >= 0, "a duplicate descriptor is available for adoption");
    std::set<std::string> names;
    {
        const DirectoryStream stream(::fdopendir(adopted));
        expect(stream.get() != nullptr, "the duplicate is adopted by a stream");
        while (const auto entry = ::readdir(stream.get())) {
            names.insert(entry->d_name);
        }
    }
    expect(names.contains("entry"), "the adopted stream enumerated the entries");
    expect(closed(adopted), "the stream closes exactly the adopted descriptor");
    expect(open_still(directory.get()),
        "the stream leaves the separately owned descriptor open");

    const int recycled = ::open((root / "listed").c_str(), read_flags());
    expect(recycled == adopted,
        "the adopted number is recycled, so a second close would be visible");
    expect(open_still(recycled),
        "the destroyed stream did not close the recycled descriptor");
    if (recycled >= 0) ::close(recycled);
}

void check_safe_leaf() {
    expect(safe_leaf("alpha"), "an ordinary immediate leaf is safe");
    expect(safe_leaf(".hidden"), "a dot-prefixed immediate leaf is safe");
    expect(!safe_leaf(fs::path()), "an empty name is not a safe leaf");
    expect(!safe_leaf("."), "the current directory is not a safe leaf");
    expect(!safe_leaf(".."), "the parent directory is not a safe leaf");
    expect(!safe_leaf("/alpha"), "an absolute path is not a safe leaf");
    expect(!safe_leaf("alpha/beta"), "a multi-component path is not a safe leaf");
    expect(!safe_leaf("alpha/"), "a trailing separator is not a safe leaf");
    expect(!safe_leaf("../alpha"), "an escaping path is not a safe leaf");
    expect(!safe_leaf(fs::path(std::string("alpha\0beta", 10))),
        "an embedded NUL is not a safe leaf");
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <fixture-root>\n";
        return 2;
    }
    const fs::path root = argv[1];
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);
    if (error) {
        std::cerr << "fixture root is unavailable: " << error.message() << '\n';
        return 2;
    }

    check_descriptor_ownership(root / "ownership");
    check_read_flags(root / "flags");
    check_directory_stream_ownership(root / "stream");
    check_safe_leaf();

    if (failures != 0) {
        std::cerr << failures << " descriptor primitive contract failures\n";
        return 1;
    }
    std::cout << "descriptor primitive contract holds\n";
    return 0;
}
