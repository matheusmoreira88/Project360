#pragma once

#include <QMainWindow>
#include <QString>

class QLabel;
class QListWidget;
class QPushButton;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void importExecutable();
    void showSelection();
    void applyTheme();

    QListWidget* library_ = nullptr;
    QLabel* selectedTitle_ = nullptr;
    QLabel* selectedPath_ = nullptr;
    QLabel* selectionStatus_ = nullptr;
    QPushButton* launchButton_ = nullptr;
    QPushButton* importButton_ = nullptr;
    QString selectedExecutable_;
};
