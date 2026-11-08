#pragma once

#include "frontend/game_library_store.hpp"

#include <filesystem>
#include <QMainWindow>
#include <QString>

class QLabel;
class QListWidget;
class QPushButton;
class QWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(std::filesystem::path library_file, QWidget* parent = nullptr);

private:
    void importExecutable();
    void refreshLibrary(const QString& selected_path = {});
    void showSelection();
    void applyTheme();

    QListWidget* library_ = nullptr;
    QWidget* hero_ = nullptr;
    QLabel* selectedTitle_ = nullptr;
    QLabel* selectedPath_ = nullptr;
    QLabel* selectionStatus_ = nullptr;
    QPushButton* launchButton_ = nullptr;
    QPushButton* importButton_ = nullptr;
    project360::frontend::GameLibraryStore library_store_;
    QString selectedExecutable_;
};
