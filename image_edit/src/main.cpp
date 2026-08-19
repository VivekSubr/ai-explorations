#include "mainwindow.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SVG Playground"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("SVG Playground"));

    application.setStyle(QStringLiteral("Fusion"));
    application.setStyleSheet(QString::fromLatin1(R"CSS(
        QMainWindow {
            background: #eef1f5;
        }
        QToolBar {
            spacing: 6px;
            padding: 7px;
            border: 0;
            border-bottom: 1px solid #d8dee8;
            background: #ffffff;
        }
        QToolButton {
            min-width: 28px;
            padding: 5px 8px;
            border: 1px solid transparent;
            border-radius: 5px;
        }
        QToolButton:hover {
            background: #eef2ff;
            border-color: #c7d2fe;
        }
        QComboBox {
            padding: 5px 28px 5px 8px;
            border: 1px solid #cbd5e1;
            border-radius: 5px;
            background: #ffffff;
        }
        QPlainTextEdit {
            padding: 12px;
            border: 0;
            background: #111827;
            color: #e5e7eb;
            selection-background-color: #4f46e5;
        }
        QStatusBar {
            border-top: 1px solid #d8dee8;
            background: #ffffff;
        }
        QStatusBar QLabel {
            padding: 2px 8px;
        }
        QSplitter::handle {
            width: 2px;
            background: #cbd5e1;
        }
    )CSS"));

    MainWindow window;
    window.show();
    return application.exec();
}
