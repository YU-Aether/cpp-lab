#include "main_window.h"

#include <QApplication>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    lab::MainWindow window;
    window.resize(680, 460);
    window.show();

    return app.exec();
}