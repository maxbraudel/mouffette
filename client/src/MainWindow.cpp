#include "MainWindow.h"
#include <QMenuBar>
#include <QMessageBox>
#include <QHostInfo>
#include <QGuiApplication>
#include <QDebug>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QStyleOption>
#include <algorithm>
#include <cmath>
#include <QDialog>
#include <QLineEdit>
#include <QNativeGestureEvent>
#include <QCursor>
#include <QRandomGenerator>
#include <QPainter>
#include <QPaintEvent>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDragMoveEvent>
#include <QMimeData>
#include <QGraphicsPixmapItem>
#include <QGraphicsSceneMouseEvent>
#include <QPen>
#include <QBrush>
#include <QUrl>
#include <QImage>
#include <QPixmap>
#include <QGraphicsTextItem>
#include <QGraphicsRectItem>
#include <QGraphicsPathItem>
#include <QPainterPathStroker>
#include <QFileInfo>
#include <QGraphicsItem>
#include <QSet>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoSink>
#include <QMediaMetaData>
#include <QVideoFrame>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <combaseapi.h>
// Ensure GUIDs (IIDs/CLSIDs) are defined in this translation unit for MinGW linkers
#ifndef INITGUID
#define INITGUID
#endif
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#endif
#ifdef Q_OS_MACOS
#include <QProcess>
#endif

const QString MainWindow::DEFAULT_SERVER_URL = "ws://192.168.0.188:8080";

// Ensure all fade animations respect the configured duration
void MainWindow::applyAnimationDurations() {
    if (m_spinnerFade) m_spinnerFade->setDuration(m_loaderFadeDurationMs);
    if (m_canvasFade) m_canvasFade->setDuration(m_fadeDurationMs);
    if (m_volumeFade) m_volumeFade->setDuration(m_fadeDurationMs);
}

// Simple modern circular line spinner (no Q_OBJECT needed)
class SpinnerWidget : public QWidget {
public:
    explicit SpinnerWidget(QWidget* parent = nullptr)
        : QWidget(parent)
        , m_angle(0)
        , m_radiusPx(24)      // default visual radius in pixels
        , m_lineWidthPx(6)    // default line width
        , m_color("#4a90e2") // default color matches Send button
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, [this]() {
            m_angle = (m_angle + 6) % 360; // smooth rotation
            update();
        });
        m_timer->setInterval(16); // ~60 FPS
    }

    void start() { if (!m_timer->isActive()) m_timer->start(); }
    void stop() { if (m_timer->isActive()) m_timer->stop(); }

    // Customization knobs
    void setRadius(int radiusPx) {
        m_radiusPx = qMax(8, radiusPx);
        updateGeometry();
        update();
    }
    void setLineWidth(int px) {
        m_lineWidthPx = qMax(1, px);
        update();
    }
    void setColor(const QColor& c) {
        m_color = c;
        update();
    }
    int radius() const { return m_radiusPx; }
    int lineWidth() const { return m_lineWidthPx; }
    QColor color() const { return m_color; }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        // Palette-aware background
        QStyleOption opt; opt.initFrom(this);
        style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);

    const int s = qMin(width(), height());
    // Use configured radius but ensure it fits inside current widget size with padding
    int outer = 2 * m_radiusPx;        // desired diameter from config
    const int maxOuter = qMax(16, s - 12); // keep some padding to avoid clipping
    outer = qMin(outer, maxOuter);
    const int thickness = qMin(m_lineWidthPx, qMax(1, outer / 2)); // ensure sane bounds

        QPointF center(width() / 2.0, height() / 2.0);
        QRectF rect(center.x() - outer/2.0, center.y() - outer/2.0, outer, outer);
        
    QColor arc = m_color;
    arc.setAlpha(230);

    // Flat caps (no rounded ends) as requested
    p.setPen(QPen(arc, thickness, Qt::SolidLine, Qt::FlatCap));
        int span = 16 * 300; // 300-degree arc for modern look
        p.save();
        p.translate(center);
        p.rotate(m_angle);
        p.translate(-center);
        p.drawArc(rect, 0, span);
        p.restore();
    }

    QSize minimumSizeHint() const override { return QSize(48, 48); }

private:
    QTimer* m_timer;
    int m_angle;
    int m_radiusPx;
    int m_lineWidthPx;
    QColor m_color;
};

// Simple rounded-rectangle graphics item with a settable rect and radius
class RoundedRectItem : public QGraphicsPathItem {
public:
    explicit RoundedRectItem(QGraphicsItem* parent = nullptr)
        : QGraphicsPathItem(parent) {}
    void setRect(const QRectF& r) { m_rect = r; updatePath(); }
    void setRect(qreal x, qreal y, qreal w, qreal h) { setRect(QRectF(x, y, w, h)); }
    QRectF rect() const { return m_rect; }
    void setRadius(qreal radiusPx) { m_radius = std::max<qreal>(0.0, radiusPx); updatePath(); }
    qreal radius() const { return m_radius; }
private:
    void updatePath() {
        QPainterPath p;
        if (m_rect.isNull()) { setPath(p); return; }
        // Clamp radius so it never exceeds half of width/height
        const qreal r = std::min({ m_radius, m_rect.width() * 0.5, m_rect.height() * 0.5 });
        if (r > 0.0)
            p.addRoundedRect(m_rect, r, r);
        else
            p.addRect(m_rect);
        setPath(p);
    }
    QRectF m_rect;
    qreal  m_radius = 0.0;
};

// Resizable, movable pixmap item with corner handles; keeps aspect ratio
class ResizableMediaBase : public QGraphicsItem {
public:
    explicit ResizableMediaBase(const QSize& baseSizePx, int visualSizePx, int selectionSizePx, const QString& filename = QString())
    {
        m_visualSize = qMax(4, visualSizePx);
        m_selectionSize = qMax(m_visualSize, selectionSizePx);
        setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);
        m_baseSize = baseSizePx;
        setScale(1.0);
        setZValue(1.0);
        // Filename label setup (zoom-independent via ItemIgnoresTransformations)
        m_filename = filename;
    m_labelBg = new RoundedRectItem(this);
    m_labelBg->setPen(Qt::NoPen);
    // Unified translucent dark background used by label and video controls
    m_labelBg->setBrush(QColor(0, 0, 0, 160));
        m_labelBg->setZValue(100.0);
        m_labelBg->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        m_labelBg->setAcceptedMouseButtons(Qt::NoButton);
        m_labelText = new QGraphicsTextItem(m_filename, m_labelBg);
        m_labelText->setDefaultTextColor(Qt::white);
        m_labelText->setZValue(101.0);
        m_labelText->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
        m_labelText->setAcceptedMouseButtons(Qt::NoButton);
        updateLabelLayout();
    }
    // Global override (in pixels) for the height of media overlays (e.g., video controls).
    // -1 means "auto" (use filename label background height).
    static void setHeightOfMediaOverlaysPx(int px) { heightOfMediaOverlays = px; }
    static int getHeightOfMediaOverlaysPx() { return heightOfMediaOverlays; }
    // Global override for corner radius (in pixels) for overlay buttons and filename label background
    // Applies to: filename background, play/stop/repeat/mute buttons. Excludes: progress & volume bars.
    static void setCornerRadiusOfMediaOverlaysPx(int px) { cornerRadiusOfMediaOverlays = std::max(0, px); }
    static int getCornerRadiusOfMediaOverlaysPx() { return cornerRadiusOfMediaOverlays; }
    // Utility for view: tell if a given item-space pos is on a resize handle
    bool isOnHandleAtItemPos(const QPointF& itemPos) const {
        return hitTestHandle(itemPos) != None;
    }

    // Start a resize operation given a scene position; returns true if a handle was engaged
    bool beginResizeAtScenePos(const QPointF& scenePos) {
        Handle h = hitTestHandle(mapFromScene(scenePos));
        if (h == None) return false;
        m_activeHandle = h;
        m_fixedItemPoint = handlePoint(opposite(m_activeHandle));
        m_fixedScenePoint = mapToScene(m_fixedItemPoint);
    m_initialScale = scale();
    const qreal d = std::hypot(scenePos.x() - m_fixedScenePoint.x(),
                   scenePos.y() - m_fixedScenePoint.y());
    m_initialGrabDist = (d > 1e-6) ? d : 1e-6;
        grabMouse();
        return true;
    }

    // For view-level hover support: suggest a cursor based on a scene position
    Qt::CursorShape cursorForScenePos(const QPointF& scenePos) const {
        switch (hitTestHandle(mapFromScene(scenePos))) {
            case TopLeft:
            case BottomRight:
                return Qt::SizeFDiagCursor;
            case TopRight:
            case BottomLeft:
                return Qt::SizeBDiagCursor;
            default:
                return Qt::ArrowCursor;
        }
    }

    // Check if this item is currently being resized
    bool isActivelyResizing() const {
        return m_activeHandle != None;
    }

    void setHandleVisualSize(int px) {
        int newVisual = qMax(4, px);
        int newSelection = qMax(m_selectionSize, newVisual);
        if (newSelection != m_selectionSize) {
            prepareGeometryChange();
            m_selectionSize = newSelection;
        }
        m_visualSize = newVisual;
        update();
    }
    void setHandleSelectionSize(int px) {
        int newSel = qMax(4, px);
        if (newSel != m_selectionSize) {
            prepareGeometryChange();
            m_selectionSize = newSel;
            update();
        }
    }

    QRectF boundingRect() const override {
        QRectF br(0, 0, m_baseSize.width(), m_baseSize.height());
        // Only extend bounding rect for handle hit zones when selected
        if (isSelected()) {
            qreal pad = toItemLengthFromPixels(m_selectionSize) / 2.0;
            return br.adjusted(-pad, -pad, pad, pad);
        }
        return br;
    }

    QPainterPath shape() const override {
        QPainterPath path;
        QRectF br(0, 0, m_baseSize.width(), m_baseSize.height());
        path.addRect(br);
        // Only add handle hit zones when selected
        if (isSelected()) {
            const qreal s = toItemLengthFromPixels(m_selectionSize);
            path.addRect(QRectF(br.topLeft() - QPointF(s/2,s/2), QSizeF(s,s)));
            path.addRect(QRectF(QPointF(br.right(), br.top()) - QPointF(s/2,s/2), QSizeF(s,s)));
            path.addRect(QRectF(QPointF(br.left(), br.bottom()) - QPointF(s/2,s/2), QSizeF(s,s)));
            path.addRect(QRectF(br.bottomRight() - QPointF(s/2,s/2), QSizeF(s,s)));
        }
        return path;
    }

protected:
    // Draw selection chrome and keep label positioned
    void paintSelectionAndLabel(QPainter* painter) {
        updateLabelLayout();
        if (!isSelected()) return;
        QRectF br(0, 0, m_baseSize.width(), m_baseSize.height());
        painter->save();
        painter->setBrush(Qt::NoBrush);
        QPen whitePen(QColor(255,255,255));
        whitePen.setCosmetic(true);
        whitePen.setWidth(1);
        whitePen.setStyle(Qt::DashLine);
        whitePen.setDashPattern(QVector<qreal>({4, 4}));
        whitePen.setCapStyle(Qt::FlatCap);
        whitePen.setJoinStyle(Qt::MiterJoin);
        painter->setPen(whitePen);
        painter->drawRect(br);
        QPen bluePen(QColor(74,144,226));
        bluePen.setCosmetic(true);
        bluePen.setWidth(1);
        bluePen.setStyle(Qt::DashLine);
        bluePen.setDashPattern(QVector<qreal>({4, 4}));
        bluePen.setDashOffset(4);
        bluePen.setCapStyle(Qt::FlatCap);
        bluePen.setJoinStyle(Qt::MiterJoin);
        painter->setPen(bluePen);
        painter->drawRect(br);
        painter->restore();
        const qreal s = toItemLengthFromPixels(m_visualSize);
        painter->setPen(QPen(QColor(74,144,226), 0));
        painter->setBrush(QBrush(Qt::white));
        painter->drawRect(QRectF(br.topLeft() - QPointF(s/2,s/2), QSizeF(s,s)));
        painter->drawRect(QRectF(QPointF(br.right(), br.top()) - QPointF(s/2,s/2), QSizeF(s,s)));
        painter->drawRect(QRectF(QPointF(br.left(), br.bottom()) - QPointF(s/2,s/2), QSizeF(s,s)));
        painter->drawRect(QRectF(br.bottomRight() - QPointF(s/2,s/2), QSizeF(s,s)));
    }

    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override {
        if (change == ItemSelectedChange) {
            // Include/exclude handle zones (affects bounding/shape)
            prepareGeometryChange();
            // Pre-toggle visibility to match new state
            if (m_labelBg && m_labelText) {
                const bool willBeSelected = value.toBool();
                const bool show = willBeSelected && !m_filename.isEmpty();
                m_labelBg->setVisible(show);
                m_labelText->setVisible(show);
            }
        }
        if (change == ItemSelectedHasChanged) {
            // Keep label properly positioned after selection changes
            updateLabelLayout();
        }
    return QGraphicsItem::itemChange(change, value);
    }

    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        m_activeHandle = hitTestHandle(event->pos());
        if (m_activeHandle != None) {
            m_fixedItemPoint = handlePoint(opposite(m_activeHandle));
            m_fixedScenePoint = mapToScene(m_fixedItemPoint);
            // Capture initial state so scaling starts from current cursor distance (no jump)
            m_initialScale = scale();
            const qreal d = std::hypot(event->scenePos().x() - m_fixedScenePoint.x(),
                                       event->scenePos().y() - m_fixedScenePoint.y());
            m_initialGrabDist = (d > 1e-6) ? d : 1e-6;
            event->accept();
            return;
        }
    QGraphicsItem::mousePressEvent(event);
    }

    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override {
        if (m_activeHandle != None) {
            const QPointF v = event->scenePos() - m_fixedScenePoint;
            const qreal currDist = std::hypot(v.x(), v.y());
            qreal newScale = m_initialScale * (currDist / (m_initialGrabDist > 0 ? m_initialGrabDist : 1e-6));
            newScale = std::clamp<qreal>(newScale, 0.05, 100.0);
            setScale(newScale);
            setPos(m_fixedScenePoint - newScale * m_fixedItemPoint);
            event->accept();
            return;
        }
        // Do not manage cursor here - let the view handle it globally
    QGraphicsItem::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        if (m_activeHandle != None) {
            m_activeHandle = None;
            ungrabMouse();
            event->accept();
            return;
        }
    QGraphicsItem::mouseReleaseEvent(event);
    }

    void hoverMoveEvent(QGraphicsSceneHoverEvent* event) override {
        // Do not manage cursor here - let the view handle it globally
    QGraphicsItem::hoverMoveEvent(event);
    }

    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override {
        // Do not manage cursor here - let the view handle it globally
    QGraphicsItem::hoverLeaveEvent(event);
    }

