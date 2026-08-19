#pragma once

#include <QMainWindow>

class QCloseEvent;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QTimer;
class SvgCanvas;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void updatePreview();
    void openFile();
    bool saveFile();
    bool saveFileAs();
    void exportPng();
    void copyMarkup();
    void loadPreset(int index);
    void updateCursorPosition();

private:
    void buildUi();
    void buildMenus();
    void buildToolbar();
    void populatePresets();
    void setDocument(const QString &markup, const QString &path = {});
    bool writeSvgFile(const QString &path);
    bool confirmDiscardChanges();
    void updateWindowTitle();
    void showFileError(const QString &message);

    QPlainTextEdit *m_editor = nullptr;
    SvgCanvas *m_canvas = nullptr;
    QComboBox *m_presets = nullptr;
    QComboBox *m_backgrounds = nullptr;
    QLabel *m_validationLabel = nullptr;
    QLabel *m_positionLabel = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QTimer *m_previewTimer = nullptr;
    QString m_currentPath;
    bool m_loadingPreset = false;
};
