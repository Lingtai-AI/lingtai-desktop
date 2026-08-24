#pragma once

#include "attachment_selection.h"

#include <QtCore/QSize>
#include <QtGui/QPixmap>

namespace lingtai::desktop {

// Reopens no-follow and revalidates the accepted descriptor identity/size,
// then reads a bounded preview through that descriptor. Changed sources and
// invalid or implausibly large images fail closed to the ordinary file card.
[[nodiscard]] QPixmap load_attachment_thumbnail(
    const AcceptedAttachment &attachment,
    QSize bounds = QSize(72, 56));

} // namespace lingtai::desktop
