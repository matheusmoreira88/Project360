#pragma once

#include "game_library_model.hpp"

#include <filesystem>
#include <QMainWindow>
#include <QString>

class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QComboBox;
class QSortFilterProxyModel;
class QWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(std::filesystem::path library_file, QWidget* parent = nullptr);

private:
    void importExecutable();
    void removeSelectedExecutable();
    void updateLibraryCount();
    void showSelection();
    void applyTheme();

    GameLibraryModel* library_model_ = nullptr;
    QSortFilterProxyModel* library_proxy_ = nullptr;
    QListView* library_ = nullptr;
    QWidget* hero_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* sortOrder_ = nullptr;
    QLabel* libraryCount_ = nullptr;
    QLabel* selectedTitle_ = nullptr;
    QLabel* selectedPath_ = nullptr;
    QLabel* selectionStatus_ = nullptr;
    QPushButton* launchButton_ = nullptr;
    QPushButton* importButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
};
