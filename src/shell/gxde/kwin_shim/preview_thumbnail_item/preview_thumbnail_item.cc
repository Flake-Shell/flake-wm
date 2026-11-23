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
 * A placeholder window thumbnail used by the standalone switcher preview.
 */

#include "src/shell/gxde/kwin_shim/preview_thumbnail_item/preview_thumbnail_item.h"

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QRectF>

namespace Occultation {

PreviewThumbnailItem::PreviewThumbnailItem(QQuickItem* parent)
    : QQuickPaintedItem(parent) {
  setAntialiasing(true);
}

QUuid PreviewThumbnailItem::wId() const { return m_windowId; }

void PreviewThumbnailItem::setWId(const QUuid& windowId) {
  if (m_windowId == windowId) {
    return;
  }
  m_windowId = windowId;
  update();
  Q_EMIT wIdChanged();
}

void PreviewThumbnailItem::paint(QPainter* painter) {
  painter->setRenderHint(QPainter::Antialiasing);
  const QRectF area = boundingRect();
  QLinearGradient background(area.topLeft(), area.bottomRight());
  background.setColorAt(0, QColor(QStringLiteral("#175d8f")));
  background.setColorAt(1, QColor(QStringLiteral("#5b2d91")));
  painter->setPen(Qt::NoPen);
  painter->setBrush(background);
  painter->drawRoundedRect(area.adjusted(2, 2, -2, -2), 7, 7);

  painter->setBrush(QColor(255, 255, 255, 220));
  painter->drawRoundedRect(
      area.adjusted(area.width() * .12, area.height() * .18,
                    -area.width() * .42, -area.height() * .32),
      3, 3);
  painter->setBrush(QColor(23, 36, 54, 215));
  painter->drawRoundedRect(
      area.adjusted(area.width() * .48, area.height() * .30,
                    -area.width() * .10, -area.height() * .18),
      3, 3);
}

}  // namespace Occultation
