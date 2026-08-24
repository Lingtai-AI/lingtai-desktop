#include "attachment_thumbnail.h"

#include <QtCore/QFile>
#include <QtGui/QImageReader>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lingtai::desktop {
namespace {

QPixmap load_thumbnail(
        const std::filesystem::path &path,
        AttachmentMediaKind media_kind,
        std::uint64_t device_id,
        std::uint64_t inode_id,
        std::uint64_t byte_size,
        QSize bounds) {
    if (media_kind != AttachmentMediaKind::image
        || !bounds.isValid() || bounds.isEmpty()
        || bounds.width() > 256 || bounds.height() > 256
        || !path.is_absolute()) {
        return {};
    }

    const auto descriptor = ::open(path.c_str(),
        O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return {};
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)
        || status.st_size < 0
        || static_cast<std::uint64_t>(status.st_dev) != device_id
        || static_cast<std::uint64_t>(status.st_ino) != inode_id
        || static_cast<std::uint64_t>(status.st_size) != byte_size) {
        ::close(descriptor);
        return {};
    }

    QFile source;
    if (!source.open(descriptor, QIODevice::ReadOnly,
            QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        return {};
    }
    QImageReader reader(&source);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    const auto dimensions = reader.size();
    constexpr auto kMaximumImageDimension = 8192;
    constexpr auto kMaximumImagePixels = 16LL * 1024LL * 1024LL;
    if (!dimensions.isValid() || dimensions.isEmpty()
        || dimensions.width() > kMaximumImageDimension
        || dimensions.height() > kMaximumImageDimension
        || static_cast<qint64>(dimensions.width()) * dimensions.height()
            > kMaximumImagePixels) {
        return {};
    }
    reader.setScaledSize(dimensions.scaled(bounds, Qt::KeepAspectRatio));
    const auto image = reader.read();
    if (image.isNull() || image.width() > bounds.width()
        || image.height() > bounds.height()) {
        return {};
    }
    return QPixmap::fromImage(image);
}

} // namespace

QPixmap load_attachment_thumbnail(
        const AcceptedAttachment &attachment,
        QSize bounds) {
    return load_thumbnail(
        attachment.source_path,
        attachment.media_kind,
        attachment.device_id,
        attachment.inode_id,
        attachment.byte_size,
        bounds);
}

QPixmap load_attachment_thumbnail(
        const DirectConversationAttachment &attachment,
        QSize bounds) {
    return load_thumbnail(
        attachment.local_path,
        attachment.media_kind,
        attachment.device_id,
        attachment.inode_id,
        attachment.byte_size,
        bounds);
}

} // namespace lingtai::desktop
