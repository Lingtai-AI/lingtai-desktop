#ifdef QT_CORE_LIB
#error "attachment selection must stay Qt-independent"
#endif

#include "attachment_selection.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using lingtai::desktop::AttachmentMediaKind;
using lingtai::desktop::AttachmentRejectionReason;
using lingtai::desktop::AttachmentSelectionResult;
using lingtai::desktop::kAttachmentPerFileLimitBytes;
using lingtai::desktop::kAttachmentTotalLimitBytes;
using lingtai::desktop::preflight_attachments;

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void write_file(const fs::path &path, const std::string &contents) {
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
}

void make_sparse_file(const fs::path &path, std::uint64_t size) {
    write_file(path, "");
    std::error_code error;
    fs::resize_file(path, size, error);
    expect(!error, "sparse fixture is sized");
}

struct TreeEntry {
    fs::path relative_path;
    fs::file_type type = fs::file_type::none;
    std::uintmax_t size = 0;

    bool operator==(const TreeEntry &) const = default;
};

std::vector<TreeEntry> tree_snapshot(const fs::path &root) {
    std::vector<TreeEntry> entries;
    std::error_code error;
    for (fs::recursive_directory_iterator iterator(root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto status = iterator->symlink_status(error);
        if (error) break;
        entries.push_back({
            fs::relative(iterator->path(), root),
            status.type(),
            fs::is_regular_file(status) ? iterator->file_size(error) : 0,
        });
        if (error) break;
    }
    expect(!error, "fixture tree can be snapshotted");
    return entries;
}

void verify_metadata_order_and_classification(const fs::path &sandbox) {
    const auto first = sandbox / "first.PnG";
    const auto second = sandbox / "archive.unknown";
    write_file(first, "image-bytes");
    write_file(second, "data");

    const auto result = preflight_attachments({first, second});
    expect(result.accepted.size() == 2, "two regular files are accepted");
    if (result.accepted.size() == 2) {
        expect(result.accepted[0].source_path == fs::canonical(first),
            "accepted source is canonical and stable");
        expect(result.accepted[0].display_filename == "first.PnG",
            "display filename preserves the selected leaf name");
        expect(result.accepted[0].byte_size == 11,
            "exact first-file size is retained");
        struct stat opened {};
        expect(::stat(first.c_str(), &opened) == 0,
            "accepted fixture identity can be inspected");
        expect(result.accepted[0].device_id
                    == static_cast<std::uint64_t>(opened.st_dev)
                && result.accepted[0].inode_id
                    == static_cast<std::uint64_t>(opened.st_ino),
            "opened source device and inode identity are retained");
        expect(result.accepted[0].media_kind == AttachmentMediaKind::image,
            "image extension matching is case-insensitive");
        expect(result.accepted[1].display_filename == "archive.unknown",
            "accepted input order is preserved");
        expect(result.accepted[1].media_kind == AttachmentMediaKind::file,
            "unknown extensions remain ordinary files");
    }
    expect(result.accepted_bytes == 15, "accepted bytes are accounted exactly");
    expect(result.rejected.empty(), "valid files have no rejection details");
}

void verify_duplicates_and_local_failures(const fs::path &sandbox) {
    const auto source = sandbox / "duplicate.txt";
    const auto alias = sandbox / "duplicate-alias.txt";
    const auto directory = sandbox / "directory";
    write_file(source, "duplicate");
    std::error_code error;
    fs::create_symlink(source, alias, error);
    expect(!error, "duplicate symlink fixture is created");
    fs::create_directory(directory, error);
    expect(!error, "directory fixture is created");

    const auto missing = sandbox / "missing.txt";
    const auto result = preflight_attachments({source, missing, alias, directory});
    expect(result.accepted.size() == 1, "equivalent sources are accepted once");
    expect(result.rejected.size() == 3, "three typed local rejections are returned");
    if (result.rejected.size() == 3) {
        expect(result.rejected[0].input_path == missing
                && result.rejected[0].reason == AttachmentRejectionReason::missing,
            "missing input is identified in rejection order");
        expect(result.rejected[1].input_path == alias
                && result.rejected[1].reason == AttachmentRejectionReason::duplicate,
            "canonical duplicate is identified without adding bytes");
        expect(result.rejected[2].input_path == directory
                && result.rejected[2].reason == AttachmentRejectionReason::not_regular,
            "a directory is rejected as not regular");
    }
    expect(result.accepted_bytes == 9, "duplicate rejection consumes no budget");

#if !defined(_WIN32)
    const auto fifo = sandbox / "pipe";
    expect(::mkfifo(fifo.c_str(), 0600) == 0, "FIFO fixture is created");
    const auto fifo_result = preflight_attachments({fifo});
    expect(fifo_result.rejected.size() == 1
            && fifo_result.rejected[0].reason
                == AttachmentRejectionReason::not_regular,
        "a FIFO is rejected without blocking");

    const auto unreadable = sandbox / "unreadable.txt";
    write_file(unreadable, "locked");
    fs::permissions(unreadable, fs::perms::none, fs::perm_options::replace, error);
    if (error || geteuid() == 0) {
        std::cout << "SKIP: unreadable fixture is ineffective\n";
    } else {
        const auto unreadable_result = preflight_attachments({unreadable});
        expect(unreadable_result.rejected.size() == 1
                && unreadable_result.rejected[0].reason
                    == AttachmentRejectionReason::unreadable,
            "an unopenable regular file is rejected as unreadable");
    }
    fs::permissions(unreadable, fs::perms::owner_all,
        fs::perm_options::replace, error);
    expect(!error, "unreadable fixture permissions are restored");
#endif
}

void verify_limits_and_no_mutation(const fs::path &sandbox) {
    const auto exact = sandbox / "exact.bin";
    const auto over = sandbox / "over.bin";
    make_sparse_file(exact, kAttachmentPerFileLimitBytes);
    make_sparse_file(over, kAttachmentPerFileLimitBytes + 1);
    const auto boundary = preflight_attachments({exact, over});
    expect(boundary.accepted.size() == 1
            && boundary.accepted[0].byte_size == kAttachmentPerFileLimitBytes,
        "the per-file limit is inclusive");
    expect(boundary.rejected.size() == 1
            && boundary.rejected[0].reason
                == AttachmentRejectionReason::per_file_limit,
        "one byte over the per-file limit is rejected");

    const auto a = sandbox / "a.bin";
    const auto b = sandbox / "b.bin";
    const auto c = sandbox / "c.bin";
    const auto d = sandbox / "d.bin";
    const auto too_large_for_remainder = sandbox / "remainder-over.bin";
    const auto later_small = sandbox / "later-small.bin";
    make_sparse_file(a, kAttachmentPerFileLimitBytes);
    make_sparse_file(b, kAttachmentPerFileLimitBytes);
    make_sparse_file(c, kAttachmentPerFileLimitBytes);
    make_sparse_file(d, kAttachmentPerFileLimitBytes - 1024);
    make_sparse_file(too_large_for_remainder, 2048);
    make_sparse_file(later_small, 1024);

    const auto before = tree_snapshot(sandbox);
    const auto total = preflight_attachments(
        {a, b, c, d, too_large_for_remainder, over, later_small});
    const auto after = tree_snapshot(sandbox);
    expect(before == after, "preflight does not mutate the selected filesystem");
    expect(total.accepted.size() == 5, "later smaller input remains eligible");
    expect(total.accepted_bytes == kAttachmentTotalLimitBytes,
        "cumulative accounting reaches the exact total boundary");
    expect(total.rejected.size() == 2, "oversized and over-total inputs reject");
    if (total.rejected.size() == 2) {
        expect(total.rejected[0].input_path == too_large_for_remainder
                && total.rejected[0].reason
                    == AttachmentRejectionReason::total_limit,
            "an over-total file does not consume the remaining budget");
        expect(total.rejected[1].input_path == over
                && total.rejected[1].reason
                    == AttachmentRejectionReason::per_file_limit,
            "an oversized file stays typed and does not block a later file");
    }
    if (!total.accepted.empty()) {
        expect(total.accepted.back().source_path == fs::canonical(later_small),
            "accepted order includes the later smaller file last");
    }
}

} // namespace

int main(int argc, char **argv) {
    static_assert(noexcept(preflight_attachments({})));
    static_assert(std::is_same_v<decltype(preflight_attachments({})),
        AttachmentSelectionResult>);
    static_assert(kAttachmentPerFileLimitBytes == 25ULL * 1024ULL * 1024ULL);
    static_assert(kAttachmentTotalLimitBytes == 100ULL * 1024ULL * 1024ULL);

    if (argc != 2) {
        std::cerr << "expected one temporary-directory argument\n";
        return 2;
    }
    const auto sandbox = fs::path(argv[1]);
    std::error_code error;
    fs::remove_all(sandbox, error);
    fs::create_directories(sandbox, error);
    expect(!error, "test sandbox is created");

    verify_metadata_order_and_classification(sandbox);
    verify_duplicates_and_local_failures(sandbox);
    verify_limits_and_no_mutation(sandbox);

    fs::remove_all(sandbox, error);
    expect(!error, "test sandbox is removed");
    if (failures != 0) {
        std::cerr << failures << " attachment selection assertion(s) failed\n";
        return 1;
    }
    std::cout << "ATTACHMENT_SELECTION_CONTRACT_OK\n";
    return 0;
}
