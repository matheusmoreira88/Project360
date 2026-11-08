#include "main_window.hpp"

#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSize>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

namespace {

QLabel* makeLabel(const QString& text, const QString& styleClass = {}) {
    auto* label = new QLabel(text);
    if (!styleClass.isEmpty()) {
        label->setProperty("class", styleClass);
    }
    return label;
}

QFrame* makeSeparator() {
    auto* separator = new QFrame;
    separator->setFrameShape(QFrame::HLine);
    separator->setProperty("class", "separator");
    return separator;
}

QWidget* makeSidebar() {
    auto* sidebar = new QWidget;
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(232);

    auto* layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(20, 24, 16, 20);
    layout->setSpacing(8);

    auto* brandRow = new QHBoxLayout;
    auto* mark = makeLabel("P", "brandMark");
    mark->setAlignment(Qt::AlignCenter);
    mark->setFixedSize(38, 38);
    auto* brand = makeLabel("PROJECT360", "brandName");
    brandRow->addWidget(mark);
    brandRow->addSpacing(10);
    brandRow->addWidget(brand);
    brandRow->addStretch();
    layout->addLayout(brandRow);
    layout->addSpacing(28);

    layout->addWidget(makeLabel("BIBLIOTECA", "sectionLabel"));
    auto* libraryNav = new QPushButton(QStringLiteral("▦    Biblioteca"));
    libraryNav->setObjectName("activeNav");
    libraryNav->setCursor(Qt::PointingHandCursor);
    libraryNav->setMinimumHeight(42);
    layout->addWidget(libraryNav);
    layout->addStretch();

    auto* statusCard = new QWidget;
    statusCard->setObjectName("sidebarStatus");
    auto* statusLayout = new QVBoxLayout(statusCard);
    statusLayout->setContentsMargins(12, 12, 12, 12);
    statusLayout->setSpacing(5);
    statusLayout->addWidget(makeLabel("ESTADO DO PROJETO", "sectionLabel"));
    statusLayout->addWidget(makeLabel("Em desenvolvimento", "statusValue"));
    statusLayout->addWidget(makeLabel("Loader XEX indisponível", "mutedText"));
    layout->addWidget(statusCard);
    return sidebar;
}

QWidget* makeHero() {
    auto* hero = new QWidget;
    hero->setObjectName("hero");
    hero->setMinimumHeight(208);
    auto* layout = new QVBoxLayout(hero);
    layout->setContentsMargins(28, 25, 28, 25);
    layout->setSpacing(8);

    auto* eyebrow = makeLabel("EMULAÇÃO ORIGINAL • XBOX 360", "heroEyebrow");
    layout->addWidget(eyebrow);
    auto* title = makeLabel("Sua biblioteca,\nno seu espaço.", "heroTitle");
    title->setWordWrap(true);
    layout->addWidget(title);
    auto* subtitle = makeLabel("Uma base aberta para desenvolver e validar a emulação do console.", "heroSubtitle");
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);
    layout->addStretch();
    return hero;
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Project360 — Biblioteca");
    resize(1180, 760);
    setMinimumSize(880, 600);
    applyTheme();

    auto* root = new QWidget;
    auto* rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    rootLayout->addWidget(makeSidebar());

    auto* content = new QWidget;
    content->setObjectName("content");
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(34, 27, 34, 28);
    contentLayout->setSpacing(18);

    auto* topRow = new QHBoxLayout;
    auto* headingColumn = new QVBoxLayout;
    headingColumn->setSpacing(3);
    headingColumn->addWidget(makeLabel("Biblioteca", "pageTitle"));
    headingColumn->addWidget(makeLabel("Seus arquivos locais e o estado de suporte.", "mutedText"));
    topRow->addLayout(headingColumn);
    topRow->addStretch();
    importButton_ = new QPushButton("＋  Adicionar jogo");
    importButton_->setObjectName("primaryButton");
    importButton_->setMinimumHeight(42);
    importButton_->setCursor(Qt::PointingHandCursor);
    topRow->addWidget(importButton_);
    contentLayout->addLayout(topRow);
    hero_ = makeHero();
    contentLayout->addWidget(hero_);