protected:
    enum Handle { None, TopLeft, TopRight, BottomLeft, BottomRight };
    QSize m_baseSize;
    Handle m_activeHandle = None;
    QPointF m_fixedItemPoint;  // item coords
    QPointF m_fixedScenePoint; // scene coords
    qreal m_initialScale = 1.0;
    qreal m_initialGrabDist = 1.0;
    // Desired sizes in device pixels (screen space)
    int m_visualSize = 8;
    int m_selectionSize = 12;
    Handle m_lastHoverHandle = None;
    // Filename label elements
    QString m_filename;
    RoundedRectItem* m_labelBg = nullptr;
    QGraphicsTextItem* m_labelText = nullptr;
    // Shared overlay height config
    static int heightOfMediaOverlays;
    static int cornerRadiusOfMediaOverlays;

    Handle hitTestHandle(const QPointF& p) const {
        // Only allow handle interaction when selected
        if (!isSelected()) return None;
        
        const qreal s = toItemLengthFromPixels(m_selectionSize); // selection square centered on corners
        QRectF br(0, 0, m_baseSize.width(), m_baseSize.height());
        if (QRectF(br.topLeft() - QPointF(s/2,s/2), QSizeF(s,s)).contains(p)) return TopLeft;
        if (QRectF(QPointF(br.right(), br.top()) - QPointF(s/2,s/2), QSizeF(s,s)).contains(p)) return TopRight;
        if (QRectF(QPointF(br.left(), br.bottom()) - QPointF(s/2,s/2), QSizeF(s,s)).contains(p)) return BottomLeft;
        if (QRectF(br.bottomRight() - QPointF(s/2,s/2), QSizeF(s,s)).contains(p)) return BottomRight;
        return None;
    }
    // No global override cursor helpers needed with per-item cursors
    Handle opposite(Handle h) const {
        switch (h) {
            case TopLeft: return BottomRight;
            case TopRight: return BottomLeft;
            case BottomLeft: return TopRight;
            case BottomRight: return TopLeft;
            default: return None;
        }
    }
    QPointF handlePoint(Handle h) const {
        switch (h) {
            case TopLeft: return QPointF(0,0);
            case TopRight: return QPointF(m_baseSize.width(), 0);
            case BottomLeft: return QPointF(0, m_baseSize.height());
            case BottomRight: return QPointF(m_baseSize.width(), m_baseSize.height());
            default: return QPointF(0,0);
        }
    }

    qreal toItemLengthFromPixels(int px) const {
        if (!scene() || scene()->views().isEmpty()) return px; // fallback
        QGraphicsView* v = scene()->views().first();
        QTransform itemToViewport = v->viewportTransform() * sceneTransform();
        // Effective scale along X (uniform expected)
        qreal sx = std::hypot(itemToViewport.m11(), itemToViewport.m21());
        if (sx <= 1e-6) return px;
        // Convert device pixels to item units
        return px / sx;
    }

    void updateLabelLayout() {
        if (!m_labelBg || !m_labelText) return;
        const bool show = !m_filename.isEmpty() && isSelected();
        m_labelBg->setVisible(show);
        m_labelText->setVisible(show);
        if (!show) return;

        // Padding and vertical gap in pixels
        const int padXpx = 8;
        const int padYpx = 4;
        const int gapPx  = 8;
        // Update text and background sizes (in px, unaffected by zoom)
        m_labelText->setPlainText(m_filename);
        QRectF tr = m_labelText->boundingRect();
        const qreal bgW = tr.width() + 2*padXpx;
        const qreal bgH = tr.height() + 2*padYpx;
    m_labelBg->setRect(0, 0, bgW, bgH);
    // Apply configured corner radius
    m_labelBg->setRadius(ResizableMediaBase::getCornerRadiusOfMediaOverlaysPx());
        m_labelText->setPos(padXpx, padYpx);

        // Position: center above image top edge
        if (!scene() || scene()->views().isEmpty()) {
            // Fallback using item-units conversion
            const qreal wItem = toItemLengthFromPixels(static_cast<int>(std::round(bgW)));
            const qreal hItem = toItemLengthFromPixels(static_cast<int>(std::round(bgH)));
            const qreal gapItem = toItemLengthFromPixels(gapPx);
            const qreal xItem = (m_baseSize.width() - wItem) / 2.0;
            const qreal yItem = - (hItem + gapItem);
            m_labelBg->setPos(xItem, yItem);
            return;
        }
        QGraphicsView* v = scene()->views().first();
        // Parent top-center in viewport coords
        QPointF topCenterItem(m_baseSize.width()/2.0, 0.0);
        QPointF topCenterScene = mapToScene(topCenterItem);
        QPointF topCenterView = v->mapFromScene(topCenterScene);
        // Desired top-left of label in viewport (pixels)
        QPointF labelTopLeftView = topCenterView - QPointF(bgW/2.0, gapPx + bgH);
        // Map back to item coords
        QPointF labelTopLeftScene = v->mapToScene(labelTopLeftView.toPoint());
        QPointF labelTopLeftItem = mapFromScene(labelTopLeftScene);
    m_labelBg->setPos(labelTopLeftItem);
    }
};

// Default: auto (match filename label background height)
int ResizableMediaBase::heightOfMediaOverlays = -1;
int ResizableMediaBase::cornerRadiusOfMediaOverlays = 6;

// Image media implementation using the shared base
class ResizablePixmapItem : public ResizableMediaBase {
public:
    explicit ResizablePixmapItem(const QPixmap& pm, int visualSizePx, int selectionSizePx, const QString& filename = QString())
        : ResizableMediaBase(pm.size(), visualSizePx, selectionSizePx, filename), m_pix(pm)
    {}
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override {
        Q_UNUSED(option); Q_UNUSED(widget);
        if (!m_pix.isNull()) {
            painter->drawPixmap(QPointF(0,0), m_pix);
        }
        paintSelectionAndLabel(painter);
    }
private:
    QPixmap m_pix;
};

// Video media implementation: renders current frame and overlays controls
class ResizableVideoItem : public ResizableMediaBase {
public:
    explicit ResizableVideoItem(const QString& filePath, int visualSizePx, int selectionSizePx, const QString& filename = QString())
        : ResizableMediaBase(QSize(640,360), visualSizePx, selectionSizePx, filename)
    {
        m_player = new QMediaPlayer();
        m_audio = new QAudioOutput();
        m_sink = new QVideoSink();
        m_player->setAudioOutput(m_audio);
        m_player->setVideoSink(m_sink);
        m_player->setSource(QUrl::fromLocalFile(filePath));
    // Size adoption will be handled from frames and metadata; no direct videoSizeChanged hookup to keep Qt compatibility
        QObject::connect(m_sink, &QVideoSink::videoFrameChanged, [this](const QVideoFrame& f){
            // Only replace the cached frame when valid and not holding the last frame at end-of-media
            if (!m_holdLastFrameAtEnd && f.isValid()) {
                m_lastFrame = f;
                maybeAdoptFrameSize(f);
                // If we primed playback to grab the first frame, pause immediately and restore audio state
                if (m_primingFirstFrame && !m_firstFramePrimed) {
                    m_firstFramePrimed = true;
                    m_primingFirstFrame = false;
                    if (m_player) {
                        m_player->pause();
                        m_player->setPosition(0);
                    }
                    if (m_audio) m_audio->setMuted(m_savedMuted);
                }
            }
            this->update();
        });
        // On load, adopt size from metadata, try to fetch poster image, and prime first frame (without user action)
        QObject::connect(m_player, &QMediaPlayer::mediaStatusChanged, [this](QMediaPlayer::MediaStatus s){
            if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) {
                if (!m_adoptedSize) {
                    const QMediaMetaData md = m_player->metaData();
                    const QVariant v = md.value(QMediaMetaData::Resolution);
                    const QSize sz = v.toSize();
                    if (!sz.isEmpty()) adoptBaseSize(sz);
                    // Try to use a poster/thumbnail image while waiting for the first frame
                    const QVariant thumbVar = md.value(QMediaMetaData::ThumbnailImage);
                    if (!m_posterImageSet && thumbVar.isValid()) {
                        if (thumbVar.canConvert<QImage>()) {
                            m_posterImage = thumbVar.value<QImage>();
                            m_posterImageSet = !m_posterImage.isNull();
                        } else if (thumbVar.canConvert<QPixmap>()) {
                            m_posterImage = thumbVar.value<QPixmap>().toImage();
                            m_posterImageSet = !m_posterImage.isNull();
                        }
                        if (!m_posterImageSet) {
                            const QVariant coverVar = md.value(QMediaMetaData::CoverArtImage);
                            if (coverVar.canConvert<QImage>()) {
                                m_posterImage = coverVar.value<QImage>();
                                m_posterImageSet = !m_posterImage.isNull();
                            } else if (coverVar.canConvert<QPixmap>()) {
                                m_posterImage = coverVar.value<QPixmap>().toImage();
                                m_posterImageSet = !m_posterImage.isNull();
                            }
                        }
                        if (m_posterImageSet) this->update();
                    }
                }
                if (!m_firstFramePrimed && !m_primingFirstFrame) {
                    m_holdLastFrameAtEnd = false; // new load, don't hold
                    m_savedMuted = m_audio ? m_audio->isMuted() : false;
                    if (m_audio) m_audio->setMuted(true);
                    m_primingFirstFrame = true;
                    if (m_player) m_player->play();
                }
            }
            if (s == QMediaPlayer::EndOfMedia) {
                if (m_repeatEnabled) {
                    m_player->setPosition(0);
                    m_player->play();
                } else {
                    // Keep showing the last frame and pin progress to 100%
                    m_holdLastFrameAtEnd = true;
                    if (m_player) m_player->pause();
                    if (m_durationMs > 0) m_positionMs = m_durationMs;
                    updateControlsLayout();
                    update();
                }
            }
        });
    QObject::connect(m_player, &QMediaPlayer::durationChanged, [this](qint64 d){ m_durationMs = d; this->update(); });
    QObject::connect(m_player, &QMediaPlayer::positionChanged, [this](qint64 p){
        if (m_holdLastFrameAtEnd) return; // keep progress pinned at end until user action
        m_positionMs = p; this->update();
    });

    // Controls overlays (ignore transforms so they stay in absolute pixels)
    m_controlsBg = new QGraphicsRectItem(this);
    m_controlsBg->setPen(Qt::NoPen);
    // Keep container transparent; draw backgrounds on child rects so play is a square, progress is full-width
    m_controlsBg->setBrush(Qt::NoBrush);
    m_controlsBg->setZValue(100.0);
    m_controlsBg->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_controlsBg->setAcceptedMouseButtons(Qt::NoButton);

    m_playBtnRectItem = new RoundedRectItem(m_controlsBg);
    m_playBtnRectItem->setPen(Qt::NoPen);
    // Square background for the play button, same as filename label
    m_playBtnRectItem->setBrush(m_labelBg ? m_labelBg->brush() : QBrush(QColor(0,0,0,160)));
    m_playBtnRectItem->setZValue(101.0);
    m_playBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_playBtnRectItem->setAcceptedMouseButtons(Qt::NoButton);

    m_playIcon = new QGraphicsPathItem(m_playBtnRectItem);
    m_playIcon->setBrush(Qt::white);
    m_playIcon->setPen(Qt::NoPen);
    m_playIcon->setZValue(102.0);
    m_playIcon->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_playIcon->setAcceptedMouseButtons(Qt::NoButton);

    // Stop button (top row, square)
    m_stopBtnRectItem = new RoundedRectItem(m_controlsBg);
    m_stopBtnRectItem->setPen(Qt::NoPen);
    m_stopBtnRectItem->setBrush(m_labelBg ? m_labelBg->brush() : QBrush(QColor(0,0,0,160)));
    m_stopBtnRectItem->setZValue(101.0);
    m_stopBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_stopBtnRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_stopIcon = new QGraphicsPathItem(m_stopBtnRectItem);
    m_stopIcon->setBrush(Qt::white);
    m_stopIcon->setPen(Qt::NoPen);
    m_stopIcon->setZValue(102.0);
    m_stopIcon->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_stopIcon->setAcceptedMouseButtons(Qt::NoButton);

    // Repeat toggle button (top row, square)
    m_repeatBtnRectItem = new RoundedRectItem(m_controlsBg);
    m_repeatBtnRectItem->setPen(Qt::NoPen);
    m_repeatBtnRectItem->setBrush(m_labelBg ? m_labelBg->brush() : QBrush(QColor(0,0,0,160)));
    m_repeatBtnRectItem->setZValue(101.0);
    m_repeatBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_repeatBtnRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_repeatIcon = new QGraphicsPathItem(m_repeatBtnRectItem);
    m_repeatIcon->setBrush(Qt::white);
    m_repeatIcon->setPen(Qt::NoPen);
    m_repeatIcon->setZValue(102.0);
    m_repeatIcon->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_repeatIcon->setAcceptedMouseButtons(Qt::NoButton);

