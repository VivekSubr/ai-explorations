#include "svgcanvas.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>

namespace {
constexpr double kMinimumZoom = 0.05;
constexpr double kMaximumZoom = 32.0;
constexpr int kCanvasPadding = 64;
}

SvgCanvas::SvgCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    connect(&m_renderer, &QSvgRenderer::repaintNeeded, this, [this] {
        update();
    });
}

bool SvgCanvas::setSvg(const QByteArray &data)
{
    m_valid = m_renderer.load(data);
    if (m_valid && m_autoFit) {
        fitToView();
    } else {
        update();
    }
    return m_valid;
}

void SvgCanvas::setBackground(Background background)
{
    m_background = background;
    update();
}

double SvgCanvas::zoom() const
{
    return m_zoom;
}

void SvgCanvas::zoomIn()
{
    m_autoFit = false;
    setZoom(m_zoom * 1.2);
}

void SvgCanvas::zoomOut()
{
    m_autoFit = false;
    setZoom(m_zoom / 1.2);
}

void SvgCanvas::resetZoom()
{
    m_autoFit = false;
    m_pan = {};
    setZoom(1.0);
}

void SvgCanvas::fitToView()
{
    if (!m_valid) {
        return;
    }

    QSizeF svgSize = m_renderer.defaultSize();
    if (svgSize.isEmpty()) {
        svgSize = m_renderer.viewBoxF().size();
    }
    if (svgSize.isEmpty()) {
        svgSize = QSizeF(512, 512);
    }

    const double availableWidth = std::max(1, width() - kCanvasPadding);
    const double availableHeight = std::max(1, height() - kCanvasPadding);
    m_pan = {};
    m_autoFit = true;
    setZoom(std::min(availableWidth / svgSize.width(),
                     availableHeight / svgSize.height()));
}

void SvgCanvas::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    drawBackground(painter);

    if (!m_valid) {
        painter.setPen(m_background == Background::Dark
                           ? QColor(QStringLiteral("#aeb8c7"))
                           : QColor(QStringLiteral("#667085")));
        painter.drawText(rect(), Qt::AlignCenter,
                         tr("Enter valid SVG markup to preview it"));
        return;
    }

    QSizeF svgSize = m_renderer.defaultSize();
    if (svgSize.isEmpty()) {
        svgSize = m_renderer.viewBoxF().size();
    }

    const QRectF target(-svgSize.width() / 2.0,
                        -svgSize.height() / 2.0,
                        svgSize.width(),
                        svgSize.height());

    painter.translate(rect().center() + m_pan);
    painter.scale(m_zoom, m_zoom);
    m_renderer.render(&painter, target);
}

void SvgCanvas::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_autoFit) {
        fitToView();
    }
}

void SvgCanvas::wheelEvent(QWheelEvent *event)
{
    if (event->angleDelta().y() == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    m_autoFit = false;
    setZoom(m_zoom * (event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15));
    event->accept();
}

void SvgCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton
        || (event->button() == Qt::LeftButton
            && m_spacePressed)) {
        m_panning = true;
        m_autoFit = false;
        m_lastMousePosition = event->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void SvgCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning) {
        const QPoint currentPosition = event->position().toPoint();
        m_pan += currentPosition - m_lastMousePosition;
        m_lastMousePosition = currentPosition;
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void SvgCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_panning
        && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        m_panning = false;
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void SvgCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spacePressed = true;
        setCursor(Qt::OpenHandCursor);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void SvgCanvas::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spacePressed = false;
        if (!m_panning) {
            unsetCursor();
        }
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void SvgCanvas::setZoom(double zoom)
{
    m_zoom = std::clamp(zoom, kMinimumZoom, kMaximumZoom);
    emit zoomChanged(m_zoom);
    update();
}

void SvgCanvas::drawBackground(QPainter &painter)
{
    switch (m_background) {
    case Background::Light:
        painter.fillRect(rect(), QColor(QStringLiteral("#f8fafc")));
        break;
    case Background::Dark:
        painter.fillRect(rect(), QColor(QStringLiteral("#18212f")));
        break;
    case Background::Transparent:
        painter.fillRect(rect(), palette().window());
        break;
    case Background::Checkerboard: {
        constexpr int tileSize = 16;
        const QColor light(QStringLiteral("#f7f8fa"));
        const QColor dark(QStringLiteral("#dfe3e8"));
        painter.fillRect(rect(), light);
        for (int y = 0; y < height(); y += tileSize) {
            for (int x = 0; x < width(); x += tileSize) {
                if (((x / tileSize) + (y / tileSize)) % 2 != 0) {
                    painter.fillRect(x, y, tileSize, tileSize, dark);
                }
            }
        }
        break;
    }
    }
}
