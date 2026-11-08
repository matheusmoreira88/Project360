#include "frontend/qt/main_window.hpp"

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QListView>
#include <QPixmap>
#include <QPushButton>
#include <QTemporaryDir>
#include <QWidget>

#include <filesystem>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QTemporaryDir temporary_directory;
    check(temporary_directory.isValid(), "temporary application data directory is available");
    if (!temporary_directory.isValid()) {
        return 1;
    }

    const auto library_file =
        std::filesystem::path(temporary_directory.path().toStdWString()) / "library.p360";
    MainWindow window(library_file);
    auto* search = window.findChild<QLineEdit*>("librarySearch");
    auto* library = window.findChild<QListView*>("libraryList");
    auto* sorting = window.findChild<QComboBox*>("sortOrder");
    auto* launch = window.findChild<QPushButton*>("launchButton");
    auto* remove = window.findChild<QPushButton*>("removeButton");

    check(search != nullptr, "search field is present");
    check(library != nullptr, "model-backed library view is present");
    check(sorting != nullptr, "sort selector is present");
    check(launch != nullptr && !launch->isEnabled(), "launch remains disabled without an emulator runtime");
    check(remove != nullptr && !remove->isEnabled(), "remove is disabled when no entry is selected");

    window.show();
    application.processEvents();
    const QPixmap frame = window.grab();
    check(window.isVisible(), "main window is shown");
    check(!frame.isNull() && frame.size().isValid(), "main window renders an offscreen frame");

    if (failures != 0) {
        std::cerr << failures << " Qt window smoke test(s) failed\n";
        return 1;
    }
    std::cout << "Qt window smoke test passed\n";
    return 0;
}