    // Mute toggle button (top row, square)
    m_muteBtnRectItem = new RoundedRectItem(m_controlsBg);
    m_muteBtnRectItem->setPen(Qt::NoPen);
    m_muteBtnRectItem->setBrush(m_labelBg ? m_labelBg->brush() : QBrush(QColor(0,0,0,160)));
    m_muteBtnRectItem->setZValue(101.0);
    m_muteBtnRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_muteBtnRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_muteIcon = new QGraphicsPathItem(m_muteBtnRectItem);
    m_muteIcon->setBrush(Qt::white);
    m_muteIcon->setPen(Qt::NoPen);
    m_muteIcon->setZValue(102.0);
    m_muteIcon->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_muteIcon->setAcceptedMouseButtons(Qt::NoButton);
    // Slash overlay for muted state
    m_muteSlashIcon = new QGraphicsPathItem(m_muteBtnRectItem);
    m_muteSlashIcon->setBrush(Qt::NoBrush);
    m_muteSlashIcon->setPen(QPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    m_muteSlashIcon->setZValue(103.0);
    m_muteSlashIcon->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_muteSlashIcon->setAcceptedMouseButtons(Qt::NoButton);

    // Volume slider (top row, remaining width)
    m_volumeBgRectItem = new QGraphicsRectItem(m_controlsBg);
    m_volumeBgRectItem->setPen(Qt::NoPen);
    m_volumeBgRectItem->setBrush(m_labelBg ? m_labelBg->brush() : QBrush(QColor(0,0,0,160)));
    m_volumeBgRectItem->setZValue(101.0);
    m_volumeBgRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_volumeBgRectItem->setAcceptedMouseButtons(Qt::NoButton);
    m_volumeFillRectItem = new QGraphicsRectItem(m_volumeBgRectItem);
    m_volumeFillRectItem->setPen(Qt::NoPen);
    m_volumeFillRectItem->setBrush(QColor(74,144,226));
    m_volumeFillRectItem->setZValue(102.0);
    m_volumeFillRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_volumeFillRectItem->setAcceptedMouseButtons(Qt::NoButton);

    m_progressBgRectItem = new QGraphicsRectItem(m_controlsBg);
    m_progressBgRectItem->setPen(Qt::NoPen);
    // Full-width background for the progress row, same as filename label
    m_progressBgRectItem->setBrush(m_labelBg ? m_labelBg->brush() : QBrush(QColor(0,0,0,160)));
    m_progressBgRectItem->setZValue(101.0);
    m_progressBgRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_progressBgRectItem->setAcceptedMouseButtons(Qt::NoButton);

    m_progressFillRectItem = new QGraphicsRectItem(m_progressBgRectItem);
    m_progressFillRectItem->setPen(Qt::NoPen);
    // Progress fill uses the primary accent color; keep solid for readability over translucent bg
    m_progressFillRectItem->setBrush(QColor(74,144,226));
    m_progressFillRectItem->setZValue(102.0);
    m_progressFillRectItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    m_progressFillRectItem->setAcceptedMouseButtons(Qt::NoButton);
    // Controls are hidden by default (only visible when the item is selected)
    if (m_controlsBg) m_controlsBg->setVisible(false);
    if (m_playBtnRectItem) m_playBtnRectItem->setVisible(false);
    if (m_playIcon) m_playIcon->setVisible(false);
    if (m_stopBtnRectItem) m_stopBtnRectItem->setVisible(false);
    if (m_stopIcon) m_stopIcon->setVisible(false);
    if (m_repeatBtnRectItem) m_repeatBtnRectItem->setVisible(false);
    if (m_repeatIcon) m_repeatIcon->setVisible(false);
    if (m_muteBtnRectItem) m_muteBtnRectItem->setVisible(false);
    if (m_muteIcon) m_muteIcon->setVisible(false);
    if (m_muteSlashIcon) m_muteSlashIcon->setVisible(false);
    if (m_volumeBgRectItem) m_volumeBgRectItem->setVisible(false);
    if (m_volumeFillRectItem) m_volumeFillRectItem->setVisible(false);
    if (m_progressBgRectItem) m_progressBgRectItem->setVisible(false);
    if (m_progressFillRectItem) m_progressFillRectItem->setVisible(false);
    }
    ~ResizableVideoItem() override {
    if (m_player) QObject::disconnect(m_player, nullptr, nullptr, nullptr);
    if (m_sink) QObject::disconnect(m_sink, nullptr, nullptr, nullptr);
        delete m_player; delete m_audio; delete m_sink;
    }
    void togglePlayPause() {
        if (!m_player) return;
    m_holdLastFrameAtEnd = false;
        if (m_player->playbackState() == QMediaPlayer::PlayingState) m_player->pause(); else m_player->play();
    }
    void stopToBeginning() {
        if (!m_player) return;
    m_holdLastFrameAtEnd = false;
        m_player->pause();
        m_player->setPosition(0);
    m_positionMs = 0;
    updateControlsLayout();
    update();
    }
    void toggleRepeat() {
    m_repeatEnabled = !m_repeatEnabled;
    // Refresh to update button background tint
    updateControlsLayout();
    update();
    }
    void toggleMute() {
        if (!m_audio) return;
    m_audio->setMuted(!m_audio->isMuted());
    // Ensure the icon updates immediately
    updateControlsLayout();
    update();
    }
    void seekToRatio(qreal r) {
        if (!m_player || m_durationMs <= 0) return;
        r = std::clamp<qreal>(r, 0.0, 1.0);
    m_holdLastFrameAtEnd = false;
    qint64 pos = static_cast<qint64>(r * m_durationMs);
    m_player->setPosition(pos);
    m_positionMs = pos;
    updateControlsLayout();
    update();
    }
    void setInitialScaleFactor(qreal f) { m_initialScaleFactor = f; }
    // Drag helpers for view-level coordination
    bool isDraggingProgress() const { return m_draggingProgress; }
    bool isDraggingVolume() const { return m_draggingVolume; }
    void updateDragWithScenePos(const QPointF& scenePos) {
        QPointF p = mapFromScene(scenePos);
        if (m_draggingProgress) {
            qreal r = (p.x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width();
            r = std::clamp<qreal>(r, 0.0, 1.0);
            m_holdLastFrameAtEnd = false;
            seekToRatio(r);
            if (m_durationMs > 0) m_positionMs = static_cast<qint64>(r * m_durationMs);
            updateControlsLayout();
            update();
        } else if (m_draggingVolume) {
            qreal r = (p.x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width();
            r = std::clamp<qreal>(r, 0.0, 1.0);
            if (m_audio) m_audio->setVolume(r);
            updateControlsLayout();
            update();
        }
    }
    void endDrag() {
        if (m_draggingProgress || m_draggingVolume) {
            m_draggingProgress = false;
            m_draggingVolume = false;
            ungrabMouse();
            updateControlsLayout();
            update();
        }
    }
    // Expose a helper for view-level control handling
    bool handleControlsPressAtItemPos(const QPointF& itemPos) {
        if (!isSelected()) return false;
        // Play button
    if (m_playBtnRectItemCoords.contains(itemPos)) { m_holdLastFrameAtEnd = false; togglePlayPause(); return true; }
        // Stop
    if (m_stopBtnRectItemCoords.contains(itemPos)) { m_holdLastFrameAtEnd = false; stopToBeginning(); return true; }
        // Repeat
        if (m_repeatBtnRectItemCoords.contains(itemPos)) { toggleRepeat(); return true; }
        // Mute
        if (m_muteBtnRectItemCoords.contains(itemPos)) { toggleMute(); return true; }
        // Progress bar
        if (m_progRectItemCoords.contains(itemPos)) {
            qreal r = (itemPos.x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width();
            m_holdLastFrameAtEnd = false;
            seekToRatio(r);
            // Begin drag-to-seek
            m_draggingProgress = true;
            grabMouse();
            return true;
        }
        // Volume slider
        if (m_volumeRectItemCoords.contains(itemPos)) {
            qreal r = (itemPos.x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width();
            r = std::clamp<qreal>(r, 0.0, 1.0);
            if (m_audio) m_audio->setVolume(r);
            updateControlsLayout(); update();
            // Begin drag-to-volume
            m_draggingVolume = true;
            grabMouse();
            return true;
        }
        return false;
    }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override {
        Q_UNUSED(option); Q_UNUSED(widget);
        // Draw current frame; otherwise if available, draw poster image; avoid black placeholder to prevent flicker
        QRectF br(0,0, baseWidth(), baseHeight());
        if (m_lastFrame.isValid()) {
            QImage img = m_lastFrame.toImage();
            if (!img.isNull()) {
                painter->drawImage(br, img);
            } else if (m_posterImageSet && !m_posterImage.isNull()) {
                // Fallback if conversion failed for some reason
                painter->drawImage(br, m_posterImage);
            }
        } else if (m_posterImageSet && !m_posterImage.isNull()) {
            painter->drawImage(br, m_posterImage);
        }
        // Update floating controls overlay (only relevant when selected)
    if (isSelected()) updateControlsLayout();
        // Selection/handles and label
        paintSelectionAndLabel(painter);
    }
    QRectF boundingRect() const override {
        QRectF br(0,0, baseWidth(), baseHeight());
        // Only extend for controls when selected
        if (isSelected()) {
            const int overrideH = ResizableMediaBase::getHeightOfMediaOverlaysPx();
            // Match label background height exactly when auto
            const qreal labelBgH = (m_labelBg ? m_labelBg->rect().height() : 0.0);
            const int padYpx = 4;
            const int fallbackH = (m_labelText ? static_cast<int>(std::round(m_labelText->boundingRect().height())) + 2*padYpx : 24);
            const int rowH = (overrideH > 0) ? overrideH : (labelBgH > 0 ? static_cast<int>(std::round(labelBgH)) : fallbackH);
            const int gapPx = 8; // outer gap (media-to-controls) and inner gap (between rows)
            qreal extra = toItemLengthFromPixels(rowH * 2 + 2 * gapPx); // two rows + outer+inner gap
            br.setHeight(br.height() + extra);
        }
        if (isSelected()) {
            qreal pad = toItemLengthFromPixels(m_selectionSize) / 2.0;
            br = br.adjusted(-pad, -pad, pad, pad);
        }
        return br;
    }
    QPainterPath shape() const override {
        QPainterPath p; p.addRect(QRectF(0,0, baseWidth(), baseHeight()));
        // controls clickable areas only when selected
        if (isSelected()) {
            if (!m_playBtnRectItemCoords.isNull()) p.addRect(m_playBtnRectItemCoords);
            if (!m_stopBtnRectItemCoords.isNull()) p.addRect(m_stopBtnRectItemCoords);
            if (!m_repeatBtnRectItemCoords.isNull()) p.addRect(m_repeatBtnRectItemCoords);
            if (!m_muteBtnRectItemCoords.isNull()) p.addRect(m_muteBtnRectItemCoords);
            if (!m_progRectItemCoords.isNull()) p.addRect(m_progRectItemCoords);
            if (!m_volumeRectItemCoords.isNull()) p.addRect(m_volumeRectItemCoords);
        }
        if (isSelected()) {
            const qreal s = toItemLengthFromPixels(m_selectionSize);
            QRectF br(0,0, baseWidth(), baseHeight());
            p.addRect(QRectF(br.topLeft() - QPointF(s/2,s/2), QSizeF(s,s)));
            p.addRect(QRectF(QPointF(br.right(), br.top()) - QPointF(s/2,s/2), QSizeF(s,s)));
            p.addRect(QRectF(QPointF(br.left(), br.bottom()) - QPointF(s/2,s/2), QSizeF(s,s)));
            p.addRect(QRectF(br.bottomRight() - QPointF(s/2,s/2), QSizeF(s,s)));
        }
        return p;
    }
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        // Translate to base for resize first
        m_activeHandle = hitTestHandle(event->pos());
        if (m_activeHandle != None) {
            m_fixedItemPoint = handlePoint(opposite(m_activeHandle));
            m_fixedScenePoint = mapToScene(m_fixedItemPoint);
            m_initialScale = scale();
            const qreal d = std::hypot(event->scenePos().x() - m_fixedScenePoint.x(), event->scenePos().y() - m_fixedScenePoint.y());
            m_initialGrabDist = (d > 1e-6) ? d : 1e-6;
            event->accept();
            return;
        }
        // Controls interactions using last computed rects (only when selected)
        if (isSelected() && m_playBtnRectItemCoords.contains(event->pos())) {
            togglePlayPause();
            event->accept();
            return;
        }
        if (isSelected() && m_stopBtnRectItemCoords.contains(event->pos())) { stopToBeginning(); event->accept(); return; }
        if (isSelected() && m_repeatBtnRectItemCoords.contains(event->pos())) { toggleRepeat(); event->accept(); return; }
        if (isSelected() && m_muteBtnRectItemCoords.contains(event->pos())) { toggleMute(); event->accept(); return; }
        if (isSelected() && m_progRectItemCoords.contains(event->pos())) {
            qreal r = (event->pos().x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width();
            seekToRatio(r);
            m_draggingProgress = true;
            grabMouse();
            event->accept();
            return;
        }
        if (isSelected() && m_volumeRectItemCoords.contains(event->pos())) {
            qreal r = (event->pos().x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width();
            r = std::clamp<qreal>(r, 0.0, 1.0);
            if (m_audio) m_audio->setVolume(r);
            updateControlsLayout(); update();
            m_draggingVolume = true;
            grabMouse();
            event->accept();
            return;
        }
        ResizableMediaBase::mousePressEvent(event);
    }
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override {
        // Treat double-clicks on controls the same as single clicks; keep selection
        if (isSelected()) {
            if (m_playBtnRectItemCoords.contains(event->pos())) {
                togglePlayPause();
                event->accept();
                return;
            }
            if (m_stopBtnRectItemCoords.contains(event->pos())) {
                stopToBeginning();
                event->accept();
                return;
            }
            if (m_repeatBtnRectItemCoords.contains(event->pos())) {
                toggleRepeat();
                event->accept();
                return;
            }
            if (m_muteBtnRectItemCoords.contains(event->pos())) {
                toggleMute();
                event->accept();
                return;
            }
            if (m_progRectItemCoords.contains(event->pos())) {
                qreal r = (event->pos().x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width();
                seekToRatio(r);
                event->accept();
                return;
            }
            if (m_volumeRectItemCoords.contains(event->pos())) {
                qreal r = (event->pos().x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width();
                r = std::clamp<qreal>(r, 0.0, 1.0);
                if (m_audio) m_audio->setVolume(r);
                event->accept();
                return;
            }
        }
        ResizableMediaBase::mouseDoubleClickEvent(event);
    }
protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override {
        if (change == ItemSelectedChange) {
            const bool willBeSelected = value.toBool();
            // Prepare for geometry change since bounding rect depends on selection
            prepareGeometryChange();
            setControlsVisible(willBeSelected);
        }
        if (change == ItemSelectedHasChanged) {
            // Recompute layout after selection toggles
            updateControlsLayout();
        }
        return ResizableMediaBase::itemChange(change, value);
    }

    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override {
        // Allow base to manage resize drag when active
        if (m_activeHandle != None) { ResizableMediaBase::mouseMoveEvent(event); return; }
        if (event->buttons() & Qt::LeftButton) {
            if (m_draggingProgress) {
                qreal r = (event->pos().x() - m_progRectItemCoords.left()) / m_progRectItemCoords.width();
                // Clamp and update both the player and local UI state for immediate feedback
                r = std::clamp<qreal>(r, 0.0, 1.0);
                seekToRatio(r);
                if (m_durationMs > 0) m_positionMs = static_cast<qint64>(r * m_durationMs);
                updateControlsLayout();
                update();
                event->accept();
                return;
            }
            if (m_draggingVolume) {
                qreal r = (event->pos().x() - m_volumeRectItemCoords.left()) / m_volumeRectItemCoords.width();
                r = std::clamp<qreal>(r, 0.0, 1.0);
                if (m_audio) m_audio->setVolume(r);
                updateControlsLayout();
                update();
                event->accept();
                return;
            }
        }
        ResizableMediaBase::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        if (m_draggingProgress || m_draggingVolume) {
            m_draggingProgress = false;
            m_draggingVolume = false;
            ungrabMouse();
            event->accept();
            return;
        }
        ResizableMediaBase::mouseReleaseEvent(event);
    }
private:
    void maybeAdoptFrameSize(const QVideoFrame& f) {
        if (m_adoptedSize) return;
        if (!f.isValid()) return;
        QImage img = f.toImage();
        if (img.isNull()) return;
        const QSize sz = img.size();
        if (sz.isEmpty()) return;
        adoptBaseSize(sz);
    }
    void adoptBaseSize(const QSize& sz) {
        if (m_adoptedSize) return;
        if (sz.isEmpty()) return;
        m_adoptedSize = true;
        // Preserve center in scene while changing base size and scale
        const QRectF oldRect(0,0, baseWidth(), baseHeight());
        const QPointF oldCenterScene = mapToScene(oldRect.center());
        prepareGeometryChange();
    m_baseSize = sz;
        setScale(m_initialScaleFactor);
        const QPointF newTopLeftScene = oldCenterScene - QPointF(sz.width() * m_initialScaleFactor / 2.0,
                                                                 sz.height() * m_initialScaleFactor / 2.0);
        setPos(newTopLeftScene);
        update();
    }
    void setControlsVisible(bool show) {
        if (m_controlsBg) m_controlsBg->setVisible(show);
        if (m_playBtnRectItem) m_playBtnRectItem->setVisible(show);
        if (m_playIcon) m_playIcon->setVisible(show);
    if (m_stopBtnRectItem) m_stopBtnRectItem->setVisible(show);
    if (m_stopIcon) m_stopIcon->setVisible(show);
    if (m_repeatBtnRectItem) m_repeatBtnRectItem->setVisible(show);
    if (m_repeatIcon) m_repeatIcon->setVisible(show);
    if (m_muteBtnRectItem) m_muteBtnRectItem->setVisible(show);
    if (m_muteIcon) m_muteIcon->setVisible(show);
    if (m_muteSlashIcon) m_muteSlashIcon->setVisible(show && m_audio && m_audio->isMuted());
    if (m_volumeBgRectItem) m_volumeBgRectItem->setVisible(show);
    if (m_volumeFillRectItem) m_volumeFillRectItem->setVisible(show);
        if (m_progressBgRectItem) m_progressBgRectItem->setVisible(show);
        if (m_progressFillRectItem) m_progressFillRectItem->setVisible(show);
    }
    void updateControlsLayout() {
        if (!scene() || scene()->views().isEmpty()) return;
        if (!isSelected()) return; // layout only needed when visible
        QGraphicsView* v = scene()->views().first();
    // Heights based on filename label height, or explicit override
    const int padYpx = 4;
    const int gapPx = 8;
    const int overrideH = ResizableMediaBase::getHeightOfMediaOverlaysPx();
    const qreal labelBgH = (m_labelBg ? m_labelBg->rect().height() : 0.0);
    const int fallbackH = (m_labelText ? static_cast<int>(std::round(m_labelText->boundingRect().height())) + 2*padYpx : 24);
    const int rowH = (overrideH > 0) ? overrideH : (labelBgH > 0 ? static_cast<int>(std::round(labelBgH)) : fallbackH);
    const int totalWpx = 260; // absolute width for controls overlay
    const int playWpx = rowH; // square button width equals height
    // Top row: play, stop, repeat, mute, then volume slider filling the rest, with gaps between buttons
    const int stopWpx = rowH;
    const int repeatWpx = rowH;
    const int muteWpx = rowH;
    const int buttonGap = gapPx; // horizontal gap between buttons equals outer/inner gap
    const int volumeWpx = std::max(0, totalWpx - (playWpx + stopWpx + repeatWpx + muteWpx) - buttonGap * 4);
    const int progWpx = totalWpx; // progress spans full width on second row

        // Active-state tinting: slightly blue-tinted version of label background
        auto baseBrush = (m_labelBg ? m_labelBg->brush() : QBrush(QColor(0,0,0,160)));
        auto blendColor = [](const QColor& a, const QColor& b, qreal t){
            auto clamp255 = [](int v){ return std::max(0, std::min(255, v)); };
            int r = clamp255(static_cast<int>(std::round(a.red()   * (1.0 - t) + b.red()   * t)));
            int g = clamp255(static_cast<int>(std::round(a.green() * (1.0 - t) + b.green() * t)));
            int bch = clamp255(static_cast<int>(std::round(a.blue()  * (1.0 - t) + b.blue()  * t)));
            return QColor(r, g, bch, a.alpha());
        };
        const QColor accentBlue(74, 144, 226, 255);
        const qreal tintStrength = 0.33; // subtle blue
        // Compute base color from brush (assume solid); fallback used otherwise
        QColor baseColor = baseBrush.color().isValid() ? baseBrush.color() : QColor(0,0,0,160);
        QBrush activeBrush(blendColor(baseColor, accentBlue, tintStrength));

        // Compute bottom-center of video in viewport coords
        QPointF bottomCenterItem(baseWidth()/2.0, baseHeight());
        QPointF bottomCenterScene = mapToScene(bottomCenterItem);
        QPointF bottomCenterView = v->mapFromScene(bottomCenterScene);
        // Desired top-left of controls in viewport (pixels)
        QPointF ctrlTopLeftView = bottomCenterView + QPointF(-totalWpx/2.0, gapPx);
        // Map back to item coordinates and set positions
        QPointF ctrlTopLeftScene = v->mapToScene(ctrlTopLeftView.toPoint());
        QPointF ctrlTopLeftItem = mapFromScene(ctrlTopLeftScene);
        if (m_controlsBg) {
            // Background tall enough to hold two rows (play above, progress below) with inner gap
            m_controlsBg->setRect(0, 0, totalWpx, rowH * 2 + gapPx);
            m_controlsBg->setPos(ctrlTopLeftItem);
        }
        // Compute x-positions with uniform gaps
        int x0 = 0;
        int x1 = x0 + playWpx + buttonGap;
        int x2 = x1 + stopWpx + buttonGap;
        int x3 = x2 + repeatWpx + buttonGap;
        int x4 = x3 + muteWpx + buttonGap;
        if (m_playBtnRectItem) {
            m_playBtnRectItem->setRect(0, 0, playWpx, rowH); m_playBtnRectItem->setPos(x0, 0);
            m_playBtnRectItem->setRadius(ResizableMediaBase::getCornerRadiusOfMediaOverlaysPx());
            // keep default base brush
            m_playBtnRectItem->setBrush(baseBrush);
        }
        if (m_stopBtnRectItem) {
            m_stopBtnRectItem->setRect(0, 0, stopWpx, rowH); m_stopBtnRectItem->setPos(x1, 0);
            m_stopBtnRectItem->setRadius(ResizableMediaBase::getCornerRadiusOfMediaOverlaysPx());
            m_stopBtnRectItem->setBrush(baseBrush);
        }
        if (m_repeatBtnRectItem) {
            m_repeatBtnRectItem->setRect(0, 0, repeatWpx, rowH); m_repeatBtnRectItem->setPos(x2, 0);
            m_repeatBtnRectItem->setRadius(ResizableMediaBase::getCornerRadiusOfMediaOverlaysPx());
            m_repeatBtnRectItem->setBrush(m_repeatEnabled ? activeBrush : baseBrush);
        }
        if (m_muteBtnRectItem) {
            m_muteBtnRectItem->setRect(0, 0, muteWpx, rowH); m_muteBtnRectItem->setPos(x3, 0);
            m_muteBtnRectItem->setRadius(ResizableMediaBase::getCornerRadiusOfMediaOverlaysPx());
            bool muted = m_audio && m_audio->isMuted();
            m_muteBtnRectItem->setBrush(muted ? activeBrush : baseBrush);
        }
        if (m_volumeBgRectItem) { m_volumeBgRectItem->setRect(0, 0, volumeWpx, rowH); m_volumeBgRectItem->setPos(x4, 0); }
        if (m_volumeFillRectItem) {
            const qreal margin = 2.0;
            qreal vol = m_audio ? std::clamp<qreal>(m_audio->volume(), 0.0, 1.0) : 0.0;
            const qreal innerW = std::max<qreal>(0.0, volumeWpx - 2*margin);
            m_volumeFillRectItem->setRect(margin, margin, innerW * vol, rowH - 2*margin);
        }
        if (m_progressBgRectItem) {
            // Second row, spans left to right under play button, with a gap
            m_progressBgRectItem->setRect(0, 0, progWpx, rowH);
            m_progressBgRectItem->setPos(0, rowH + gapPx);
        }
    if (m_progressFillRectItem) {
            qreal ratio = (m_durationMs > 0) ? (static_cast<qreal>(m_positionMs) / m_durationMs) : 0.0;
            ratio = std::clamp<qreal>(ratio, 0.0, 1.0);
            const qreal margin = 2.0;
            m_progressFillRectItem->setRect(margin, margin, (progWpx - 2*margin) * ratio, rowH - 2*margin);
        }
    // Update icon shapes and center them in their squares
        if (m_playIcon) {
            if (m_player && m_player->playbackState() == QMediaPlayer::PlayingState) {
                // Pause icon: two bars
                qreal w = playWpx;
                qreal h = rowH;
                qreal barW = w * 0.25; qreal gap = w * 0.15;
                QRectF leftBar(0, 0, barW, h*0.6);
                leftBar.moveTopLeft(QPointF(0, h*0.2));
                QRectF rightBar(leftBar.adjusted(barW + gap, 0, barW + gap, 0));
                QPainterPath path;
                path.addRect(leftBar);
                path.addRect(rightBar);
                // Center the path within the play button square
                QRectF bb = path.boundingRect();
                QPointF delta((w - bb.width())/2.0 - bb.left(), (h - bb.height())/2.0 - bb.top());
                QTransform t; t.translate(delta.x(), delta.y());
                m_playIcon->setPath(t.map(path));
            } else {
                // Play icon: triangle
                qreal w = playWpx;
                qreal h = rowH;
                QPolygonF poly;
                poly << QPointF(0, h*0.2)
                     << QPointF(0, h*0.8)
                     << QPointF(w*0.6, h*0.5);
                QPainterPath path; path.addPolygon(poly); path.closeSubpath();
                // Center the path within the play button square
                QRectF bb = path.boundingRect();
                QPointF delta((w - bb.width())/2.0 - bb.left(), (h - bb.height())/2.0 - bb.top());
                QTransform t; t.translate(delta.x(), delta.y());
                m_playIcon->setPath(t.map(path));
            }
        }
        // Stop icon: square
        if (m_stopIcon) {
            qreal w = stopWpx, h = rowH;
            QRectF sq(0, 0, w*0.5, h*0.5);
            QPainterPath path; path.addRect(sq);
            QRectF bb = path.boundingRect();
            QPointF delta((w - bb.width())/2.0 - bb.left(), (h - bb.height())/2.0 - bb.top());
            QTransform t; t.translate(delta.x(), delta.y());
            m_stopIcon->setPath(t.map(path));
        }
        // Repeat icon: classic circular loop arrow
        if (m_repeatIcon) {
            qreal w = repeatWpx, h = rowH;
            qreal cx = w * 0.5;
            qreal cy = h * 0.5;
            qreal r = std::min(w, h) * 0.33;
            qreal thick = std::max<qreal>(1.0, h * 0.12);
            qreal startDeg = 30.0;   // start angle (deg)
            qreal spanDeg  = 300.0;  // sweep almost full circle
            qreal trimDeg  = -8.0;    // trim to avoid overshoot under the arrowhead
            qreal effSpan  = spanDeg - trimDeg;
            qreal endDeg   = startDeg + effSpan;
            QRectF circleRect(cx - r, cy - r, 2*r, 2*r);
            // Thin arc path
            QPainterPath arc;
            arc.arcMoveTo(circleRect, startDeg);
            arc.arcTo(circleRect, startDeg, effSpan);
            // Thicken arc to a filled ring segment
            QPainterPathStroker stroker;
            stroker.setWidth(thick);
            stroker.setJoinStyle(Qt::RoundJoin);
            stroker.setCapStyle(Qt::FlatCap);
            QPainterPath ring = stroker.createStroke(arc);
            // Arrow head at the end of arc; align to tangent and place at outer edge
            const qreal PI = 3.14159265358979323846;
            qreal rad = endDeg * PI / 180.0;
            // Tangent for increasing angle (counterclockwise): (-sin, cos)
            QPointF dir(-std::sin(rad), std::cos(rad));
            qreal len = std::hypot(dir.x(), dir.y()); if (len > 0) dir /= len;
            // Rotate arrowhead 45° to the left (CCW) relative to the previous right-rotated direction
            const qreal a = -45.0 * (3.14159265358979323846 / 180.0); // -45° (clockwise) from tangent → visually left from prior state
            qreal ca = std::cos(a), sa = std::sin(a);
            QPointF dirHead(dir.x()*ca - dir.y()*sa, dir.x()*sa + dir.y()*ca);
            len = std::hypot(dirHead.x(), dirHead.y()); if (len > 0) dirHead /= len;
            QPointF perpHead(-dirHead.y(), dirHead.x());
            QPointF radial(std::cos(rad), std::sin(rad));
            QPointF tip(cx + (r + thick * 0.5) * radial.x(), cy + (r + thick * 0.5) * radial.y());
            // Double the arrowhead size
            qreal headLen = std::max(thick * 2.4, r * 0.70);
            qreal headWide = thick * 1.8;
            QPointF base = tip - dirHead * headLen;
            QPolygonF headPoly;
            headPoly << tip << (base + perpHead * headWide) << (base - perpHead * headWide);
            QPainterPath path = ring;
            path.addPolygon(headPoly);
            // Center path within square
            QRectF bb = path.boundingRect();
            QPointF delta((w - bb.width())/2.0 - bb.left(), (h - bb.height())/2.0 - bb.top());
            QTransform t; t.translate(delta.x(), delta.y());
            m_repeatIcon->setPath(t.map(path));
        }
        // Mute icon: speaker only; slash drawn as separate overlay when muted
        if (m_muteIcon) {
            qreal w = muteWpx, h = rowH;
            QPainterPath path;
            QRectF box(w*0.2, h*0.35, w*0.2, h*0.3);
            path.addRect(box);
            QPolygonF horn; horn << QPointF(box.right(), box.top()) << QPointF(w*0.6, h*0.2) << QPointF(w*0.6, h*0.8) << QPointF(box.right(), box.bottom());
            path.addPolygon(horn);
            QRectF bb = path.boundingRect();
            QPointF delta((w - bb.width())/2.0 - bb.left(), (h - bb.height())/2.0 - bb.top());
            QTransform t; t.translate(delta.x(), delta.y());
            m_muteIcon->setPath(t.map(path));
        }
        // Update mute slash overlay path and visibility
        if (m_muteSlashIcon) {
            qreal w = muteWpx, h = rowH;
            QPainterPath slash;
            slash.moveTo(w*0.2, h*0.2);
            slash.lineTo(w*0.8, h*0.8);
            // center to button square using same translation as icon
            QRectF bb = slash.boundingRect();
            QPointF delta((w - bb.width())/2.0 - bb.left(), (h - bb.height())/2.0 - bb.top());
            QTransform t; t.translate(delta.x(), delta.y());
            m_muteSlashIcon->setPath(t.map(slash));
            m_muteSlashIcon->setVisible(m_audio && m_audio->isMuted());
        }
        // Store item-space rects for hit testing on the parent
        // Map viewport-space rectangles back to item coords
    QRectF playView(ctrlTopLeftView, QSizeF(playWpx, rowH));
    QRectF stopView(ctrlTopLeftView + QPointF(playWpx + buttonGap, 0), QSizeF(stopWpx, rowH));
    QRectF repeatView(ctrlTopLeftView + QPointF(playWpx + buttonGap + stopWpx + buttonGap, 0), QSizeF(repeatWpx, rowH));
    QRectF muteView(ctrlTopLeftView + QPointF(playWpx + buttonGap + stopWpx + buttonGap + repeatWpx + buttonGap, 0), QSizeF(muteWpx, rowH));
    QRectF volumeView(ctrlTopLeftView + QPointF(playWpx + buttonGap + stopWpx + buttonGap + repeatWpx + buttonGap + muteWpx + buttonGap, 0), QSizeF(volumeWpx, rowH));
    QRectF progView(ctrlTopLeftView + QPointF(0, rowH + gapPx), QSizeF(progWpx, rowH));
        QRectF playScene(v->mapToScene(playView.toRect()).boundingRect());
    QRectF stopScene(v->mapToScene(stopView.toRect()).boundingRect());
    QRectF repeatScene(v->mapToScene(repeatView.toRect()).boundingRect());
    QRectF muteScene(v->mapToScene(muteView.toRect()).boundingRect());
    QRectF volumeScene(v->mapToScene(volumeView.toRect()).boundingRect());
        QRectF progScene(v->mapToScene(progView.toRect()).boundingRect());
        m_playBtnRectItemCoords = mapFromScene(playScene).boundingRect();
    m_stopBtnRectItemCoords = mapFromScene(stopScene).boundingRect();
    m_repeatBtnRectItemCoords = mapFromScene(repeatScene).boundingRect();
    m_muteBtnRectItemCoords = mapFromScene(muteScene).boundingRect();
    m_volumeRectItemCoords = mapFromScene(volumeScene).boundingRect();
        m_progRectItemCoords = mapFromScene(progScene).boundingRect();
    }
    qreal baseWidth() const { return static_cast<qreal>(m_baseSize.width()); }
    qreal baseHeight() const { return static_cast<qreal>(m_baseSize.height()); }
    QMediaPlayer* m_player = nullptr;
    QAudioOutput* m_audio = nullptr;
    QVideoSink* m_sink = nullptr;
    QVideoFrame m_lastFrame;
    qint64 m_durationMs = 0;
    qint64 m_positionMs = 0;
    // First-frame priming state
    bool m_primingFirstFrame = false;
    bool m_firstFramePrimed = false;
    bool m_savedMuted = false;
    // Optional poster image from metadata to avoid initial black frame
    QImage m_posterImage;
    bool m_posterImageSet = false;
    // Floating controls (absolute px)
    QGraphicsRectItem* m_controlsBg = nullptr;
    RoundedRectItem* m_playBtnRectItem = nullptr;
    QGraphicsPathItem* m_playIcon = nullptr; // path supports both triangle and bars
    RoundedRectItem* m_stopBtnRectItem = nullptr;
    QGraphicsPathItem* m_stopIcon = nullptr;
    RoundedRectItem* m_repeatBtnRectItem = nullptr;
    QGraphicsPathItem* m_repeatIcon = nullptr;
    RoundedRectItem* m_muteBtnRectItem = nullptr;
    QGraphicsPathItem* m_muteIcon = nullptr;
    QGraphicsPathItem* m_muteSlashIcon = nullptr;
    QGraphicsRectItem* m_volumeBgRectItem = nullptr;
    QGraphicsRectItem* m_volumeFillRectItem = nullptr;
    QGraphicsRectItem* m_progressBgRectItem = nullptr;
    QGraphicsRectItem* m_progressFillRectItem = nullptr;
    bool m_adoptedSize = false;
    qreal m_initialScaleFactor = 1.0;
    // Cached item-space rects for hit-testing
    QRectF m_playBtnRectItemCoords;
    QRectF m_stopBtnRectItemCoords;
    QRectF m_repeatBtnRectItemCoords;
    QRectF m_muteBtnRectItemCoords;
    QRectF m_volumeRectItemCoords;
    QRectF m_progRectItemCoords;
    bool m_repeatEnabled = false;
    // Drag state for sliders
    bool m_draggingProgress = false;
    bool m_draggingVolume = false;
    // Hold last frame after EndOfMedia until next user action
    bool m_holdLastFrameAtEnd = false;
};

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_centralWidget(nullptr)
    , m_webSocketClient(new WebSocketClient(this)) // Initialize WebSocket client
    , m_statusUpdateTimer(new QTimer(this))
    , m_trayIcon(nullptr)
    , m_screenViewWidget(nullptr)
    , m_screenCanvas(nullptr)
    , m_ignoreSelectionChange(false)
    , m_displaySyncTimer(new QTimer(this))
    , m_canvasStack(new QStackedWidget(this)) // Initialize m_canvasStack
{
    // Check if system tray is available
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(this, "System Tray",
                             "System tray is not available on this system.");
    }
    
    setupUI();
    setupMenuBar();
    setupSystemTray();
    setupVolumeMonitoring();
    
    // Connect WebSocket signals
    connect(m_webSocketClient, &WebSocketClient::connected, this, &MainWindow::onConnected);
    connect(m_webSocketClient, &WebSocketClient::disconnected, this, &MainWindow::onDisconnected);
    connect(m_webSocketClient, &WebSocketClient::clientListReceived, this, &MainWindow::onClientListReceived);
    connect(m_webSocketClient, &WebSocketClient::registrationConfirmed, this, &MainWindow::onRegistrationConfirmed);
    connect(m_webSocketClient, &WebSocketClient::screensInfoReceived, this, &MainWindow::onScreensInfoReceived);
    connect(m_webSocketClient, &WebSocketClient::watchStatusChanged, this, &MainWindow::onWatchStatusChanged);
    connect(m_webSocketClient, &WebSocketClient::dataRequestReceived, this, [this]() {
        // Server requested immediate state (on first watch or refresh)
        m_webSocketClient->sendStateSnapshot(getLocalScreenInfo(), getSystemVolumePercent());
    });
    
    // Setup status update timer
    m_statusUpdateTimer->setInterval(1000); // Update every second
    connect(m_statusUpdateTimer, &QTimer::timeout, this, &MainWindow::updateConnectionStatus);
    m_statusUpdateTimer->start();

    // Debounced display sync timer
    m_displaySyncTimer->setSingleShot(true);
    m_displaySyncTimer->setInterval(300); // debounce bursts of screen change signals
    connect(m_displaySyncTimer, &QTimer::timeout, this, [this]() {
        if (m_webSocketClient->isConnected() && m_isWatched) syncRegistration();
    });

    // Listen to display changes to keep server-side screen info up-to-date
    auto connectScreenSignals = [this](QScreen* s) {
        connect(s, &QScreen::geometryChanged, this, [this]() { m_displaySyncTimer->start(); });
        connect(s, &QScreen::availableGeometryChanged, this, [this]() { m_displaySyncTimer->start(); });
        connect(s, &QScreen::physicalDotsPerInchChanged, this, [this]() { m_displaySyncTimer->start(); });
        connect(s, &QScreen::primaryOrientationChanged, this, [this]() { m_displaySyncTimer->start(); });
    };
    for (QScreen* s : QGuiApplication::screens()) connectScreenSignals(s);
    connect(qApp, &QGuiApplication::screenAdded, this, [this, connectScreenSignals](QScreen* s) {
        connectScreenSignals(s);
        m_displaySyncTimer->start();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen*) {
        m_displaySyncTimer->start();
    });

    // Smart reconnection system with exponential backoff
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    m_reconnectAttempts = 0;
    m_maxReconnectDelay = 60000; // Max 60 seconds between attempts
    connect(m_reconnectTimer, &QTimer::timeout, this, &MainWindow::attemptReconnect);

        // (cleaned) avoid duplicate hookups
#ifdef Q_OS_MACOS
    // Use a normal window on macOS so the title bar and traffic lights have standard size.
    // Avoid Qt::Tool/WA_MacAlwaysShowToolWindow which force a tiny utility-chrome window.
#endif
    
    // Initially disable UI until connected
    setUIEnabled(false);
    
    // Start minimized to tray and auto-connect
    hide();
    connectToServer();
}

void MainWindow::showScreenView(const ClientInfo& client) {
    qDebug() << "showScreenView called for client:" << client.getMachineName();
    // Update client name
    m_clientNameLabel->setText(QString("%1 (%2)").arg(client.getMachineName()).arg(client.getPlatform()));
    
    // Reset to spinner state but delay showing the spinner to avoid flicker
    if (m_canvasStack) m_canvasStack->setCurrentIndex(0);
    if (m_loadingSpinner) {
        m_loadingSpinner->stop();
        m_spinnerFade->stop();
        m_spinnerOpacity->setOpacity(0.0);
    }
    if (m_volumeIndicator) {
        m_volumeIndicator->hide();
        m_volumeFade->stop();
        m_volumeOpacity->setOpacity(0.0);
    }
    if (m_canvasFade) m_canvasFade->stop();
    if (m_canvasOpacity) m_canvasOpacity->setOpacity(0.0);
    
    if (m_loaderDelayTimer) {
        m_loaderDelayTimer->stop();
        // Prevent multiple connections across navigations
        QObject::disconnect(m_loaderDelayTimer, nullptr, nullptr, nullptr);
        connect(m_loaderDelayTimer, &QTimer::timeout, this, [this]() {
            if (!m_canvasStack || !m_loadingSpinner || !m_spinnerFade || !m_spinnerOpacity) return;
            m_canvasStack->setCurrentIndex(0);
            m_spinnerFade->stop();
            m_spinnerOpacity->setOpacity(0.0);
            m_loadingSpinner->start();
            // Spinner uses its own fade duration
            if (m_spinnerFade) m_spinnerFade->setDuration(m_loaderFadeDurationMs);
            m_spinnerFade->setStartValue(0.0);
            m_spinnerFade->setEndValue(1.0);
            m_spinnerFade->start();
        });
        m_loaderDelayTimer->start(m_loaderDelayMs);
    }
    if (m_screenCanvas) {
        m_screenCanvas->clearScreens();
    }
    
    // Request fresh screen info on demand
    if (!client.getId().isEmpty()) {
        m_webSocketClient->requestScreens(client.getId());
    }
    
    // Switch to screen view page
    m_stackedWidget->setCurrentWidget(m_screenViewWidget);
    // Show back button when on screen view
    if (m_backButton) m_backButton->show();
    
    // Start watching this client's screens for real-time updates
    if (m_webSocketClient && m_webSocketClient->isConnected()) {
        if (!m_watchedClientId.isEmpty() && m_watchedClientId != client.getId()) {
            m_webSocketClient->unwatchScreens(m_watchedClientId);
        }
        if (!client.getId().isEmpty()) {
            m_webSocketClient->watchScreens(client.getId());
            m_watchedClientId = client.getId();
        }
    }
    qDebug() << "Screen view now showing. Current widget index:" << m_stackedWidget->currentIndex();
}

void MainWindow::showClientListView() {
    qDebug() << "showClientListView called. Current widget index before:" << m_stackedWidget->currentIndex();
    
    // Switch to client list page
    m_stackedWidget->setCurrentWidget(m_clientListPage);
    // Hide back button when on client list
    if (m_backButton) m_backButton->hide();
    
    qDebug() << "Client list view now showing. Current widget index:" << m_stackedWidget->currentIndex();
    
    // Stop watching when leaving screen view
    if (m_webSocketClient && m_webSocketClient->isConnected() && !m_watchedClientId.isEmpty()) {
        m_webSocketClient->unwatchScreens(m_watchedClientId);
        m_watchedClientId.clear();
    }
    if (m_screenCanvas) m_screenCanvas->hideRemoteCursor();
    // Clear selection without triggering selection change event
    m_ignoreSelectionChange = true;
    m_clientListWidget->clearSelection();
    m_ignoreSelectionChange = false;
}

void MainWindow::onClientItemClicked(QListWidgetItem* item) {
    if (!item) return;
    int index = m_clientListWidget->row(item);
    if (index >= 0 && index < m_availableClients.size()) {
        const ClientInfo& client = m_availableClients[index];
        m_selectedClient = client;
        // Switch to screen view first with any cached info for immediate feedback
        showScreenView(client);
        // Then request fresh screens info on-demand
        if (m_webSocketClient && m_webSocketClient->isConnected()) {
            m_webSocketClient->requestScreens(client.getId());
        }
    }
}

void MainWindow::updateVolumeIndicator() {
    int vol = m_selectedClient.getVolumePercent();
    QString text;
    if (vol >= 0) {
        QString icon = (vol == 0) ? "🔇" : (vol < 34 ? "🔈" : (vol < 67 ? "🔉" : "🔊"));
        text = QString("%1 %2%").arg(icon).arg(vol);
    } else {
        text = QString("🔈 --");
    }
    m_volumeIndicator->setText(text);
}

void MainWindow::onBackToClientListClicked() {
    showClientListView();
}

void MainWindow::onSendMediaClicked() {
    // Placeholder implementation
    QMessageBox::information(this, "Send Media", 
        QString("Sending media to %1's screens...\n\nThis feature will be implemented in the next phase.")
        .arg(m_selectedClient.getMachineName()));
}



bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    // Swallow spacebar outside of the canvas to prevent accidental activations,
    // but let it reach the canvas for the recenter shortcut.
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Space) {
            QWidget* w = qobject_cast<QWidget*>(obj);
            bool onCanvas = false;
            if (m_screenCanvas) {
                onCanvas = (w == m_screenCanvas) || (w && m_screenCanvas->isAncestorOf(w));
            }
            if (!onCanvas) {
                return true; // consume outside the canvas
            } else {
                // Recenter shortcut on canvas regardless of exact focus widget
                m_screenCanvas->recenterWithMargin(33);
                return true; // consume
            }
        }
    }
    // Apply rounded clipping to canvas widgets so border radius is visible
    if (obj == m_canvasContainer || obj == m_canvasStack || (m_screenCanvas && obj == m_screenCanvas->viewport())) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
            if (QWidget* w = qobject_cast<QWidget*>(obj)) {
                const int r = 5; // match clients list radius
                QPainterPath path;
                path.addRoundedRect(w->rect(), r, r);
                QRegion mask(path.toFillPolygon().toPolygon());
                w->setMask(mask);
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// ScreenCanvas implementation
ScreenCanvas::ScreenCanvas(QWidget* parent)
    : QGraphicsView(parent)
    , m_scene(new QGraphicsScene(this))
    , m_panning(false)
{
    setScene(m_scene);
    setDragMode(QGraphicsView::NoDrag);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Use the application palette for consistent theming with the client list container
    const QBrush bgBrush = palette().brush(QPalette::Base);
    setBackgroundBrush(bgBrush);        // outside the scene rect
    if (m_scene) m_scene->setBackgroundBrush(bgBrush); // inside the scene rect
    setFrameShape(QFrame::NoFrame);
    setRenderHint(QPainter::Antialiasing);
    // Use manual anchoring logic for consistent behavior across platforms
    setTransformationAnchor(QGraphicsView::NoAnchor);
    // When the view is resized, keep the view-centered anchor (we also recenter explicitly on window resize)
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    // Enable drag & drop
    setAcceptDrops(true);
    
    // Enable mouse tracking for panning
    setMouseTracking(true);
    m_lastMousePos = QPoint(0, 0);
    // Pinch guard timer prevents two-finger scroll handling during active pinch
    m_nativePinchGuardTimer = new QTimer(this);
    m_nativePinchGuardTimer->setSingleShot(true);
    m_nativePinchGuardTimer->setInterval(60); // short grace period after last pinch delta
    connect(m_nativePinchGuardTimer, &QTimer::timeout, this, [this]() {
        m_nativePinchActive = false;
    });
    
    // Enable pinch gesture (except on macOS where we'll rely on NativeGesture to avoid double handling)
#ifndef Q_OS_MACOS
    grabGesture(Qt::PinchGesture);
#endif
    // Remote cursor overlay (hidden by default)
    m_remoteCursorDot = m_scene->addEllipse(0, 0, 10, 10, QPen(QColor(74, 144, 226)), QBrush(Qt::white));
    if (m_remoteCursorDot) {
        m_remoteCursorDot->setZValue(10000);
        m_remoteCursorDot->setVisible(false);
    }
}

void ScreenCanvas::setScreens(const QList<ScreenInfo>& screens) {
    m_screens = screens;
    clearScreens();
    createScreenItems();
    
    // Keep a large scene for free movement and center on screens
    const double LARGE_SCENE_SIZE = 100000.0;
    QRectF sceneRect(-LARGE_SCENE_SIZE/2, -LARGE_SCENE_SIZE/2, LARGE_SCENE_SIZE, LARGE_SCENE_SIZE);
    m_scene->setSceneRect(sceneRect);
    if (!m_screens.isEmpty()) {
        recenterWithMargin(53);
    }
}

void ScreenCanvas::clearScreens() {
    // Clear existing screen items
    for (QGraphicsRectItem* item : m_screenItems) {
        m_scene->removeItem(item);
        delete item;
    }
    m_screenItems.clear();
}

void ScreenCanvas::createScreenItems() {
    const double SCALE_FACTOR = m_scaleFactor; // Use member scale factor so drops match
    const double H_SPACING = 0.0;  // No horizontal gap between adjacent screens
    const double V_SPACING = 5.0;  // Keep a small vertical gap between rows
    
    // Calculate compact positioning
    QMap<int, QRectF> compactPositions = calculateCompactPositions(SCALE_FACTOR, H_SPACING, V_SPACING);
    
    for (int i = 0; i < m_screens.size(); ++i) {
        const ScreenInfo& screen = m_screens[i];
        QGraphicsRectItem* screenItem = createScreenItem(screen, i, compactPositions[i]);
        m_screenItems.append(screenItem);
        m_scene->addItem(screenItem);
    }
    ensureZOrder();
}

void ScreenCanvas::updateRemoteCursor(int globalX, int globalY) {
    if (!m_remoteCursorDot || m_screens.isEmpty() || m_screenItems.size() != m_screens.size()) {
        if (m_remoteCursorDot) m_remoteCursorDot->setVisible(false);
        return;
    }
    int idx = -1;
    int localX = 0, localY = 0;
    for (int i = 0; i < m_screens.size(); ++i) {
        const auto& s = m_screens[i];
        if (globalX >= s.x && globalX < s.x + s.width && globalY >= s.y && globalY < s.y + s.height) {
            idx = i;
            localX = globalX - s.x;
            localY = globalY - s.y;
            break;
        }
    }
    if (idx < 0) {
        m_remoteCursorDot->setVisible(false);
        return;
    }
    QGraphicsRectItem* item = m_screenItems[idx];
    if (!item) { m_remoteCursorDot->setVisible(false); return; }
    const QRectF r = item->rect();
    if (m_screens[idx].width <= 0 || m_screens[idx].height <= 0 || r.width() <= 0 || r.height() <= 0) { m_remoteCursorDot->setVisible(false); return; }
    const double fx = static_cast<double>(localX) / static_cast<double>(m_screens[idx].width);
    const double fy = static_cast<double>(localY) / static_cast<double>(m_screens[idx].height);
    const double sceneX = r.left() + fx * r.width();
    const double sceneY = r.top() + fy * r.height();
    const double dotSize = 10.0;
    m_remoteCursorDot->setRect(sceneX - dotSize/2.0, sceneY - dotSize/2.0, dotSize, dotSize);
    m_remoteCursorDot->setVisible(true);
}

void ScreenCanvas::hideRemoteCursor() {
    if (m_remoteCursorDot) m_remoteCursorDot->setVisible(false);
}

void ScreenCanvas::ensureZOrder() {
    // Put screens at a low Z, remote cursor very high, media in between
    for (QGraphicsRectItem* r : m_screenItems) {
        if (r) r->setZValue(-100.0);
    }
    if (m_remoteCursorDot) m_remoteCursorDot->setZValue(10000.0);
}

void ScreenCanvas::setMediaHandleSelectionSizePx(int px) {
    m_mediaHandleSelectionSizePx = qMax(4, px);
    if (!m_scene) return;
    for (QGraphicsItem* it : m_scene->items()) {
        if (auto* rp = dynamic_cast<ResizablePixmapItem*>(it)) {
            rp->setHandleSelectionSize(m_mediaHandleSelectionSizePx);
        } else if (auto* rv = dynamic_cast<ResizableMediaBase*>(it)) {
            rv->setHandleSelectionSize(m_mediaHandleSelectionSizePx);
        }
    }
}

void ScreenCanvas::setMediaHandleVisualSizePx(int px) {
    m_mediaHandleVisualSizePx = qMax(4, px);
    if (!m_scene) return;
    for (QGraphicsItem* it : m_scene->items()) {
        if (auto* rp = dynamic_cast<ResizablePixmapItem*>(it)) {
            rp->setHandleVisualSize(m_mediaHandleVisualSizePx);
        } else if (auto* rv = dynamic_cast<ResizableMediaBase*>(it)) {
            rv->setHandleVisualSize(m_mediaHandleVisualSizePx);
        }
    }
}

void ScreenCanvas::setMediaHandleSizePx(int px) {
    setMediaHandleVisualSizePx(px);
    setMediaHandleSelectionSizePx(px);
}

// Drag & drop handlers
void ScreenCanvas::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls() || event->mimeData()->hasImage()) {
        event->acceptProposedAction();
    } else {
        QGraphicsView::dragEnterEvent(event);
    }
}

void ScreenCanvas::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasUrls() || event->mimeData()->hasImage()) {
        event->acceptProposedAction();
    } else {
        QGraphicsView::dragMoveEvent(event);
    }
}

void ScreenCanvas::dropEvent(QDropEvent* event) {
    QImage image;
    QString filename;
    QString droppedPath;
    if (event->mimeData()->hasImage()) {
        image = qvariant_cast<QImage>(event->mimeData()->imageData());
        filename = "pasted-image";
    } else if (event->mimeData()->hasUrls()) {
        const auto urls = event->mimeData()->urls();
        if (!urls.isEmpty()) {
            const QUrl& url = urls.first();
            const QString path = url.toLocalFile();
            if (!path.isEmpty()) {
                droppedPath = path;
                filename = QFileInfo(path).fileName();
                image.load(path); // may fail if it's a video
            }
        }
    }
    const QPointF scenePos = mapToScene(event->position().toPoint());
    if (!image.isNull()) {
        const double w = image.width() * m_scaleFactor;
        const double h = image.height() * m_scaleFactor;
        auto* item = new ResizablePixmapItem(QPixmap::fromImage(image), m_mediaHandleVisualSizePx, m_mediaHandleSelectionSizePx, filename);
        item->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemSendsGeometryChanges);
        item->setPos(scenePos.x() - w/2.0, scenePos.y() - h/2.0);
        if (image.width() > 0) item->setScale(w / image.width());
        m_scene->addItem(item);
    } else if (!droppedPath.isEmpty()) {
        // Decide if it's a video by extension
        const QString ext = QFileInfo(droppedPath).suffix().toLower();
        static const QSet<QString> kVideoExts = {"mp4","mov","m4v","avi","mkv","webm"};
        if (kVideoExts.contains(ext)) {
            // Create with placeholder logical size; adopt real size on first frame
            auto* vitem = new ResizableVideoItem(droppedPath, m_mediaHandleVisualSizePx, m_mediaHandleSelectionSizePx, filename);
            vitem->setInitialScaleFactor(m_scaleFactor);
            // Start with a neutral 1.0 scale on placeholder size and center approx.
            vitem->setScale(m_scaleFactor);
            const double w = 640.0 * m_scaleFactor;
            const double h = 360.0 * m_scaleFactor;
            vitem->setPos(scenePos.x() - w/2.0, scenePos.y() - h/2.0);
            m_scene->addItem(vitem);
        } else {
            QGraphicsView::dropEvent(event);
            return;
        }
    } else {
        QGraphicsView::dropEvent(event);
        return;
    }
    ensureZOrder();
    event->acceptProposedAction();
}

