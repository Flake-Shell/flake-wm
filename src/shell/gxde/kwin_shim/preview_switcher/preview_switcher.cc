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

#include "src/shell/gxde/kwin_shim/preview_switcher/preview_switcher.h"

namespace Occultation {

PreviewSwitcher::PreviewSwitcher(QObject* parent)
    : QObject(parent), m_model(this) {}

QAbstractItemModel* PreviewSwitcher::model() { return &m_model; }

QRect PreviewSwitcher::screenGeometry() const {
  return QRect(0, 0, 1920, 1080);
}

bool PreviewSwitcher::isVisible() const { return true; }

bool PreviewSwitcher::allDesktops() const { return false; }

int PreviewSwitcher::currentIndex() const { return m_currentIndex; }

void PreviewSwitcher::setCurrentIndex(int index) {
  if (m_currentIndex == index) {
    return;
  }
  m_currentIndex = index;
  Q_EMIT currentIndexChanged(index);
}

bool PreviewSwitcher::noModifierGrab() const { return false; }

bool PreviewSwitcher::compositing() const { return true; }

QObject* PreviewSwitcher::item() const { return m_item; }

void PreviewSwitcher::setItem(QObject* item) {
  if (m_item == item) {
    return;
  }
  m_item = item;
  Q_EMIT itemChanged();
}

}  // namespace Occultation
