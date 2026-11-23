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
 * The runtime of the KWin shim: it talks to flakewm through libkywc and
 * feeds the window list and switcher state to the gxde-kwin QML frontend.
 */

#ifndef SRC_SHELL_GXDE_KWIN_SHIM_EXOSKELETON_EXOSKELETON_H_
#define SRC_SHELL_GXDE_KWIN_SHIM_EXOSKELETON_EXOSKELETON_H_

#include <QObject>
#include <QRect>
#include <QString>

#include "src/shell/gxde/kwin_shim/switcher_model/switcher_model.h"

class QSocketNotifier;

typedef struct _kywc_context kywc_context;
typedef struct _kywc_output kywc_output;
typedef struct _kywc_toplevel kywc_toplevel;
typedef struct _kywc_workspace kywc_workspace;

namespace Occultation {

class Exoskeleton final : public QObject {
  Q_OBJECT
  Q_PROPERTY(SwitcherModel* model READ model CONSTANT)
  Q_PROPERTY(bool started READ isStarted NOTIFY startedChanged)
  Q_PROPERTY(bool visible READ isVisible NOTIFY visibleChanged)
  Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY
                 currentIndexChanged)
  Q_PROPERTY(QRect screenGeometry READ screenGeometry WRITE setScreenGeometry
                 NOTIFY screenGeometryChanged)
  Q_PROPERTY(bool allDesktops READ allDesktops WRITE setAllDesktops NOTIFY
                 allDesktopsChanged)
  Q_PROPERTY(bool noModifierGrab READ noModifierGrab WRITE setNoModifierGrab
                 NOTIFY noModifierGrabChanged)

 public:
  static Exoskeleton* instance();

  SwitcherModel* model() const;
  kywc_context* context() const;

  bool isStarted() const;
  bool isVisible() const;
  int currentIndex() const;
  QRect screenGeometry() const;
  bool allDesktops() const;
  bool noModifierGrab() const;

  Q_INVOKABLE bool start(const QString& displayName = QString());
  Q_INVOKABLE void stop();
  Q_INVOKABLE void show(int direction = 1);
  Q_INVOKABLE void accept();
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void next();
  Q_INVOKABLE void previous();

  void setCurrentIndex(int index);
  void setScreenGeometry(const QRect& geometry);
  void setAllDesktops(bool allDesktops);
  void setNoModifierGrab(bool noModifierGrab);

  QString protocolUuid(const QUuid& windowId) const;
  bool isDesktopId(const QUuid& windowId) const;
  QString currentOutputUuid() const;
  QString currentWorkspaceUuid() const;

  static void handleContextCreate(kywc_context* context, void* data);
  static void handleContextDestroy(kywc_context* context, void* data);
  static void handleNewOutput(kywc_context* context, kywc_output* output,
                              void* data);
  static void handleNewToplevel(kywc_context* context, kywc_toplevel* toplevel,
                                void* data);
  static void handleNewWorkspace(kywc_context* context,
                                 kywc_workspace* workspace, void* data);

 signals:
  void startedChanged();
  void visibleChanged();
  void currentIndexChanged(int index);
  void screenGeometryChanged();
  void allDesktopsChanged();
  void noModifierGrabChanged();
  void connectionLost();
  void showDesktopRequested();

 private slots:
  void processWaylandEvents();
  void requestShowDesktop();

 private:
  explicit Exoskeleton(QObject* parent = nullptr);
  ~Exoskeleton() override;

  void setVisible(bool visible);
  void normalizeCurrentIndex();

  SwitcherModel* m_model = nullptr;
  kywc_context* m_context = nullptr;
  QSocketNotifier* m_notifier = nullptr;
  QRect m_screenGeometry;
  bool m_visible = false;
  bool m_allDesktops = false;
  bool m_noModifierGrab = false;
  bool m_stopping = false;
  int m_currentIndex = 0;
};

}  // namespace Occultation

#endif  // SRC_SHELL_GXDE_KWIN_SHIM_EXOSKELETON_EXOSKELETON_H_