QGraphicsRectItem* ScreenCanvas::createScreenItem(const ScreenInfo& screen, int index, const QRectF& position) {
    // Border must be fully inside so the outer size matches the logical screen size.
    const int penWidth = m_screenBorderWidthPx; // configurable
    // Inset the rect by half the pen width so the stroke stays entirely inside.
    QRectF inner = position.adjusted(penWidth / 2.0, penWidth / 2.0,
                                     -penWidth / 2.0, -penWidth / 2.0);
    QGraphicsRectItem* item = new QGraphicsRectItem(inner);
    
    // Set appearance
    if (screen.primary) {
        item->setBrush(QBrush(QColor(74, 144, 226, 180))); // Primary screen - blue
        item->setPen(QPen(QColor(74, 144, 226), penWidth));
    } else {
        item->setBrush(QBrush(QColor(80, 80, 80, 180))); // Secondary screen - gray
        item->setPen(QPen(QColor(160, 160, 160), penWidth));
    }
    
    // Store screen index for click handling
    item->setData(0, index);
    
    // Add screen label
    QGraphicsTextItem* label = new QGraphicsTextItem(QString("Screen %1\n%2×%3")
        .arg(index + 1)
        .arg(screen.width)
        .arg(screen.height));
    label->setDefaultTextColor(Qt::white);
    label->setFont(QFont("Arial", 12, QFont::Bold));
    
    // Center the label on the screen
    QRectF labelRect = label->boundingRect();
    QRectF screenRect = item->rect();
    label->setPos(screenRect.center() - labelRect.center());
    label->setParentItem(item);
    
    return item;
}

