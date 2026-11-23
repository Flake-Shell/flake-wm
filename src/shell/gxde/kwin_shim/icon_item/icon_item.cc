/* Copyright (C) 2026 CharOfString <root@charofstring.cc>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 * ----------------------------------------------------------------------------
 * A QML item that paints a window icon, standing in for KWin's icon item.
 */

#include "src/shell/gxde/kwin_shim/icon_item/icon_item.h"

#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>

namespace Occultation {

IconItem::IconItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
  setAntialiasing(true);
  setMipmap(true);
}

QVariant IconItem::icon() const { return m_icon; }

void IconItem::setIcon(const QVariant& icon) {
  if (m_icon == icon) {
    return;
  }
  m_icon = icon;
  update();
  Q_EMIT iconChanged();
}

void IconItem::paint(QPainter* painter) {
  QIcon icon;
  if (m_icon.canConvert<QIcon>()) {
    icon = qvariant_cast<QIcon>(m_icon);
  } else if (m_icon.canConvert<QPixmap>()) {
    icon = QIcon(qvariant_cast<QPixmap>(m_icon));
  } else if (m_icon.canConvert<QImage>()) {
    icon = QIcon(QPixmap::fromImage(qvariant_cast<QImage>(m_icon)));
  } else {
    const QString name = m_icon.toString();
    icon = QFileInfo::exists(name) ? QIcon(name) : QIcon::fromTheme(name);
  }

  if (icon.isNull()) {
    icon = QIcon::fromTheme(QStringLiteral("application-x-executable"));
  }

  painter->setRenderHint(QPainter::SmoothPixmapTransform, smooth());
  const QSize available = boundingRect().size().toSize();
  const QSize actual = icon.actualSize(available);
  QRect target(QPoint(), actual);
  target.moveCenter(boundingRect().center().toPoint());
  icon.paint(painter, target);
}

}  // namespace Occultation
