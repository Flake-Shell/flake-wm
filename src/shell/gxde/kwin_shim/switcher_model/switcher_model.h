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
 * The window list model of the window switcher, built from flakewm's
 * toplevels.
 */

#ifndef SRC_SHELL_GXDE_KWIN_SHIM_SWITCHER_MODEL_SWITCHER_MODEL_H_
#define SRC_SHELL_GXDE_KWIN_SHIM_SWITCHER_MODEL_SWITCHER_MODEL_H_

#include <QAbstractListModel>
#include <QUuid>

typedef struct _kywc_toplevel kywc_toplevel;

namespace Occultation {

class Exoskeleton;

class SwitcherModel final : public QAbstractListModel {
  Q_OBJECT

 public:
  enum Role {
    ClientRole = Qt::UserRole,
    CaptionRole = Qt::UserRole + 1,
    DesktopNameRole = Qt::UserRole + 2,
    IconRole = Qt::UserRole + 3,
    WIdRole = Qt::UserRole + 5,
    MinimizedRole = Qt::UserRole + 6,
    CloseableRole = Qt::UserRole + 7,
  };
  Q_ENUM(Role)

  explicit SwitcherModel(QObject* parent = nullptr);
  ~SwitcherModel() override;

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index,
                int role = Qt::DisplayRole) const override;
  QHash<int, QByteArray> roleNames() const override;

  Q_INVOKABLE QString longestCaption() const;

 public slots:
  void close(int index);
  void activate(int index);

 signals:
  void showDesktopRequested();

 private:
  friend class Exoskeleton;
  friend class ThumbnailItem;
  struct Entry;

  void addToplevel(kywc_toplevel* toplevel);
  void removeToplevel(kywc_toplevel* toplevel);
  void setFrozen(bool frozen);
  void refreshDesktopEntry();
  QString protocolUuid(const QUuid& windowId) const;
  bool isDesktopId(const QUuid& windowId) const;

  static void handleToplevelState(kywc_toplevel* toplevel, uint32_t mask);
  static void handleToplevelDestroy(kywc_toplevel* toplevel);

  int rowForToplevel(kywc_toplevel* toplevel) const;
  int rowForEntry(const Entry* entry) const;
  Entry* desktopEntry() const;
  void updateEntry(Entry* entry, uint32_t mask);
  void promoteActivated(Entry* entry);

  QVector<Entry*> m_entries;
  QVector<Entry*> m_desktopEntries;
  bool m_frozen = false;
};

}  // namespace Occultation

#endif  // SRC_SHELL_GXDE_KWIN_SHIM_SWITCHER_MODEL_SWITCHER_MODEL_H_
