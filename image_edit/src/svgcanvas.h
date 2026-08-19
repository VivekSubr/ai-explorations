#pragma once

#include <QByteArray>
#include <QColor>
#include <QPoint>
#include <QPointF>
#include <QSvgRenderer>
#include <QWidget>

class QKeyEvent;
class QPainter;

class SvgCanvas final : public QWidget
{
    Q_OBJECT

public:
    enum class Background {
        Light,
        Dark,
        Checkerboard,
        Transparent
    };

    explicit SvgCanvas(QWidget *parent = nullptr);

    bool setSvg(const QByteArray &data);
    void setBackground(Background background);
    [[nodiscard]] double zoom() const;

public slots:
    void zoomIn();
    void zoomOut();
    void resetZoom();
    void fitToView();

signals:
    void zoomChanged(double zoom);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    void setZoom(double zoom);
    void drawBackground(QPainter &painter);

    QSvgRenderer m_renderer;
    Background m_background = Background::Checkerboard;
    double m_zoom = 1.0;
    QPointF m_pan;
    QPoint m_lastMousePosition;
    bool m_panning = false;
    bool m_spacePressed = false;
    bool m_autoFit = true;
    bool m_valid = false;
};
