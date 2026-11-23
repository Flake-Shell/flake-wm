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
 * The fake window list used by the standalone switcher preview.
 */

#ifndef SRC_SHELL_GXDE_KWIN_SHIM_PREVIEW_MODEL_PREVIEW_MODEL_H_
#define SRC_SHELL_GXDE_KWIN_SHIM_PREVIEW_MODEL_PREVIEW_MODEL_H_

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QUuid>

namespace Occultation {

class PreviewModel final : public QAbstractListModel {
  Q_OBJECT

 public:
  enum Role {
    CaptionRole = Qt::UserRole + 1,
    DesktopNameRole,
    MinimizedRole,
    WIdRole,
    CloseableRole,
    IconRole,
  };

  explicit PreviewModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

 private:
  struct Entry {
    QString caption;
    QString iconName;
    QUuid windowId;
    bool closeable = true;
  };

  QList<Entry> m_entries;
};

}  // namespace Occultation

#endif  // SRC_SHELL_GXDE_KWIN_SHIM_PREVIEW_MODEL_PREVIEW_MODEL_H_
