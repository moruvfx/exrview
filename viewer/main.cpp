#include "MainWindow.h"

#include <QApplication>
#include <QIcon>
#include <QPalette>
#include <QStyleFactory>
#include <QSurfaceFormat>

int main(int argc, char** argv)
{
    // OpenGL 3.3 core cho shader OCIO. Máy không đáp ứng -> ImageView tự chuyển sang render CPU.
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    QApplication::setOrganizationName("ExrView");
    QApplication::setApplicationName("ExrView");
    QApplication::setApplicationVersion(EXRVIEW_VERSION);
    QApplication::setWindowIcon(QIcon(":/exrview.png"));

    // Giao diện tối giống các phần mềm VFX — màu nền trung tính không làm lệch cảm nhận màu.
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, QColor(48, 48, 50));
    p.setColor(QPalette::WindowText, QColor(220, 220, 222));
    p.setColor(QPalette::Base, QColor(34, 34, 36));
    p.setColor(QPalette::AlternateBase, QColor(42, 42, 44));
    p.setColor(QPalette::ToolTipBase, QColor(30, 30, 32));
    p.setColor(QPalette::ToolTipText, QColor(220, 220, 222));
    p.setColor(QPalette::Text, QColor(220, 220, 222));
    p.setColor(QPalette::Button, QColor(56, 56, 58));
    p.setColor(QPalette::ButtonText, QColor(220, 220, 222));
    p.setColor(QPalette::Highlight, QColor(232, 140, 40));
    p.setColor(QPalette::HighlightedText, Qt::black);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 120));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 120));
    app.setPalette(p);

    app.setStyleSheet(R"(
        QMainWindow::separator { background: #1e1e20; width: 2px; height: 2px; }
        QToolBar { background: #2d2d30; border-bottom: 1px solid #1c1c1e; spacing: 4px; padding: 2px 4px; }
        QToolButton { background: transparent; border: 1px solid transparent; border-radius: 3px; padding: 3px 6px; }
        QToolButton:hover { background: #3e3e42; border-color: #55555a; }
        QToolButton:checked { background: #e88c28; color: #111; border-color: #f09c38; font-weight: bold; }
        QComboBox, QDoubleSpinBox, QSpinBox { background: #232326; border: 1px solid #3f3f44; border-radius: 3px; padding: 2px 6px; min-height: 20px; }
        QComboBox:hover, QDoubleSpinBox:hover, QSpinBox:hover { border-color: #606068; }
        QComboBox:focus, QDoubleSpinBox:focus, QSpinBox:focus { border-color: #e88c28; }
        QSlider::groove:horizontal { background: #1e1e20; height: 4px; border-radius: 2px; }
        QSlider::sub-page:horizontal { background: #e88c28; border-radius: 2px; }
        QSlider::handle:horizontal { background: #dcdce0; border: 1px solid #555; width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }
        QSlider::handle:horizontal:hover { background: #ffffff; border-color: #e88c28; }
        QStatusBar { background: #252528; border-top: 1px solid #1c1c1e; }
        QStatusBar::item { border: none; }
        QDockWidget::title { background: #2b2b2e; padding: 4px 8px; border-bottom: 1px solid #1c1c1e; font-weight: bold; }
    )");

    MainWindow w;
    w.show();
    const QStringList args = app.arguments();
    if (args.size() > 1) w.openFile(args.at(1));
    return app.exec();
}