    auto* libraryHeader = new QHBoxLayout;
    libraryHeader->addWidget(makeLabel("Adicionados", "sectionHeading"));
    libraryHeader->addStretch();
    libraryHeader->addWidget(makeLabel("ARQUIVOS LOCAIS", "sectionLabel"));
    contentLayout->addLayout(libraryHeader);

    auto* libraryArea = new QHBoxLayout;
    libraryArea->setSpacing(18);
    library_ = new QListWidget;
    library_->setObjectName("libraryList");
    library_->setMinimumWidth(270);
    library_->setMaximumWidth(370);
    library_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    library_->setUniformItemSizes(true);
    library_->setSpacing(5);
    library_->setAccessibleName("Arquivos adicionados à biblioteca");
    libraryArea->addWidget(library_);

    auto* details = new QWidget;
    details->setObjectName("detailsCard");
    auto* detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(22, 20, 22, 20);
    detailsLayout->setSpacing(10);
    detailsLayout->addWidget(makeLabel("DETALHES", "sectionLabel"));
    selectedTitle_ = makeLabel("Nenhum arquivo selecionado", "detailTitle");
    selectedTitle_->setWordWrap(true);
    detailsLayout->addWidget(selectedTitle_);
    selectedPath_ = makeLabel("Adicione um executável local para exibir o caminho.", "mutedText");
    selectedPath_->setWordWrap(true);
    selectedPath_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailsLayout->addWidget(selectedPath_);
    detailsLayout->addWidget(makeSeparator());
    selectionStatus_ = makeLabel("O carregador de executáveis ainda não foi implementado.", "noticeText");
    selectionStatus_->setWordWrap(true);
    detailsLayout->addWidget(selectionStatus_);
    detailsLayout->addStretch();
    launchButton_ = new QPushButton("Iniciar");
    launchButton_->setObjectName("secondaryButton");
    launchButton_->setMinimumHeight(42);
    launchButton_->setEnabled(false);
    launchButton_->setToolTip("Indisponível: CPU, kernel e loader necessários ainda estão em desenvolvimento.");
    detailsLayout->addWidget(launchButton_);
    libraryArea->addWidget(details, 1);
    contentLayout->addLayout(libraryArea, 1);

    rootLayout->addWidget(content, 1);
    setCentralWidget(root);

    connect(importButton_, &QPushButton::clicked, this, &MainWindow::importExecutable);
    connect(library_, &QListWidget::currentRowChanged, this, [this](int) { showSelection(); });
}

void MainWindow::importExecutable() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Adicionar executável Xbox 360",
        {},
        "Executáveis Xbox 360 (*.xex);;Todos os arquivos (*.*)");
    if (path.isEmpty()) {
        return;
    }

    const QFileInfo file(path);
    if (!file.exists() || !file.isFile() || file.suffix().compare("xex", Qt::CaseInsensitive) != 0) {
        selectedTitle_->setText("Arquivo não adicionado");
        selectedPath_->setText("Selecione um arquivo existente com extensão .xex.");
        selectionStatus_->setText("A biblioteca aceita apenas caminhos locais para XEX nesta etapa.");
        return;
    }

    selectedExecutable_ = file.absoluteFilePath();
    const QString displayName = file.completeBaseName();
    bool alreadyListed = false;
    for (int row = 0; row < library_->count(); ++row) {
        if (library_->item(row)->data(Qt::UserRole).toString() == selectedExecutable_) {
            library_->setCurrentRow(row);
            alreadyListed = true;
            break;
        }
    }
    if (!alreadyListed) {
        auto* item = new QListWidgetItem(QStringLiteral("◈   %1\n     XEX • não carregado").arg(displayName));
        item->setData(Qt::UserRole, selectedExecutable_);
        item->setToolTip(selectedExecutable_);
        item->setSizeHint(QSize(0, 62));
        library_->addItem(item);
        library_->setCurrentItem(item);
    }
    hero_->setVisible(library_->count() == 0);
    showSelection();
}