void ScreenCanvas::setScreenBorderWidthPx(int px) {
    m_screenBorderWidthPx = qMax(0, px);
    // Update existing screen items to keep the same outer size while drawing the stroke fully inside
    // We know m_screenItems aligns with m_screens by index after createScreenItems().
    if (!m_scene) return;
    for (int i = 0; i < m_screenItems.size() && i < m_screens.size(); ++i) {
        QGraphicsRectItem* item = m_screenItems[i];
        if (!item) continue;
        const int penW = m_screenBorderWidthPx;
        // Compute the outer rect from the current item rect by reversing the previous inset.
        // Previous inset may differ; reconstruct outer by expanding by old half-pen, then re-inset by new half-pen.
        // Since we don't track the old pen per-item, approximate using current pen width on the item.
        int oldPenW = static_cast<int>(item->pen().widthF());
        QRectF currentInner = item->rect();
        QRectF outer = currentInner.adjusted(-(oldPenW/2.0), -(oldPenW/2.0), (oldPenW/2.0), (oldPenW/2.0));
        QRectF newInner = outer.adjusted(penW/2.0, penW/2.0, -penW/2.0, -penW/2.0);
        item->setRect(newInner);
        QPen p = item->pen();
        p.setWidthF(penW);
        item->setPen(p);
    }
}

