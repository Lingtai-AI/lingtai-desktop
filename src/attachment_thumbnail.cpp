#include "attachment_thumbnail.h"

#include <QtCore/QFile>
#include <QtGui/QImageReader>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lingtai::desktop {

QPixmap load_attachment_thumbnail(
        const AcceptedAttachment &attachment,
        QSize bounds) {
    if (attachment.media_kind != AttachmentMediaKind::image
        || !bounds.isValid() || bounds.isEmpty()
        || bounds.width() > 256 || bounds.height() > 256
        || !attachment.source_path.is_absolute()) {
        return {};
    }

    const auto descriptor = ::open(attachment.source_path.c_str(),
        O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return {};
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)
        || status.st_size < 0
        || static_cast<std::uint64_t>(status.st_dev) != attachment.device_id
        || static_cast<std::uint64_t>(status.st_ino) != attachment.inode_id
        || static_cast<std::uint64_t>(status.st_size) != attachment.byte_size) {
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
    // Some Qt image plugins do not honor scaled decode. Cap the full decoded
    // source to at most 16M pixels (~64 MiB at 32 bpp) even in that case;
    // the returned preview remains capped separately at 256x256.
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

} // namespace lingtai::desktop
