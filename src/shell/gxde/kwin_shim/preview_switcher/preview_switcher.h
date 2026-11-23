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
 * The fake switcher backend used by the standalone switcher preview.
 */

#ifndef SRC_SHELL_GXDE_KWIN_SHIM_PREVIEW_SWITCHER_PREVIEW_SWITCHER_H_
#define SRC_SHELL_GXDE_KWIN_SHIM_PREVIEW_SWITCHER_PREVIEW_SWITCHER_H_

#include <QAbstractItemModel>
#include <QObject>
#include <QRect>

#include "src/shell/gxde/kwin_shim/preview_model/preview_model.h"

namespace Occultation {

class PreviewSwitcher : public QObject {
  Q_OBJECT
  Q_PROPERTY(QAbstractItemModel* model READ model CONSTANT)
  Q_PROPERTY(QRect screenGeometry READ screenGeometry CONSTANT)
  Q_PROPERTY(bool visible READ isVisible CONSTANT)
  Q_PROPERTY(bool allDesktops READ allDesktops CONSTANT)
  Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY
                 currentIndexChanged)
  Q_PROPERTY(bool noModifierGrab READ noModifierGrab CONSTANT)
  Q_PROPERTY(bool compositing READ compositing CONSTANT)
  Q_PROPERTY(QObject* item READ item WRITE setItem NOTIFY itemChanged)
  Q_CLASSINFO("DefaultProperty", "item")

 public:
  explicit PreviewSwitcher(QObject* parent = nullptr);

  QAbstractItemModel* model();
  QRect screenGeometry() const;
  bool isVisible() const;
  bool allDesktops() const;
  int currentIndex() const;
  void setCurrentIndex(int index);
  bool noModifierGrab() const;
  bool compositing() const;
  QObject* item() const;
  void setItem(QObject* item);

 signals:
  void currentIndexChanged(int index);
  void itemChanged();

 private:
  PreviewModel m_model;
  QObject* m_item = nullptr;
  int m_currentIndex = 1;
};

}  // namespace Occultation

#endif  // SRC_SHELL_GXDE_KWIN_SHIM_PREVIEW_SWITCHER_PREVIEW_SWITCHER_H_
