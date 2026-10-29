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

    MainWindow w;
    w.show();
    const QStringList args = app.arguments();
    if (args.size() > 1) w.openFile(args.at(1));
    return app.exec();
}
