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

#ifndef SRC_SHELL_GXDE_KWIN_SHIM_ICON_ITEM_ICON_ITEM_H_
#define SRC_SHELL_GXDE_KWIN_SHIM_ICON_ITEM_ICON_ITEM_H_

#include <QQuickPaintedItem>
#include <QVariant>

namespace Occultation {

class IconItem : public QQuickPaintedItem {
  Q_OBJECT
  Q_PROPERTY(QVariant icon READ icon WRITE setIcon NOTIFY iconChanged)

 public:
  explicit IconItem(QQuickItem* parent = nullptr);

  QVariant icon() const;
  void setIcon(const QVariant& icon);
  void paint(QPainter* painter) override;

 signals:
  void iconChanged();

 private:
  QVariant m_icon;
};

}  // namespace Occultation

#endif  // SRC_SHELL_GXDE_KWIN_SHIM_ICON_ITEM_ICON_ITEM_H_
