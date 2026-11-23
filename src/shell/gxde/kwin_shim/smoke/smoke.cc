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
 * Smoke test: loads the window switcher QML against the shim's QML types.
 */

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>

#include "src/shell/gxde/kwin_shim/registration/registration.h"

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  Occultation::registerQmlTypes();

  QQmlEngine engine;
  QQmlComponent component(
      &engine,
      QUrl(QStringLiteral("qrc:/org/gxde/occultation/switcher/main.qml")));
  if (component.isError()) {
    qCritical().noquote() << component.errorString();
    return 1;
  }

  QScopedPointer<QObject> root(component.create());
  if (!root) {
    qCritical().noquote() << component.errorString();
    return 2;
  }
  return 0;
}