void MainWindow::showSelection() {
    const auto* item = library_->currentItem();
    if (item == nullptr) {
        return;
    }
    selectedExecutable_ = item->data(Qt::UserRole).toString();
    const QFileInfo file(selectedExecutable_);
    selectedTitle_->setText(file.completeBaseName());
    selectedPath_->setText(file.absoluteFilePath());
    selectionStatus_->setText(
        "Arquivo encontrado. O Project360 ainda não analisa metadados, mapeia nem executa este XEX.");
}

void MainWindow::applyTheme() {
    setStyleSheet(R"(
        QWidget { color: #e8edf7; font-family: "Segoe UI"; font-size: 10pt; }
        QMainWindow, #content { background: #10131a; }
        #sidebar { background: #0b0e14; border-right: 1px solid #222936; }
        QLabel[class="brandMark"] { background: #55d6be; color: #071410; border-radius: 11px; font-size: 19pt; font-weight: 800; }
        QLabel[class="brandName"] { color: #f4f7fc; font-size: 11pt; font-weight: 750; letter-spacing: 1px; }
        QLabel[class="sectionLabel"] { color: #778397; font-size: 8pt; font-weight: 700; letter-spacing: 1px; }
        QLabel[class="statusValue"] { color: #e4eaf4; font-weight: 650; }
        QLabel[class="mutedText"] { color: #909caf; }
        QPushButton { border: 0; border-radius: 8px; padding: 9px 12px; text-align: left; }
        QPushButton#activeNav { background: #1b292e; color: #75e0ce; font-weight: 650; }
        QPushButton#navButton { background: transparent; color: #a3adbd; }
        QPushButton#navButton:disabled { color: #697487; }
        QPushButton#navButton:hover:enabled { background: #171e28; color: #e8edf7; }
        #sidebarStatus, #detailsCard { background: #171c26; border: 1px solid #272f3d; border-radius: 12px; }
        #hero { border: 1px solid #28645e; border-radius: 15px;
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #173d40, stop:0.52 #1a2b37, stop:1 #20243a); }
        QLabel[class="heroEyebrow"] { color: #86e6d5; font-size: 8pt; font-weight: 750; letter-spacing: 1px; }
        QLabel[class="heroTitle"] { color: #f7fbff; font-size: 23pt; font-weight: 750; }
        QLabel[class="heroSubtitle"] { color: #c3d2dc; }
        QLabel[class="pageTitle"] { font-size: 21pt; font-weight: 750; }
        QLabel[class="sectionHeading"] { font-size: 12pt; font-weight: 700; }
        QLabel[class="detailTitle"] { font-size: 16pt; font-weight: 700; }
        QLabel[class="noticeText"] { color: #f1c778; background: #302919; border: 1px solid #514326; border-radius: 8px; padding: 11px; }
        QFrame[class="separator"] { color: #293140; }
        QListWidget#libraryList { background: #151a23; border: 1px solid #272f3d; border-radius: 11px; padding: 7px; outline: 0; }
        QListWidget#libraryList::item { color: #bfc8d5; border-radius: 7px; padding: 8px; }
        QListWidget#libraryList::item:selected { background: #213c3c; color: #83e3d4; }
        QListWidget#libraryList::item:hover:!selected { background: #1d2430; }
        QPushButton#primaryButton { background: #55d6be; color: #081714; font-weight: 750; padding: 10px 17px; }
        QPushButton#primaryButton:hover { background: #79e5d1; }
        QPushButton#secondaryButton { background: #27313d; color: #788598; font-weight: 700; text-align: center; }
        QPushButton#secondaryButton:disabled { background: #222832; color: #707a89; }
    )");
}
