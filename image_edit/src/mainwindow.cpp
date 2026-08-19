#include "mainwindow.h"

#include "svgcanvas.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QSvgRenderer>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QToolBar>
#include <QtMath>

namespace {
const QString kWelcomeSvg = QString::fromUtf8(R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="640" height="420" viewBox="0 0 640 420">
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#7c3aed"/>
      <stop offset="1" stop-color="#06b6d4"/>
    </linearGradient>
    <filter id="shadow" x="-20%" y="-20%" width="140%" height="140%">
      <feDropShadow dx="0" dy="12" stdDeviation="14" flood-opacity=".24"/>
    </filter>
  </defs>
  <rect width="640" height="420" rx="32" fill="#0f172a"/>
  <circle cx="535" cy="95" r="110" fill="#ffffff" opacity=".06"/>
  <path d="M0 320 C130 235 220 380 350 295 S535 230 640 300 V420 H0Z" fill="url(#sky)" opacity=".55"/>
  <g filter="url(#shadow)">
    <rect x="92" y="82" width="456" height="256" rx="24" fill="#ffffff"/>
    <path d="M185 253 245 151l60 102 42-70 108 70Z" fill="url(#sky)"/>
    <circle cx="412" cy="143" r="27" fill="#fbbf24"/>
  </g>
  <text x="320" y="382" text-anchor="middle" fill="#e2e8f0" font-family="sans-serif" font-size="22" font-weight="600">
    Edit the markup and watch it come alive
  </text>
</svg>)SVG");

struct Preset {
    QString name;
    QString markup;
};

const QList<Preset> kPresets = {
    {QObject::tr("Welcome card"), kWelcomeSvg},
    {QObject::tr("App icon"), QString::fromUtf8(R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="320" height="320" viewBox="0 0 320 320">
  <defs>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="1">
      <stop stop-color="#f97316"/>
      <stop offset="1" stop-color="#ec4899"/>
    </linearGradient>
  </defs>
  <rect x="20" y="20" width="280" height="280" rx="72" fill="url(#g)"/>
  <path d="M95 112h130M95 160h90M95 208h130" fill="none" stroke="white" stroke-width="20" stroke-linecap="round"/>
</svg>)SVG")},
    {QObject::tr("Animated loader"), QString::fromUtf8(R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="320" height="320" viewBox="0 0 320 320">
  <rect width="320" height="320" rx="32" fill="#111827"/>
  <g transform="translate(160 160)">
    <circle r="86" fill="none" stroke="#334155" stroke-width="18"/>
    <path d="M0-86a86 86 0 0 1 86 86" fill="none" stroke="#38bdf8" stroke-width="18" stroke-linecap="round">
      <animateTransform attributeName="transform" type="rotate" from="0" to="360" dur="1s" repeatCount="indefinite"/>
    </path>
  </g>
</svg>)SVG")},
    {QObject::tr("Pattern"), QString::fromUtf8(R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="640" height="420" viewBox="0 0 640 420">
  <defs>
    <pattern id="tiles" width="72" height="72" patternUnits="userSpaceOnUse" patternTransform="rotate(20)">
      <rect width="72" height="72" fill="#f8fafc"/>
      <circle cx="18" cy="18" r="9" fill="#8b5cf6"/>
      <circle cx="54" cy="54" r="9" fill="#06b6d4"/>
      <path d="M0 36h72M36 0v72" stroke="#cbd5e1" stroke-width="2"/>
    </pattern>
  </defs>
  <rect width="640" height="420" rx="28" fill="url(#tiles)"/>
</svg>)SVG")}
};
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    buildUi();
    buildMenus();
    buildToolbar();
    populatePresets();
    setDocument(kWelcomeSvg);

    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("window/state")).toByteArray());
}

void MainWindow::buildUi()
{
    setMinimumSize(900, 600);
    resize(1280, 780);

    m_editor = new QPlainTextEdit(this);
    m_editor->setObjectName(QStringLiteral("svgEditor"));
    m_editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setPlaceholderText(tr("Paste SVG markup here..."));
    m_editor->setTabStopDistance(
        QFontMetricsF(m_editor->font()).horizontalAdvance(QLatin1Char(' ')) * 2);

    m_canvas = new SvgCanvas(this);
    m_canvas->setObjectName(QStringLiteral("svgCanvas"));

    auto *splitter = new QSplitter(this);
    splitter->addWidget(m_editor);
    splitter->addWidget(m_canvas);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({500, 780});
    setCentralWidget(splitter);

    m_validationLabel = new QLabel(this);
    m_positionLabel = new QLabel(this);
    m_zoomLabel = new QLabel(tr("100%"), this);
    statusBar()->addWidget(m_validationLabel, 1);
    statusBar()->addPermanentWidget(m_positionLabel);
    statusBar()->addPermanentWidget(m_zoomLabel);

    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(180);

    connect(m_editor, &QPlainTextEdit::textChanged, this, [this] {
        m_previewTimer->start();
        updateWindowTitle();
    });
    connect(m_editor, &QPlainTextEdit::cursorPositionChanged,
            this, &MainWindow::updateCursorPosition);
    connect(m_editor->document(), &QTextDocument::modificationChanged,
            this, &MainWindow::updateWindowTitle);
    connect(m_previewTimer, &QTimer::timeout, this, &MainWindow::updatePreview);
    connect(m_canvas, &SvgCanvas::zoomChanged, this, [this](double zoom) {
        m_zoomLabel->setText(tr("%1%").arg(qRound(zoom * 100.0)));
    });

    updateCursorPosition();
}

void MainWindow::buildMenus()
{
    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    auto *openAction = fileMenu->addAction(tr("&Open..."), this, &MainWindow::openFile);
    openAction->setShortcut(QKeySequence::Open);
    auto *saveAction = fileMenu->addAction(tr("&Save"), this, &MainWindow::saveFile);
    saveAction->setShortcut(QKeySequence::Save);
    auto *saveAsAction = fileMenu->addAction(tr("Save &As..."), this, &MainWindow::saveFileAs);
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("Export &PNG..."), this, &MainWindow::exportPng);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    auto *editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(tr("&Undo"), QKeySequence::Undo, m_editor, &QPlainTextEdit::undo);
    editMenu->addAction(tr("&Redo"), QKeySequence::Redo, m_editor, &QPlainTextEdit::redo);
    editMenu->addSeparator();
    editMenu->addAction(tr("Cu&t"), QKeySequence::Cut, m_editor, &QPlainTextEdit::cut);
    editMenu->addAction(tr("&Copy"), QKeySequence::Copy, m_editor, &QPlainTextEdit::copy);
    editMenu->addAction(tr("&Paste"), QKeySequence::Paste, m_editor, &QPlainTextEdit::paste);
    editMenu->addSeparator();
    editMenu->addAction(tr("Copy SVG markup"), this, &MainWindow::copyMarkup);