QMap<int, QRectF> ScreenCanvas::calculateCompactPositions(double scaleFactor, double hSpacing, double vSpacing) const {
    QMap<int, QRectF> positions;
    
    if (m_screens.isEmpty()) {
        return positions;
    }
    
    // Simple left-to-right, top-to-bottom compact layout
    
    // First, sort screens by their actual position (left to right, then top to bottom)
    QList<QPair<int, ScreenInfo>> screenPairs;
    for (int i = 0; i < m_screens.size(); ++i) {
        screenPairs.append(qMakePair(i, m_screens[i]));
    }
    
    // Sort by Y first (top to bottom), then by X (left to right)
    std::sort(screenPairs.begin(), screenPairs.end(), 
              [](const QPair<int, ScreenInfo>& a, const QPair<int, ScreenInfo>& b) {
                  if (qAbs(a.second.y - b.second.y) < 100) { // If roughly same height
                      return a.second.x < b.second.x; // Sort by X
                  }
                  return a.second.y < b.second.y; // Sort by Y
              });
    
    // Layout screens with compact positioning
    double currentX = 0;
    double currentY = 0;
    double rowHeight = 0;
    int lastY = INT_MIN;
    
    for (const auto& pair : screenPairs) {
        int index = pair.first;
        const ScreenInfo& screen = pair.second;
        
        double screenWidth = screen.width * scaleFactor;
        double screenHeight = screen.height * scaleFactor;
        
        // Start new row if Y position changed significantly
        if (lastY != INT_MIN && qAbs(screen.y - lastY) > 100) {
            currentX = 0;
            currentY += rowHeight + vSpacing;
            rowHeight = 0;
        }
        
        // Position the screen
        QRectF rect(currentX, currentY, screenWidth, screenHeight);
    positions[index] = rect;
        
        // Update for next screen
    currentX += screenWidth + hSpacing;
        rowHeight = qMax(rowHeight, screenHeight);
        lastY = screen.y;
    }
    
    return positions;
}

QRectF ScreenCanvas::screensBoundingRect() const {
    QRectF bounds;
    bool first = true;
    for (auto* item : m_screenItems) {
        if (!item) continue;
        QRectF r = item->sceneBoundingRect();
        if (first) { bounds = r; first = false; }
        else { bounds = bounds.united(r); }
    }
    return bounds;
}

void ScreenCanvas::recenterWithMargin(int marginPx) {
    QRectF bounds = screensBoundingRect();
    if (bounds.isNull() || !bounds.isValid()) return;
    // Fit content with a fixed pixel margin in the viewport
    const QSize vp = viewport() ? viewport()->size() : size();
    const qreal availW = static_cast<qreal>(vp.width())  - 2.0 * marginPx;
    const qreal availH = static_cast<qreal>(vp.height()) - 2.0 * marginPx;

    if (availW <= 1 || availH <= 1 || bounds.width() <= 0 || bounds.height() <= 0) {
        // Fallback to a simple fit if viewport is too small
        fitInView(bounds, Qt::KeepAspectRatio);
        centerOn(bounds.center());
        return;
    }

    const qreal sx = availW / bounds.width();
    const qreal sy = availH / bounds.height();
    const qreal s = std::min(sx, sy);

    // Apply the scale in one shot and center on content
    QTransform t;
    t.scale(s, s);
    setTransform(t);
    centerOn(bounds.center());
}

void ScreenCanvas::keyPressEvent(QKeyEvent* event) {
    // Delete selected media items
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (m_scene) {
            const QList<QGraphicsItem*> sel = m_scene->selectedItems();
            for (QGraphicsItem* it : sel) {
                if (auto* base = dynamic_cast<ResizableMediaBase*>(it)) {
                    base->ungrabMouse();
                    m_scene->removeItem(base);
                    delete base;
                }
            }
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Space) {
        recenterWithMargin(53);
        event->accept();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

bool ScreenCanvas::event(QEvent* event) {
    if (event->type() == QEvent::Gesture) {
#ifdef Q_OS_MACOS
        // On macOS, rely on QNativeGestureEvent for trackpad pinch to avoid duplicate scaling
        return false;
#else
        return gestureEvent(static_cast<QGestureEvent*>(event));
#endif
    }
    // Handle native gestures (macOS trackpad pinch)
    if (event->type() == QEvent::NativeGesture) {
        auto* ng = static_cast<QNativeGestureEvent*>(event);
        if (ng->gestureType() == Qt::ZoomNativeGesture) {
            // Mark pinch as active and extend guard
            m_nativePinchActive = true;
            m_nativePinchGuardTimer->start();
            // ng->value() is a delta; use exponential to convert to multiplicative factor
            const qreal factor = std::pow(2.0, ng->value());
            // Figma/Canva-style: anchor at the cursor position first
            QPoint vpPos = viewport()->mapFromGlobal(QCursor::pos());
            if (!viewport()->rect().contains(vpPos)) {
                // Fallback to gesture position (view coords -> viewport) then last mouse pos/center
                QPoint viewPos = ng->position().toPoint();
                vpPos = viewport()->mapFrom(this, viewPos);
                if (!viewport()->rect().contains(vpPos)) {
                    vpPos = m_lastMousePos.isNull() ? viewport()->rect().center() : m_lastMousePos;
                }
            }
            zoomAroundViewportPos(vpPos, factor);
            event->accept();
            return true;
        }
    }
    return QGraphicsView::event(event);
}

bool ScreenCanvas::gestureEvent(QGestureEvent* event) {
#ifdef Q_OS_MACOS
    Q_UNUSED(event);
    return false; // handled via NativeGesture on macOS
#endif
    if (QGesture* pinch = event->gesture(Qt::PinchGesture)) {
        auto* pinchGesture = static_cast<QPinchGesture*>(pinch);
        if (pinchGesture->changeFlags() & QPinchGesture::ScaleFactorChanged) {
            const qreal factor = pinchGesture->scaleFactor();
            const QPoint anchor = m_lastMousePos.isNull() ? viewport()->rect().center() : m_lastMousePos;
            zoomAroundViewportPos(anchor, factor);
        }
        event->accept();
        return true;
    }
    return false;
}

void ScreenCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // Try to start resize on the topmost media item whose handle contains the point
        // BUT only if the item is already selected
        const QPointF scenePos = mapToScene(event->pos());
        ResizableMediaBase* topHandleItem = nullptr;
        qreal topZ = -std::numeric_limits<qreal>::infinity();
        for (QGraphicsItem* it : m_scene->items()) {
            if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) {
                // Only allow resize if item is selected
                if (rp->isSelected() && rp->isOnHandleAtItemPos(rp->mapFromScene(scenePos))) {
                    if (rp->zValue() > topZ) { topZ = rp->zValue(); topHandleItem = rp; }
                }
            }
        }
        if (topHandleItem) {
            if (topHandleItem->beginResizeAtScenePos(scenePos)) {
                // Set resize cursor during active resize
                Qt::CursorShape cursor = topHandleItem->cursorForScenePos(scenePos);
                viewport()->setCursor(cursor);
                event->accept();
                return;
            }
        }
        // Decide based on item type under cursor: media (or its overlays) -> scene, screens/empty -> pan
        const QList<QGraphicsItem*> hitItems = items(event->pos());
        auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* {
            while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); }
            return nullptr;
        };
        ResizableMediaBase* mediaHit = nullptr;
        QGraphicsItem* rawHit = nullptr;
        for (QGraphicsItem* it : hitItems) { if ((mediaHit = toMedia(it))) { rawHit = it; break; } }
        if (mediaHit) {
            // If a media (or its overlay) is hit, ensure it remains selected and allow direct control interactions
            if (!mediaHit->isSelected()) {
                mediaHit->setSelected(true);
            }
            // Map click to item pos and let video handle controls
            if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaHit)) {
                const QPointF itemPos = v->mapFromScene(mapToScene(event->pos()));
                if (v->handleControlsPressAtItemPos(itemPos)) {
                    event->accept();
                    return;
                }
            }
            // Otherwise, let default behavior run (move/select media), but do not clear selection
            QGraphicsView::mousePressEvent(event);
            return;
        }

        // No item hit: allow interacting with controls of currently selected video items
        // This covers the case where the floating controls are positioned outside the item's hittable area
        for (QGraphicsItem* it : scene()->selectedItems()) {
            if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                const QPointF itemPos = v->mapFromScene(mapToScene(event->pos()));
                if (v->handleControlsPressAtItemPos(itemPos)) {
                    event->accept();
                    return;
                }
            }
        }
        // Otherwise start panning the view
        if (m_scene) {
            m_scene->clearSelection(); // single-click on empty space deselects items
        }
        m_panning = true;
        m_lastPanPoint = event->pos();
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void ScreenCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const QPointF scenePos = mapToScene(event->pos());
        const QList<QGraphicsItem*> hitItems = items(event->pos());
        auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* {
            while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); }
            return nullptr;
        };
        ResizableMediaBase* mediaHit = nullptr;
        for (QGraphicsItem* it : hitItems) { if ((mediaHit = toMedia(it))) break; }
        if (mediaHit) {
            if (!mediaHit->isSelected()) mediaHit->setSelected(true);
            if (auto* v = dynamic_cast<ResizableVideoItem*>(mediaHit)) {
                const QPointF itemPos = v->mapFromScene(scenePos);
                if (v->handleControlsPressAtItemPos(itemPos)) { event->accept(); return; }
            }
            // Let default double-click behavior (e.g., edit?) proceed without clearing selection
            QGraphicsView::mouseDoubleClickEvent(event);
            return;
        }
        // If no media hit, pass through
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void ScreenCanvas::mouseMoveEvent(QMouseEvent* event) {
    // Track last mouse pos in viewport coords (used for zoom anchoring)
    m_lastMousePos = event->pos();
    
    // PRIORITY: Check if we're over a resize handle and set cursor accordingly
    // This must be done BEFORE any other cursor management to ensure it's not overridden
    // BUT only if the item is selected!
    const QPointF scenePos = mapToScene(event->pos());
    Qt::CursorShape resizeCursor = Qt::ArrowCursor;
    bool onResizeHandle = false;
    
    // Check all media items for handle hover (prioritize topmost, only if selected)
    qreal topZ = -std::numeric_limits<qreal>::infinity();
    for (QGraphicsItem* it : m_scene->items()) {
        if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) {
            // Only show resize cursor if the item is selected
            if (rp->isSelected() && rp->zValue() >= topZ) {
                Qt::CursorShape itemCursor = rp->cursorForScenePos(scenePos);
                if (itemCursor != Qt::ArrowCursor) {
                    resizeCursor = itemCursor;
                    onResizeHandle = true;
                    topZ = rp->zValue();
                }
            }
        }
    }
    
    // Set the cursor with highest priority
    if (onResizeHandle) {
        viewport()->setCursor(resizeCursor);
    } else {
        viewport()->unsetCursor();
    }
    
    // Handle dragging and panning logic
    if (event->buttons() & Qt::LeftButton) {
        // If a selected video is currently dragging its sliders, update it even if cursor left its shape
        for (QGraphicsItem* it : m_scene->items()) {
            if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                if (v->isSelected() && (v->isDraggingProgress() || v->isDraggingVolume())) {
                    v->updateDragWithScenePos(mapToScene(event->pos()));
                    event->accept();
                    return;
                }
            }
        }
        const QList<QGraphicsItem*> hitItems = items(event->pos());
        auto toMedia = [](QGraphicsItem* x)->ResizableMediaBase* {
            while (x) { if (auto* m = dynamic_cast<ResizableMediaBase*>(x)) return m; x = x->parentItem(); }
            return nullptr;
        };
        bool hitMedia = false;
        for (QGraphicsItem* it : hitItems) { if (toMedia(it)) { hitMedia = true; break; } }
        if (hitMedia) { QGraphicsView::mouseMoveEvent(event); return; }
    }
    if (m_panning) {
        // Pan the view
        QPoint delta = event->pos() - m_lastPanPoint;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        m_lastPanPoint = event->pos();
    }
    QGraphicsView::mouseMoveEvent(event);
}

void ScreenCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // Finalize any active drag on selected video controls
        for (QGraphicsItem* it : m_scene->items()) {
            if (auto* v = dynamic_cast<ResizableVideoItem*>(it)) {
                if (v->isSelected() && (v->isDraggingProgress() || v->isDraggingVolume())) {
                    v->endDrag();
                    event->accept();
                    return;
                }
            }
        }
        if (m_panning) {
            m_panning = false;
            event->accept();
            return;
        }
        // Check if any item was being resized and reset cursor
        bool wasResizing = false;
        for (QGraphicsItem* it : m_scene->items()) {
            if (auto* rp = dynamic_cast<ResizableMediaBase*>(it)) {
                if (rp->isActivelyResizing()) {
                    wasResizing = true;
                    break;
                }
            }
        }
        if (wasResizing) {
            // Reset cursor after resize
            viewport()->unsetCursor();
        }
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void ScreenCanvas::wheelEvent(QWheelEvent* event) {
    // On macOS during an active pinch, ignore wheel-based two-finger scroll to avoid jitter
#ifdef Q_OS_MACOS
    if (m_nativePinchActive) {
        event->ignore();
        return;
    }
#endif
    // If Cmd (macOS) or Ctrl (others) is held, treat the wheel as zoom, anchored under cursor
#ifdef Q_OS_MACOS
    const bool zoomModifier = event->modifiers().testFlag(Qt::MetaModifier);
#else
    const bool zoomModifier = event->modifiers().testFlag(Qt::ControlModifier);
#endif

    if (zoomModifier) {
        qreal deltaY = 0.0;
        if (!event->pixelDelta().isNull()) {
            deltaY = event->pixelDelta().y();
        } else if (!event->angleDelta().isNull()) {
            // angleDelta is in 1/8 degree; 120 typically represents one step
            deltaY = event->angleDelta().y() / 8.0; // convert to degrees-ish
        }
        if (deltaY != 0.0) {
            // Convert delta to an exponential zoom factor for smoothness
            const qreal factor = std::pow(1.0015, deltaY);
            zoomAroundViewportPos(event->position(), factor);
            event->accept();
            return;
        }
    }

    // Default behavior: scroll/pan content
    // Prefer pixelDelta for trackpads (gives smooth 2D vector), fallback to angleDelta for mouse wheels.
    QPoint delta;
    if (!event->pixelDelta().isNull()) {
        delta = event->pixelDelta();
    } else if (!event->angleDelta().isNull()) {
        delta = event->angleDelta() / 8; // small scaling for smoother feel
    }
    if (!delta.isNull()) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void ScreenCanvas::zoomAroundViewportPos(const QPointF& vpPosF, qreal factor) {
    QPoint vpPos = vpPosF.toPoint();
    if (!viewport()->rect().contains(vpPos)) {
        vpPos = viewport()->rect().center();
    }
    const QPointF sceneAnchor = mapToScene(vpPos);
    // Compose a new transform that scales around the scene anchor directly
    QTransform t = transform();
    t.translate(sceneAnchor.x(), sceneAnchor.y());
    t.scale(factor, factor);
    t.translate(-sceneAnchor.x(), -sceneAnchor.y());
    setTransform(t);
}

MainWindow::~MainWindow() {
    if (m_webSocketClient->isConnected()) {
        m_webSocketClient->disconnect();
    }
}

void MainWindow::setupUI() {
    m_centralWidget = new QWidget(this);
    setCentralWidget(m_centralWidget);
    
    m_mainLayout = new QVBoxLayout(m_centralWidget);
    m_mainLayout->setSpacing(0); // Remove spacing, we'll handle it manually
    m_mainLayout->setContentsMargins(0, 0, 0, 0); // Remove margins from main layout
    
    // Top section with margins
    QWidget* topSection = new QWidget();
    QVBoxLayout* topLayout = new QVBoxLayout(topSection);
    topLayout->setContentsMargins(20, 20, 20, 20); // Apply margins only to top section
    topLayout->setSpacing(20);
    
    // Connection section (always visible)
    m_connectionLayout = new QHBoxLayout();
    
    // Back button (left-aligned, initially hidden)
    m_backButton = new QPushButton("← Back to Client List");
    m_backButton->setStyleSheet("QPushButton { padding: 8px 16px; font-weight: bold; }");
    m_backButton->setAutoDefault(false);
    m_backButton->setDefault(false);
    m_backButton->setFocusPolicy(Qt::NoFocus);
    m_backButton->hide(); // Initially hidden, shown only on screen view
    connect(m_backButton, &QPushButton::clicked, this, &MainWindow::onBackToClientListClicked);
    
    // Status label (no "Status:")
    m_connectionStatusLabel = new QLabel("DISCONNECTED");
    m_connectionStatusLabel->setStyleSheet("QLabel { color: red; font-weight: bold; }");

    // Enable/Disable toggle button with fixed width (left of Settings)
    m_connectToggleButton = new QPushButton("Disable");
    m_connectToggleButton->setStyleSheet("QPushButton { padding: 8px 16px; font-weight: bold; }");
    m_connectToggleButton->setFixedWidth(111);
    connect(m_connectToggleButton, &QPushButton::clicked, this, &MainWindow::onEnableDisableClicked);

    // Settings button
    m_settingsButton = new QPushButton("Settings");
    m_settingsButton->setStyleSheet("QPushButton { padding: 8px 16px; font-weight: bold; }");
    connect(m_settingsButton, &QPushButton::clicked, this, &MainWindow::showSettingsDialog);

    // Layout: [back][stretch][status][connect][settings]
    m_connectionLayout->addWidget(m_backButton);
    m_connectionLayout->addStretch();
    m_connectionLayout->addWidget(m_connectionStatusLabel);
    m_connectionLayout->addWidget(m_connectToggleButton);
    m_connectionLayout->addWidget(m_settingsButton);
    
    topLayout->addLayout(m_connectionLayout);
    m_mainLayout->addWidget(topSection);
    
    // Bottom section with margins (no separator line)
    QWidget* bottomSection = new QWidget();
    QVBoxLayout* bottomLayout = new QVBoxLayout(bottomSection);
    bottomLayout->setContentsMargins(20, 20, 20, 20); // Apply margins only to bottom section
    bottomLayout->setSpacing(20);
    
    // Create stacked widget for page navigation
    m_stackedWidget = new QStackedWidget();
    // Block stray key events (like space) at the stack level
    m_stackedWidget->installEventFilter(this);
    bottomLayout->addWidget(m_stackedWidget);
    m_mainLayout->addWidget(bottomSection);
    
    // Create client list page
    createClientListPage();
    
    // Create screen view page  
    createScreenViewPage();
    
    // Start with client list page
    m_stackedWidget->setCurrentWidget(m_clientListPage);

    // Receive remote cursor updates when watching
    connect(m_webSocketClient, &WebSocketClient::cursorPositionReceived, this,
            [this](const QString& targetId, int x, int y) {
                if (m_stackedWidget->currentWidget() == m_screenViewWidget && targetId == m_watchedClientId && m_screenCanvas) {
                    m_screenCanvas->updateRemoteCursor(x, y);
                }
            });
}

void MainWindow::createClientListPage() {
    m_clientListPage = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(m_clientListPage);
    layout->setSpacing(15);
    layout->setContentsMargins(0, 0, 0, 0);
    
    // Client list section
    m_clientListLabel = new QLabel("Connected Clients:");
    m_clientListLabel->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; }");
    layout->addWidget(m_clientListLabel);
    
    m_clientListWidget = new QListWidget();
    // Use palette-based colors so light/dark themes adapt automatically
    // Add subtle hover effect and remove persistent selection highlight
    m_clientListWidget->setStyleSheet(
        "QListWidget { "
        "   border: 1px solid palette(mid); "
        "   border-radius: 5px; "
        "   padding: 5px; "
        "   background-color: palette(base); "
        "   color: palette(text); "
        "}"
        "QListWidget::item { "
        "   padding: 10px; "
        "   border-bottom: 1px solid palette(midlight); "
        "}" 
        // Hover: very light blue tint
        "QListWidget::item:hover { "
        "   background-color: rgba(74, 144, 226, 28); "
        "}"
        // Suppress active/selected highlight colors
        "QListWidget::item:selected { "
        "   background-color: transparent; "
        "   color: palette(text); "
        "}"
        "QListWidget::item:selected:active { "
        "   background-color: transparent; "
        "   color: palette(text); "
        "}"
        "QListWidget::item:selected:hover { "
        "   background-color: rgba(74, 144, 226, 28); "
        "   color: palette(text); "
        "}"
    );
    connect(m_clientListWidget, &QListWidget::itemClicked, this, &MainWindow::onClientItemClicked);
    // Prevent keyboard (space/enter) from triggering navigation
    m_clientListWidget->setFocusPolicy(Qt::NoFocus);
    m_clientListWidget->installEventFilter(this);
    // Enable hover state over items (for :hover style)
    m_clientListWidget->setMouseTracking(true);
    layout->addWidget(m_clientListWidget);
    
    m_noClientsLabel = new QLabel("No clients connected. Make sure other devices are running Mouffette and connected to the same server.");
    m_noClientsLabel->setStyleSheet("QLabel { color: #666; font-style: italic; text-align: center; }");
    m_noClientsLabel->setAlignment(Qt::AlignCenter);
    m_noClientsLabel->setWordWrap(true);
    layout->addWidget(m_noClientsLabel);
    
    // Selected client info
    m_selectedClientLabel = new QLabel();
    m_selectedClientLabel->setStyleSheet("QLabel { background-color: #e8f4fd; padding: 10px; border-radius: 5px; }");
    m_selectedClientLabel->setWordWrap(true);
    m_selectedClientLabel->hide();
    layout->addWidget(m_selectedClientLabel);
    
    // Add to stacked widget
    m_stackedWidget->addWidget(m_clientListPage);
    
    // Initially hide the separate "no clients" label since we'll show it in the list widget itself
    m_noClientsLabel->hide();
}

void MainWindow::createScreenViewPage() {
    // Screen view page
    m_screenViewWidget = new QWidget();
    m_screenViewLayout = new QVBoxLayout(m_screenViewWidget);
    m_screenViewLayout->setSpacing(15);
    m_screenViewLayout->setContentsMargins(0, 0, 0, 0);
    
    // Header row: hostname on the left, indicators on the right (replaces "Connected Clients:" title)
    QHBoxLayout* headerLayout = new QHBoxLayout();

    m_clientNameLabel = new QLabel();
    m_clientNameLabel->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; color: palette(text); }");
    m_clientNameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    m_volumeIndicator = new QLabel("🔈 --");
    m_volumeIndicator->setStyleSheet("QLabel { font-size: 16px; color: palette(text); font-weight: bold; }");
    m_volumeIndicator->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_volumeIndicator->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    headerLayout->addWidget(m_clientNameLabel, 0, Qt::AlignLeft);
    headerLayout->addStretch();
    headerLayout->addWidget(m_volumeIndicator, 0, Qt::AlignRight);
    headerLayout->setContentsMargins(0, 0, 0, 0);

    m_screenViewLayout->addLayout(headerLayout);
    
    // Canvas container holds spinner and canvas with a stacked layout
    m_canvasContainer = new QWidget();
    m_canvasContainer->setObjectName("CanvasContainer");
    m_canvasContainer->setMinimumHeight(400);
    // Ensure stylesheet background/border is actually painted
    m_canvasContainer->setAttribute(Qt::WA_StyledBackground, true);
    // Match the dark background used by the client list container via palette(base)
    m_canvasContainer->setStyleSheet(
        "QWidget#CanvasContainer { "
        "   border: 1px solid palette(mid); "
        "   border-radius: 5px; "
        "   background-color: palette(base); "
    "} "
        // Ensure stacked pages inherit the same background to prevent light grey bleed
    "QWidget#CanvasContainer > * { "
    "   background-color: palette(base); "
    "   border-radius: 5px; "
    "}"
    );
    QVBoxLayout* containerLayout = new QVBoxLayout(m_canvasContainer);
    // Leave a small margin so the rounded border is visible and not overdrawn by children
    containerLayout->setContentsMargins(4,4,4,4);
    containerLayout->setSpacing(0);
    m_canvasStack = new QStackedWidget();
    // Rounded corners and border directly on the visible stack area
    m_canvasStack->setStyleSheet(
        "QStackedWidget { "
        "   background-color: palette(base); "
        "   border: 1px solid palette(mid); "
        "   border-radius: 5px; "
        "}"
    );
    containerLayout->addWidget(m_canvasStack);
    // Clip stack to rounded corners
    m_canvasStack->installEventFilter(this);

    // Spinner page
    m_loadingSpinner = new SpinnerWidget();
    // Initial appearance (easy to tweak):
    m_loadingSpinner->setRadius(22);        // circle radius in px
    m_loadingSpinner->setLineWidth(6);      // line width in px
    m_loadingSpinner->setColor(QColor("#4a90e2")); // brand blue
    m_loadingSpinner->setMinimumSize(QSize(48, 48));
    // Spinner page widget wraps the spinner centered
    QWidget* spinnerPage = new QWidget();
    QVBoxLayout* spinnerLayout = new QVBoxLayout(spinnerPage);
    spinnerLayout->setContentsMargins(0,0,0,0);
    spinnerLayout->setSpacing(0);
    spinnerLayout->addStretch();
    spinnerLayout->addWidget(m_loadingSpinner, 0, Qt::AlignCenter);
    spinnerLayout->addStretch();
    // Spinner page opacity effect & animation (fade entire loader area)
    m_spinnerOpacity = new QGraphicsOpacityEffect(spinnerPage);
    spinnerPage->setGraphicsEffect(m_spinnerOpacity);
    m_spinnerOpacity->setOpacity(0.0);
    m_spinnerFade = new QPropertyAnimation(m_spinnerOpacity, "opacity", this);
    m_spinnerFade->setDuration(m_loaderFadeDurationMs);
    m_spinnerFade->setStartValue(0.0);
    m_spinnerFade->setEndValue(1.0);
    // spinnerPage already created above

    // Canvas page
    QWidget* canvasPage = new QWidget();
    QVBoxLayout* canvasLayout = new QVBoxLayout(canvasPage);
    canvasLayout->setContentsMargins(0,0,0,0);
    canvasLayout->setSpacing(0);
    m_screenCanvas = new ScreenCanvas();
    m_screenCanvas->setMinimumHeight(400);
    // Ensure the viewport background matches and is rounded
    if (m_screenCanvas->viewport()) {
        m_screenCanvas->viewport()->setAttribute(Qt::WA_StyledBackground, true);
        m_screenCanvas->viewport()->setAutoFillBackground(true);
        m_screenCanvas->viewport()->setStyleSheet("background-color: palette(base); border-radius: 5px;");
    }
    m_screenCanvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // Ensure viewport repaints fully so opacity effect animates correctly
    m_screenCanvas->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    // Screens are not clickable; canvas supports panning and media placement
    canvasLayout->addWidget(m_screenCanvas);
    // Canvas/content opacity effect & animation (apply to viewport for QGraphicsView)
    m_canvasOpacity = new QGraphicsOpacityEffect(m_screenCanvas->viewport());
    m_screenCanvas->viewport()->setGraphicsEffect(m_canvasOpacity);
    m_canvasOpacity->setOpacity(0.0);
    m_canvasFade = new QPropertyAnimation(m_canvasOpacity, "opacity", this);
    m_canvasFade->setDuration(m_fadeDurationMs);
    m_canvasFade->setStartValue(0.0);
    m_canvasFade->setEndValue(1.0);

    // Add pages and container to main layout
    m_canvasStack->addWidget(spinnerPage); // index 0: spinner
    m_canvasStack->addWidget(canvasPage);  // index 1: canvas
    m_canvasStack->setCurrentIndex(1);     // default to canvas page hidden (opacity 0) until data
    m_screenViewLayout->addWidget(m_canvasContainer, 1);
    // Clip container and viewport to rounded corners
    m_canvasContainer->installEventFilter(this);
    if (m_screenCanvas && m_screenCanvas->viewport()) m_screenCanvas->viewport()->installEventFilter(this);

    // Ensure focus is on canvas, and block stray key events
    m_screenViewWidget->installEventFilter(this);
    m_screenCanvas->setFocusPolicy(Qt::StrongFocus);
    m_screenCanvas->installEventFilter(this);
    
    // Send button
    m_sendButton = new QPushButton("Send Media to All Screens");
    m_sendButton->setStyleSheet("QPushButton { padding: 12px 24px; font-weight: bold; background-color: #4a90e2; color: white; border-radius: 5px; }");
    m_sendButton->setEnabled(false); // Initially disabled until media is placed
    connect(m_sendButton, &QPushButton::clicked, this, &MainWindow::onSendMediaClicked);
    // Keep button at bottom, centered
    m_screenViewLayout->addWidget(m_sendButton, 0, Qt::AlignHCenter);
    // Ensure header has no stretch, container expands, button fixed
    m_screenViewLayout->setStretch(0, 0); // header
    m_screenViewLayout->setStretch(1, 1); // container expands
    m_screenViewLayout->setStretch(2, 0); // button fixed

    // Volume label opacity effect & animation
    m_volumeOpacity = new QGraphicsOpacityEffect(m_volumeIndicator);
    m_volumeIndicator->setGraphicsEffect(m_volumeOpacity);
    m_volumeOpacity->setOpacity(0.0);
    m_volumeFade = new QPropertyAnimation(m_volumeOpacity, "opacity", this);
    m_volumeFade->setDuration(m_fadeDurationMs);
    m_volumeFade->setStartValue(0.0);
    m_volumeFade->setEndValue(1.0);

    // Loader delay timer
    m_loaderDelayTimer = new QTimer(this);
    m_loaderDelayTimer->setSingleShot(true);
    
    // Add to stacked widget
    m_stackedWidget->addWidget(m_screenViewWidget);
}

void MainWindow::setupMenuBar() {
    // File menu
    m_fileMenu = menuBar()->addMenu("File");
    
    m_exitAction = new QAction("Quit Mouffette", this);
    m_exitAction->setShortcut(QKeySequence::Quit);
    connect(m_exitAction, &QAction::triggered, this, [this]() {
        if (m_webSocketClient->isConnected()) {
            m_webSocketClient->disconnect();
        }
        QApplication::quit();
    });
    m_fileMenu->addAction(m_exitAction);
    
    // Help menu
    m_helpMenu = menuBar()->addMenu("Help");
    
    m_aboutAction = new QAction("About", this);
    connect(m_aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, "About Mouffette", 
            "Mouffette v1.0.0\n\n"
            "A cross-platform media sharing application that allows users to "
            "share and display media on other connected users' screens.\n\n"
            "Built with Qt and WebSocket technology.");
    });
    m_helpMenu->addAction(m_aboutAction);
}

void MainWindow::setupSystemTray() {
    // Create tray icon (no context menu, just click handling)
    m_trayIcon = new QSystemTrayIcon(this);
    
    // Set icon - try to load from resources, fallback to simple icon
    QIcon trayIconIcon(":/icons/mouffette.png");
    if (trayIconIcon.isNull()) {
        // Fallback to simple colored icon
        QPixmap pixmap(16, 16);
        pixmap.fill(Qt::blue);
        trayIconIcon = QIcon(pixmap);
    }
    m_trayIcon->setIcon(trayIconIcon);
    
    // Set tooltip
    m_trayIcon->setToolTip("Mouffette - Media Sharing");
    
    // Connect tray icon activation for non-context menu clicks
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &MainWindow::onTrayIconActivated);
    
    // Show the tray icon
    m_trayIcon->show();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_trayIcon && m_trayIcon->isVisible()) {
        // Hide to tray instead of closing
        hide();
        event->ignore();
        
        // Show message first time
        static bool firstHide = true;
        if (firstHide) {
            showTrayMessage("Mouffette", "Application is now running in the background. Click the tray icon to show the window again.");
            firstHide = false;
        }
    } else {
        event->accept();
    }
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    
    // If we're currently showing the screen view and have a canvas with content,
    // recenter the view to maintain good visibility after window resize
    if (m_stackedWidget && m_stackedWidget->currentWidget() == m_screenViewWidget && 
        m_screenCanvas && !m_selectedClient.getScreens().isEmpty()) {
        m_screenCanvas->recenterWithMargin(33);
    }
}

void MainWindow::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason) {
    // Show/hide window on any click (left, right, or double-click)
    switch (reason) {
    case QSystemTrayIcon::Trigger:        // Single left-click
    case QSystemTrayIcon::DoubleClick:    // Double left-click  
    case QSystemTrayIcon::Context:        // Right-click
        {
            const bool minimized = (windowState() & Qt::WindowMinimized);
            const bool hidden = isHidden() || !isVisible();
            if (minimized || hidden) {
                // Reveal and focus the window if minimized or hidden
                if (minimized) {
                    setWindowState(windowState() & ~Qt::WindowMinimized);
                    showNormal();
                } else {
                    show();
                }
                raise();
                activateWindow();
            } else {
                // Fully visible: toggle to hide to tray
                hide();
            }
        }
        break;
    default:
        break;
    }
}

void MainWindow::showTrayMessage(const QString& title, const QString& message) {
    if (m_trayIcon) {
        m_trayIcon->showMessage(title, message, QSystemTrayIcon::Information, 3000);
    }
}

void MainWindow::onEnableDisableClicked() {
    if (!m_webSocketClient) return;
    
    if (m_connectToggleButton->text() == "Disable") {
        // Disable client: disconnect and prevent auto-reconnect
        m_userDisconnected = true;
        m_reconnectTimer->stop(); // Stop any pending reconnection
        if (m_webSocketClient->isConnected()) {
            m_webSocketClient->disconnect();
        }
        m_connectToggleButton->setText("Enable");
    } else {
        // Enable client: allow connections and start connecting
        m_userDisconnected = false;
        m_reconnectAttempts = 0; // Reset reconnection attempts
        connectToServer();
        m_connectToggleButton->setText("Disable");
    }
}

