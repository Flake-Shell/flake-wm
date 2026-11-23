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

#ifndef SRC_SHELL_GXDE_KWIN_SHIM_PREVIEW_THUMBNAIL_ITEM_PREVIEW_THUMBNAIL_ITEM_H_
#define SRC_SHELL_GXDE_KWIN_SHIM_PREVIEW_THUMBNAIL_ITEM_PREVIEW_THUMBNAIL_ITEM_H_

#include <QQuickPaintedItem>
#include <QUuid>

namespace Occultation {

class PreviewThumbnailItem : public QQuickPaintedItem {
  Q_OBJECT
  Q_PROPERTY(QUuid wId READ wId WRITE setWId NOTIFY wIdChanged)

 public:
  explicit PreviewThumbnailItem(QQuickItem* parent = nullptr);

  QUuid wId() const;
  void setWId(const QUuid& windowId);
  void paint(QPainter* painter) override;

 signals:
  void wIdChanged();

 private:
  QUuid m_windowId;
};

}  // namespace Occultation

#endif  // SRC_SHELL_GXDE_KWIN_SHIM_PREVIEW_THUMBNAIL_ITEM_PREVIEW_THUMBNAIL_ITEM_H_
