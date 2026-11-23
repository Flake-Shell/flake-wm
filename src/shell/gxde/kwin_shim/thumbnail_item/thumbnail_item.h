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
 * A QML item showing a live window thumbnail streamed from flakewm.
 */

#ifndef SRC_SHELL_GXDE_KWIN_SHIM_THUMBNAIL_ITEM_THUMBNAIL_ITEM_H_
#define SRC_SHELL_GXDE_KWIN_SHIM_THUMBNAIL_ITEM_THUMBNAIL_ITEM_H_

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <QImage>
#include <QMutex>
#include <QQuickItem>
#include <QSize>
#include <QUuid>

typedef struct _kywc_thumbnail kywc_thumbnail;
struct kywc_thumbnail_buffer;

namespace Occultation {

class ThumbnailItem : public QQuickItem {
  Q_OBJECT
  Q_PROPERTY(QSize sourceSize READ sourceSize WRITE setSourceSize NOTIFY
                 sourceSizeChanged)
  Q_PROPERTY(qreal brightness READ brightness WRITE setBrightness NOTIFY
                 brightnessChanged)
  Q_PROPERTY(qreal saturation READ saturation WRITE setSaturation NOTIFY
                 saturationChanged)
  Q_PROPERTY(
      QQuickItem* clipTo READ clipTo WRITE setClipTo NOTIFY clipToChanged)
  Q_PROPERTY(QUuid wId READ wId WRITE setWId NOTIFY wIdChanged)

 public:
  explicit ThumbnailItem(QQuickItem* parent = nullptr);
  ~ThumbnailItem() override;

  QSize sourceSize() const;
  void setSourceSize(const QSize& sourceSize);
  qreal brightness() const;
  void setBrightness(qreal brightness);
  qreal saturation() const;
  void setSaturation(qreal saturation);
  QQuickItem* clipTo() const;
  void setClipTo(QQuickItem* clip);
  QUuid wId() const;
  void setWId(const QUuid& windowId);

  static bool handleBuffer(kywc_thumbnail* thumbnail,
                           const struct kywc_thumbnail_buffer* buffer,
                           void* data);
  static void handleDestroy(kywc_thumbnail* thumbnail, void* data);

 signals:
  void sourceSizeChanged();
  void brightnessChanged();
  void saturationChanged();
  void clipToChanged();
  void wIdChanged();

 protected:
  QSGNode* updatePaintNode(QSGNode* oldNode,
                           UpdatePaintNodeData* data) override;
  void releaseResources() override;

 private slots:
  void connectCapture();

 private:
  struct PendingFrame {
    EGLImageKHR eglImage = EGL_NO_IMAGE_KHR;
    QImage image;
    QSize size;
    uint32_t format = 0;
    bool changed = false;
  };

  bool importBuffer(const struct kywc_thumbnail_buffer* buffer);
  bool importDmaBuf(const struct kywc_thumbnail_buffer* buffer);
  bool importMemFd(const struct kywc_thumbnail_buffer* buffer);
  void releaseCapture();
  void clearPendingFrame();
  QRectF paintedRect(const QSize& contentSize) const;

  mutable QMutex m_frameMutex;
  PendingFrame m_pendingFrame;
  kywc_thumbnail* m_thumbnail = nullptr;
  EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
  QSize m_sourceSize;
  QSize m_contentSize;
  QUuid m_windowId;
  qreal m_brightness = 1.0;
  qreal m_saturation = 1.0;
  QQuickItem* m_clipTo = nullptr;
};

}  // namespace Occultation

#endif  // SRC_SHELL_GXDE_KWIN_SHIM_THUMBNAIL_ITEM_THUMBNAIL_ITEM_H_