// Settings dialog: server URL with Save/Cancel
void MainWindow::showSettingsDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle("Settings");
    QVBoxLayout* v = new QVBoxLayout(&dialog);
    QLabel* urlLabel = new QLabel("Server URL");
    QLineEdit* urlEdit = new QLineEdit(&dialog);
    if (m_serverUrlConfig.isEmpty()) m_serverUrlConfig = DEFAULT_SERVER_URL;
    urlEdit->setText(m_serverUrlConfig);
    v->addWidget(urlLabel);
    v->addWidget(urlEdit);

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    QPushButton* cancelBtn = new QPushButton("Cancel");
    QPushButton* saveBtn = new QPushButton("Save");
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(saveBtn);
    v->addLayout(btnRow);

    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, this, [this, urlEdit, &dialog]() {
        const QString newUrl = urlEdit->text().trimmed();
        if (!newUrl.isEmpty()) {
            bool changed = (newUrl != (m_serverUrlConfig.isEmpty() ? DEFAULT_SERVER_URL : m_serverUrlConfig));
            m_serverUrlConfig = newUrl;
            if (changed) {
                // Restart connection to apply new server URL
                if (m_webSocketClient->isConnected()) {
                    m_userDisconnected = false; // this is not a manual disconnect, we want reconnect
                    m_webSocketClient->disconnect();
                }
                connectToServer();
            }
        }
        dialog.accept();
    });

    dialog.exec();
}

void MainWindow::connectToServer() {
    const QString url = m_serverUrlConfig.isEmpty() ? DEFAULT_SERVER_URL : m_serverUrlConfig;
    m_webSocketClient->connectToServer(url);
}

void MainWindow::scheduleReconnect() {
    if (m_userDisconnected) {
        return; // Don't reconnect if user disabled the client
    }
    
    // Exponential backoff: 2^attempts seconds, capped at maxReconnectDelay
    int delay = qMin(static_cast<int>(qPow(2, m_reconnectAttempts)) * 1000, m_maxReconnectDelay);
    
    // Add some jitter to avoid thundering herd (±25%)
    int jitter = QRandomGenerator::global()->bounded(-delay/4, delay/4);
    delay += jitter;
    
    qDebug() << "Scheduling reconnect attempt" << (m_reconnectAttempts + 1) << "in" << delay << "ms";
    
    m_reconnectTimer->start(delay);
    m_reconnectAttempts++;
}

void MainWindow::attemptReconnect() {
    if (m_userDisconnected) {
        return; // Don't reconnect if user disabled the client
    }
    
    qDebug() << "Attempting reconnection...";
    connectToServer();
}

void MainWindow::onConnected() {
    setUIEnabled(true);
    // Reset reconnection state on successful connection
    m_reconnectAttempts = 0;
    m_reconnectTimer->stop();
    
    // Sync this client's info with the server
    syncRegistration();
    
    statusBar()->showMessage("Connected to server", 3000);
    
    // Show tray notification
    showTrayMessage("Mouffette Connected", "Successfully connected to Mouffette server");
}

void MainWindow::onDisconnected() {
    setUIEnabled(false);
    
    // Start smart reconnection if client is enabled and not manually disconnected
    if (!m_userDisconnected) {
        scheduleReconnect();
    }
    
    // Stop watching if any
    if (!m_watchedClientId.isEmpty()) {
        m_watchedClientId.clear();
    }
    
    // Clear client list
    m_availableClients.clear();
    updateClientList(m_availableClients);
    
    statusBar()->showMessage("Disconnected from server", 3000);
    
    // Show tray notification
    showTrayMessage("Mouffette Disconnected", "Disconnected from Mouffette server");
}

void MainWindow::startWatchingSelectedClient() {
    if (!m_webSocketClient || !m_webSocketClient->isConnected()) return;
    const QString targetId = m_selectedClient.getId();
    if (targetId.isEmpty()) return;
    if (m_watchedClientId == targetId) return; // already watching
    if (!m_watchedClientId.isEmpty()) {
        m_webSocketClient->unwatchScreens(m_watchedClientId);
    }
    m_webSocketClient->watchScreens(targetId);
    m_watchedClientId = targetId;
}

void MainWindow::stopWatchingCurrentClient() {
    if (!m_webSocketClient || !m_webSocketClient->isConnected()) { m_watchedClientId.clear(); return; }
    if (m_watchedClientId.isEmpty()) return;
    m_webSocketClient->unwatchScreens(m_watchedClientId);
    m_watchedClientId.clear();
}

void MainWindow::onConnectionError(const QString& error) {
    QMessageBox::warning(this, "Connection Error", 
        QString("Failed to connect to server:\n%1").arg(error));
    
    setUIEnabled(false);
    // No direct connect/disconnect buttons anymore
}

void MainWindow::onClientListReceived(const QList<ClientInfo>& clients) {
    qDebug() << "Received client list with" << clients.size() << "clients";
    
    // Check for new clients
    int previousCount = m_availableClients.size();
    m_availableClients = clients;
    updateClientList(clients);
    
    // Show notification if new clients appeared
    if (clients.size() > previousCount && previousCount >= 0) {
        int newClients = clients.size() - previousCount;
        if (newClients > 0) {
            QString message = QString("%1 new client%2 available for sharing")
                .arg(newClients)
                .arg(newClients == 1 ? "" : "s");
            showTrayMessage("New Clients Available", message);
        }
    }
}

void MainWindow::onRegistrationConfirmed(const ClientInfo& clientInfo) {
    m_thisClient = clientInfo;
    qDebug() << "Registration confirmed for:" << clientInfo.getMachineName();
    
    // Request initial client list
    m_webSocketClient->requestClientList();
}

void MainWindow::onClientSelectionChanged() {
    if (m_ignoreSelectionChange) {
        return;
    }
    
    QListWidgetItem* currentItem = m_clientListWidget->currentItem();
    
    if (currentItem) {
        int index = m_clientListWidget->row(currentItem);
        if (index >= 0 && index < m_availableClients.size()) {
            const ClientInfo& client = m_availableClients[index];
            m_selectedClient = client;
            
            // Show the screen view for the selected client
            showScreenView(client);
            if (m_webSocketClient && m_webSocketClient->isConnected()) {
                m_webSocketClient->requestScreens(client.getId());
            }
        }
    } else {
        m_selectedClientLabel->hide();
    }
}

// (Duplicate removed) onScreensInfoReceived is implemented later in the file

void MainWindow::syncRegistration() {
    QString machineName = getMachineName();
    QString platform = getPlatformName();
    QList<ScreenInfo> screens;
    int volumePercent = -1;
    // Only include screens/volume when actively watched; otherwise identity-only
    if (m_isWatched) {
        screens = getLocalScreenInfo();
        volumePercent = getSystemVolumePercent();
    }
    
    qDebug() << "Sync registration:" << machineName << "on" << platform << "with" << screens.size() << "screens";
    
    m_webSocketClient->registerClient(machineName, platform, screens, volumePercent);
}

void MainWindow::onScreensInfoReceived(const ClientInfo& clientInfo) {
    // Update the canvas only if it matches the currently selected client
    if (!clientInfo.getId().isEmpty() && clientInfo.getId() == m_selectedClient.getId()) {
        qDebug() << "Updating canvas with fresh screens for" << clientInfo.getMachineName();
        m_selectedClient = clientInfo; // keep selected client in sync
        
    // Switch to canvas page and stop spinner; show volume with fresh data
    if (m_loaderDelayTimer) m_loaderDelayTimer->stop();
    if (m_canvasStack) m_canvasStack->setCurrentIndex(1);
    if (m_screenCanvas) {
        m_screenCanvas->viewport()->update();
        if (m_screenCanvas->scene()) m_screenCanvas->scene()->update();
    }
    if (m_loadingSpinner) { m_loadingSpinner->stop(); }
        if (m_screenCanvas) {
            m_screenCanvas->setScreens(clientInfo.getScreens());
            m_screenCanvas->recenterWithMargin(33);
            m_screenCanvas->setFocus(Qt::OtherFocusReason);
        }
        // Fade-in canvas content
    if (m_canvasFade && m_canvasOpacity) {
            applyAnimationDurations();
            m_canvasOpacity->setOpacity(0.0);
            m_canvasFade->setStartValue(0.0);
            m_canvasFade->setEndValue(1.0);
            m_canvasFade->start();
        }
        // Update and fade-in volume
        if (m_volumeIndicator) {
            updateVolumeIndicator();
            m_volumeIndicator->show();
            if (m_volumeFade && m_volumeOpacity) {
                applyAnimationDurations();
                m_volumeOpacity->setOpacity(0.0);
                m_volumeFade->setStartValue(0.0);
                m_volumeFade->setEndValue(1.0);
                m_volumeFade->start();
            }
        }
        
        // Optional: refresh UI labels if platform/machine changed
        m_clientNameLabel->setText(QString("%1 (%2)").arg(clientInfo.getMachineName()).arg(clientInfo.getPlatform()));
    }
}

void MainWindow::onWatchStatusChanged(bool watched) {
    // Store watched state locally (as this client being watched by someone else)
    // We don't need a member; we can gate sending by this flag at runtime.
    // For simplicity, keep a static so our timers can read it.
    m_isWatched = watched;
    qDebug() << "Watch status changed:" << (watched ? "watched" : "not watched");

    // Begin/stop sending our cursor position to watchers (target side)
    if (watched) {
        if (!m_cursorTimer) {
            m_cursorTimer = new QTimer(this);
            m_cursorTimer->setInterval(m_cursorUpdateIntervalMs); // configurable
            connect(m_cursorTimer, &QTimer::timeout, this, [this]() {
                static int lastX = INT_MIN, lastY = INT_MIN;
                const QPoint p = QCursor::pos();
                if (p.x() != lastX || p.y() != lastY) {
                    lastX = p.x();
                    lastY = p.y();
                    if (m_webSocketClient && m_webSocketClient->isConnected() && m_isWatched) {
                        m_webSocketClient->sendCursorUpdate(p.x(), p.y());
                    }
                }
            });
        }
        // Apply any updated interval before starting
        m_cursorTimer->setInterval(m_cursorUpdateIntervalMs);
        if (!m_cursorTimer->isActive()) m_cursorTimer->start();
    } else {
        if (m_cursorTimer) m_cursorTimer->stop();
    }
}

QList<ScreenInfo> MainWindow::getLocalScreenInfo() {
    QList<ScreenInfo> screens;
    QList<QScreen*> screenList = QGuiApplication::screens();
    
    for (int i = 0; i < screenList.size(); ++i) {
        QScreen* screen = screenList[i];
        QRect geometry = screen->geometry();
        bool isPrimary = (screen == QGuiApplication::primaryScreen());
        
        ScreenInfo screenInfo(i, geometry.width(), geometry.height(), geometry.x(), geometry.y(), isPrimary);
        screens.append(screenInfo);
    }
    
    return screens;
}

QString MainWindow::getMachineName() {
    QString hostName = QHostInfo::localHostName();
    if (hostName.isEmpty()) {
        hostName = "Unknown Machine";
    }
    return hostName;
}

QString MainWindow::getPlatformName() {
#ifdef Q_OS_MACOS
    return "macOS";
#elif defined(Q_OS_WIN)
    return "Windows";
#elif defined(Q_OS_LINUX)
    return "Linux";
#else
    return "Unknown";
#endif
}

#ifdef Q_OS_MACOS
int MainWindow::getSystemVolumePercent() {
    // Return cached value; updated asynchronously in setupVolumeMonitoring()
    return m_cachedSystemVolume;
}
#elif defined(Q_OS_WIN)
int MainWindow::getSystemVolumePercent() {
    // Use Windows Core Audio APIs (MMDevice + IAudioEndpointVolume)
    // Headers are included only on Windows builds.
    HRESULT hr;
    IMMDeviceEnumerator* pEnum = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioEndpointVolume* pEndpointVol = nullptr;
    bool coInit = SUCCEEDED(CoInitialize(nullptr));
    int result = -1;
    do {
        hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&pEnum);
        if (FAILED(hr) || !pEnum) break;
        hr = pEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
        if (FAILED(hr) || !pDevice) break;
        hr = pDevice->Activate(IID_IAudioEndpointVolume, CLSCTX_ALL, nullptr, (void**)&pEndpointVol);
        if (FAILED(hr) || !pEndpointVol) break;
        float volScalar = 0.0f;
        hr = pEndpointVol->GetMasterVolumeLevelScalar(&volScalar);
        if (FAILED(hr)) break;
        int vol = static_cast<int>(std::round(volScalar * 100.0f));
        vol = std::clamp(vol, 0, 100);
        result = vol;
    } while (false);
    if (pEndpointVol) pEndpointVol->Release();
    if (pDevice) pDevice->Release();
    if (pEnum) pEnum->Release();
    if (coInit) CoUninitialize();
    return result;
#elif defined(Q_OS_LINUX)
int MainWindow::getSystemVolumePercent() {
    return -1; // TODO: Implement via PulseAudio/PipeWire if needed
#else
int MainWindow::getSystemVolumePercent() {
    return -1;
}
#endif

void MainWindow::setupVolumeMonitoring() {
#ifdef Q_OS_MACOS
    // Asynchronous polling to avoid blocking the UI thread.
    if (!m_volProc) {
        m_volProc = new QProcess(this);
        // No visible window; ensure fast exit
        connect(m_volProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int, QProcess::ExitStatus) {
            const QByteArray out = m_volProc->readAllStandardOutput().trimmed();
            bool ok = false;
            int vol = QString::fromUtf8(out).toInt(&ok);
            if (ok) {
                vol = std::clamp(vol, 0, 100);
                if (vol != m_cachedSystemVolume) {
                    m_cachedSystemVolume = vol;
                    if (m_webSocketClient->isConnected() && m_isWatched) {
                        syncRegistration();
                    }
                }
            }
        });
    }
    if (!m_volTimer) {
        m_volTimer = new QTimer(this);
        m_volTimer->setInterval(1200); // ~1.2s cadence
        connect(m_volTimer, &QTimer::timeout, this, [this]() {
            if (m_volProc->state() == QProcess::NotRunning) {
                m_volProc->start("/usr/bin/osascript", {"-e", "output volume of (get volume settings)"});
            }
        });
        m_volTimer->start();
    }
#else
    // Non-macOS: simple polling; Windows call is fast.
    QTimer* volTimer = new QTimer(this);
    volTimer->setInterval(1200);
    connect(volTimer, &QTimer::timeout, this, [this]() {
        int v = getSystemVolumePercent();
        if (v != m_cachedSystemVolume) {
            m_cachedSystemVolume = v;
            if (m_webSocketClient->isConnected() && m_isWatched) {
                syncRegistration();
            }
        }
    });
    volTimer->start();
#endif
}

void MainWindow::updateClientList(const QList<ClientInfo>& clients) {
    m_clientListWidget->clear();
    
    if (clients.isEmpty()) {
        // Show the "no clients" message centered in the list widget with larger font
        QListWidgetItem* item = new QListWidgetItem("No clients connected. Make sure other devices are running Mouffette and connected to the same server.");
        item->setFlags(Qt::NoItemFlags); // Make it non-selectable and non-interactive
        item->setTextAlignment(Qt::AlignCenter);
        QFont font = item->font();
        font.setItalic(true);
        font.setPointSize(16); // Make the font larger
        item->setFont(font);
        item->setForeground(QColor(102, 102, 102)); // #666 color
        
    // Set a custom size hint to center the item vertically in the list widget.
    // Use the viewport height (content area) to avoid off-by-margins that cause scrollbars.
    const int viewportH = m_clientListWidget->viewport() ? m_clientListWidget->viewport()->height() : m_clientListWidget->height();
    item->setSizeHint(QSize(m_clientListWidget->width(), qMax(0, viewportH)));
        
        m_clientListWidget->addItem(item);
    // Ensure no scrollbars are shown for the single placeholder item
    m_clientListWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_clientListWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_noClientsLabel->hide(); // Hide the separate label since we show message in list
    } else {
        m_noClientsLabel->hide();
    // Restore scrollbar policies when there are items to potentially scroll
    m_clientListWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_clientListWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        
        for (const ClientInfo& client : clients) {
            QString displayText = client.getDisplayText();
            QListWidgetItem* item = new QListWidgetItem(displayText);
            item->setToolTip(QString("ID: %1\nStatus: %2").arg(client.getId()).arg(client.getStatus()));
            m_clientListWidget->addItem(item);
        }
    }
    
    // Hide selected client info when list changes
    m_selectedClientLabel->hide();
}

void MainWindow::setUIEnabled(bool enabled) {
    // Client list depends on connection
    m_clientListWidget->setEnabled(enabled);
}

void MainWindow::updateConnectionStatus() {
    QString status = m_webSocketClient->getConnectionStatus();
    // Always display status in uppercase
    m_connectionStatusLabel->setText(status.toUpper());
    
    if (status == "Connected") {
        m_connectionStatusLabel->setStyleSheet("QLabel { color: green; font-weight: bold; }");
    } else if (status.startsWith("Connecting") || status.startsWith("Reconnecting")) {
        m_connectionStatusLabel->setStyleSheet("QLabel { color: orange; font-weight: bold; }");
    } else {
        m_connectionStatusLabel->setStyleSheet("QLabel { color: red; font-weight: bold; }");
    }
}