    auto *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(tr("Zoom &In"), QKeySequence::ZoomIn, m_canvas, &SvgCanvas::zoomIn);
    viewMenu->addAction(tr("Zoom &Out"), QKeySequence::ZoomOut, m_canvas, &SvgCanvas::zoomOut);
    viewMenu->addAction(tr("&Actual Size"), QKeySequence(Qt::CTRL | Qt::Key_0),
                        m_canvas, &SvgCanvas::resetZoom);
    viewMenu->addAction(tr("&Fit to View"), QKeySequence(Qt::CTRL | Qt::Key_9),
                        m_canvas, &SvgCanvas::fitToView);
}

void MainWindow::buildToolbar()
{
    auto *toolbar = addToolBar(tr("Playground"));
    toolbar->setObjectName(QStringLiteral("playgroundToolbar"));
    toolbar->setMovable(false);

    toolbar->addWidget(new QLabel(tr(" Preset: "), toolbar));
    m_presets = new QComboBox(toolbar);
    m_presets->setMinimumWidth(160);
    toolbar->addWidget(m_presets);
    toolbar->addSeparator();

    toolbar->addAction(tr("-"), m_canvas, &SvgCanvas::zoomOut);
    toolbar->addAction(tr("Fit"), m_canvas, &SvgCanvas::fitToView);
    toolbar->addAction(tr("100%"), m_canvas, &SvgCanvas::resetZoom);
    toolbar->addAction(tr("+"), m_canvas, &SvgCanvas::zoomIn);
    toolbar->addSeparator();

    toolbar->addWidget(new QLabel(tr(" Background: "), toolbar));
    m_backgrounds = new QComboBox(toolbar);
    m_backgrounds->addItems({tr("Checkerboard"), tr("Light"), tr("Dark"), tr("Transparent")});
    toolbar->addWidget(m_backgrounds);

    connect(m_backgrounds, &QComboBox::currentIndexChanged, this, [this](int index) {
        static constexpr SvgCanvas::Background backgrounds[] = {
            SvgCanvas::Background::Checkerboard,
            SvgCanvas::Background::Light,
            SvgCanvas::Background::Dark,
            SvgCanvas::Background::Transparent
        };
        m_canvas->setBackground(backgrounds[index]);
    });
}

void MainWindow::populatePresets()
{
    m_loadingPreset = true;
    for (const Preset &preset : kPresets) {
        m_presets->addItem(preset.name);
    }
    m_loadingPreset = false;
    connect(m_presets, &QComboBox::currentIndexChanged,
            this, &MainWindow::loadPreset);
}

void MainWindow::updatePreview()
{
    const QByteArray svgData = m_editor->toPlainText().toUtf8();
    if (m_canvas->setSvg(svgData)) {
        QSvgRenderer renderer(svgData);
        const QSize size = renderer.defaultSize();
        const QString dimensions = size.isEmpty()
            ? tr("scalable")
            : tr("%1 x %2").arg(size.width()).arg(size.height());
        m_validationLabel->setText(tr("Valid SVG - %1").arg(dimensions));
        m_validationLabel->setStyleSheet(QStringLiteral("color: #159455;"));
    } else {
        m_validationLabel->setText(tr("Invalid SVG"));
        m_validationLabel->setStyleSheet(QStringLiteral("color: #dc2626;"));
    }
}

