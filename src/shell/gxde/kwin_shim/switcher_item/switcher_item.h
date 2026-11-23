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
 * The switcher object the window switcher QML binds to, standing in for
 * KWin's TabBox switcher.
 */

#ifndef SRC_SHELL_GXDE_KWIN_SHIM_SWITCHER_ITEM_SWITCHER_ITEM_H_
#define SRC_SHELL_GXDE_KWIN_SHIM_SWITCHER_ITEM_SWITCHER_ITEM_H_

#include <QAbstractItemModel>
#include <QObject>
#include <QRect>

namespace Occultation {

class SwitcherItem : public QObject {
  Q_OBJECT
  Q_PROPERTY(QAbstractItemModel* model READ model NOTIFY modelChanged)
  Q_PROPERTY(
      QRect screenGeometry READ screenGeometry NOTIFY screenGeometryChanged)
  Q_PROPERTY(bool visible READ isVisible NOTIFY visibleChanged)
  Q_PROPERTY(bool allDesktops READ isAllDesktops NOTIFY allDesktopsChanged)
  Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY
                 currentIndexChanged)
  Q_PROPERTY(
      bool noModifierGrab READ noModifierGrab NOTIFY noModifierGrabChanged)
  Q_PROPERTY(bool compositing READ compositing NOTIFY compositingChanged)
  Q_PROPERTY(QObject* item READ item WRITE setItem NOTIFY itemChanged)
  Q_CLASSINFO("DefaultProperty", "item")

 public:
  explicit SwitcherItem(QObject* parent = nullptr);

  QAbstractItemModel* model() const;
  QRect screenGeometry() const;
  bool isVisible() const;
  bool isAllDesktops() const;
  int currentIndex() const;
  void setCurrentIndex(int index);
  bool noModifierGrab() const;
  bool compositing() const;
  QObject* item() const;
  void setItem(QObject* item);

 signals:
  void visibleChanged();
  void currentIndexChanged(int index);
  void modelChanged();
  void allDesktopsChanged();
  void screenGeometryChanged();
  void itemChanged();
  void noModifierGrabChanged();
  void compositingChanged();

 private:
  QObject* m_item = nullptr;
};

}  // namespace Occultation

#endif  // SRC_SHELL_GXDE_KWIN_SHIM_SWITCHER_ITEM_SWITCHER_ITEM_H_
