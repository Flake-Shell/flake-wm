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
 * Registers the shim's QML types in the org.gxde.occultation module.
 */

#include "src/shell/gxde/kwin_shim/registration/registration.h"

#include <QJSEngine>
#include <QQmlEngine>
#include <QResource>
#include <QtQml>

#include "src/shell/gxde/kwin_shim/exoskeleton/exoskeleton.h"
#include "src/shell/gxde/kwin_shim/icon_item/icon_item.h"
#include "src/shell/gxde/kwin_shim/switcher_item/switcher_item.h"
#include "src/shell/gxde/kwin_shim/switcher_model/switcher_model.h"
#include "src/shell/gxde/kwin_shim/thumbnail_item/thumbnail_item.h"

static void initializeOccultationResources() { Q_INIT_RESOURCE(resources); }

namespace Occultation {

void registerQmlTypes() {
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  initializeOccultationResources();

  qmlRegisterType<SwitcherItem>("org.gxde.occultation", 1, 0, "Switcher");
  qmlRegisterType<ThumbnailItem>("org.gxde.occultation", 1, 0, "ThumbnailItem");
  qmlRegisterType<IconItem>("org.gxde.occultation", 1, 0, "IconItem");
  qmlRegisterUncreatableType<SwitcherModel>(
      "org.gxde.occultation", 1, 0, "SwitcherModel",
      QStringLiteral("SwitcherModel is owned by Exoskeleton"));
  qmlRegisterSingletonType<Exoskeleton>(
      "org.gxde.occultation", 1, 0, "Runtime",
      [](QQmlEngine* engine, QJSEngine*) -> QObject* {
        Exoskeleton* runtime = Exoskeleton::instance();
        QQmlEngine::setObjectOwnership(runtime, QQmlEngine::CppOwnership);
        Q_UNUSED(engine)
        return runtime;
      });
}

}  // namespace Occultation
