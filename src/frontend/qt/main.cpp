#include "main_window.hpp"

#include <QApplication>
#include <QStandardPaths>

#include <filesystem>
#include <iostream>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setApplicationName("Project360");
    application.setOrganizationName("Project360");

    const QString app_data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (app_data.isEmpty()) {
        std::cerr << "Project360 could not locate the application data directory.\n";
        return 1;
    }

    const auto library_file = std::filesystem::path(app_data.toStdWString()) / "library.p360";
    MainWindow window(library_file);
    window.show();
    return application.exec();
}
