#include "main_window.hpp"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setApplicationName("Project360");
    application.setOrganizationName("Project360");

    MainWindow window;
    window.show();
    return application.exec();
}
