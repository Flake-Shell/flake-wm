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

#include "src/shell/gxde/kwin_shim/switcher_item/switcher_item.h"

#include <QAbstractItemModel>

#include "src/shell/gxde/kwin_shim/exoskeleton/exoskeleton.h"
#include "src/shell/gxde/kwin_shim/switcher_model/switcher_model.h"

namespace Occultation {

SwitcherItem::SwitcherItem(QObject* parent) : QObject(parent) {
  Exoskeleton* runtime = Exoskeleton::instance();
  connect(runtime, &Exoskeleton::visibleChanged, this,
          &SwitcherItem::visibleChanged);
  connect(runtime, &Exoskeleton::currentIndexChanged, this,
          &SwitcherItem::currentIndexChanged);
  connect(runtime, &Exoskeleton::allDesktopsChanged, this,
          &SwitcherItem::allDesktopsChanged);
  connect(runtime, &Exoskeleton::screenGeometryChanged, this,
          &SwitcherItem::screenGeometryChanged);
  connect(runtime, &Exoskeleton::noModifierGrabChanged, this,
          &SwitcherItem::noModifierGrabChanged);
}

QAbstractItemModel* SwitcherItem::model() const {
  return Exoskeleton::instance()->model();
}

QRect SwitcherItem::screenGeometry() const {
  return Exoskeleton::instance()->screenGeometry();
}

bool SwitcherItem::isVisible() const {
  return Exoskeleton::instance()->isVisible();
}

bool SwitcherItem::isAllDesktops() const {
  return Exoskeleton::instance()->allDesktops();
}

int SwitcherItem::currentIndex() const {
  return Exoskeleton::instance()->currentIndex();
}

void SwitcherItem::setCurrentIndex(int index) {
  Exoskeleton::instance()->setCurrentIndex(index);
}

bool SwitcherItem::noModifierGrab() const {
  return Exoskeleton::instance()->noModifierGrab();
}

bool SwitcherItem::compositing() const { return true; }

QObject* SwitcherItem::item() const { return m_item; }

void SwitcherItem::setItem(QObject* item) {
  if (m_item == item) {
    return;
  }
  m_item = item;
  Q_EMIT itemChanged();
}

}  // namespace Occultation