void MainWindow::openFile()
{
    if (!confirmDiscardChanges()) {
        return;
    }

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open SVG"), {}, tr("SVG images (*.svg);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        showFileError(tr("Could not open %1:\n%2").arg(path, file.errorString()));
        return;
    }
    setDocument(QString::fromUtf8(file.readAll()), path);
}

bool MainWindow::saveFile()
{
    return m_currentPath.isEmpty() ? saveFileAs() : writeSvgFile(m_currentPath);
}

bool MainWindow::saveFileAs()
{
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save SVG"), m_currentPath, tr("SVG images (*.svg)"));
    if (path.isEmpty()) {
        return false;
    }
    if (!path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".svg");
    }
    return writeSvgFile(path);
}

void MainWindow::exportPng()
{
    const QByteArray svgData = m_editor->toPlainText().toUtf8();
    QSvgRenderer renderer(svgData);
    if (!renderer.isValid()) {
        QMessageBox::warning(this, tr("Export PNG"),
                             tr("The SVG markup must be valid before it can be exported."));
        return;
    }

    QSize sourceSize = renderer.defaultSize();
    if (sourceSize.isEmpty()) {
        sourceSize = renderer.viewBox().size();
    }
    if (sourceSize.isEmpty()) {
        sourceSize = QSize(512, 512);
    }

    bool accepted = false;
    const int width = QInputDialog::getInt(
        this, tr("PNG size"), tr("Output width:"), sourceSize.width(),
        16, 16384, 1, &accepted);
    if (!accepted) {
        return;
    }

    const int height = qMax(1, qRound(width * sourceSize.height()
                                     / static_cast<double>(sourceSize.width())));
    const QString suggestedName = m_currentPath.isEmpty()
        ? tr("svg-export.png")
        : QFileInfo(m_currentPath).completeBaseName() + QStringLiteral(".png");
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export PNG"), suggestedName,
        tr("PNG images (*.png)"));
    if (path.isEmpty()) {
        return;
    }
    if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".png");
    }

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    renderer.render(&painter, QRectF(0, 0, width, height));
    painter.end();

    if (!image.save(path, "PNG")) {
        showFileError(tr("Could not export PNG to %1.").arg(path));
        return;
    }
    statusBar()->showMessage(tr("Exported %1 x %2 PNG").arg(width).arg(height), 4000);
}

void MainWindow::copyMarkup()
{
    QApplication::clipboard()->setText(m_editor->toPlainText());
    statusBar()->showMessage(tr("SVG markup copied"), 2500);
}

void MainWindow::loadPreset(int index)
{
    if (m_loadingPreset || index < 0 || index >= kPresets.size()) {
        return;
    }
    if (!confirmDiscardChanges()) {
        m_loadingPreset = true;
        m_presets->setCurrentIndex(-1);
        m_loadingPreset = false;
        return;
    }
    setDocument(kPresets.at(index).markup);
}

void MainWindow::updateCursorPosition()
{
    const QTextCursor cursor = m_editor->textCursor();
    m_positionLabel->setText(
        tr("Ln %1, Col %2").arg(cursor.blockNumber() + 1).arg(cursor.positionInBlock() + 1));
}

void MainWindow::setDocument(const QString &markup, const QString &path)
{
    m_currentPath = path;
    m_editor->setPlainText(markup);
    m_editor->document()->setModified(false);
    updatePreview();
    updateWindowTitle();
}

bool MainWindow::writeSvgFile(const QString &path)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        showFileError(tr("Could not save %1:\n%2").arg(path, file.errorString()));
        return false;
    }

    const QByteArray data = m_editor->toPlainText().toUtf8();
    if (file.write(data) != data.size() || !file.commit()) {
        showFileError(tr("Could not save %1:\n%2").arg(path, file.errorString()));
        return false;
    }

    m_currentPath = path;
    m_editor->document()->setModified(false);
    updateWindowTitle();
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(path).fileName()), 3000);
    return true;
}

bool MainWindow::confirmDiscardChanges()
{
    if (!m_editor->document()->isModified()) {
        return true;
    }

    const QMessageBox::StandardButton choice = QMessageBox::warning(
        this, tr("Unsaved changes"),
        tr("The current SVG has unsaved changes."),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (choice == QMessageBox::Save) {
        return saveFile();
    }
    return choice == QMessageBox::Discard;
}

void MainWindow::updateWindowTitle()
{
    const QString name = m_currentPath.isEmpty()
        ? tr("Untitled")
        : QFileInfo(m_currentPath).fileName();
    const QString modified = m_editor->document()->isModified()
        ? QStringLiteral("*")
        : QString();
    setWindowTitle(tr("%1%2 - SVG Playground").arg(name, modified));
}

void MainWindow::showFileError(const QString &message)
{
    QMessageBox::critical(this, tr("File error"), message);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!confirmDiscardChanges()) {
        event->ignore();
        return;
    }

    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState());
    event->accept();
}
