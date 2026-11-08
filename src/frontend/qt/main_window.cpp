#include "main_window.hpp"

#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QAbstractItemView>
#include <QComboBox>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

#include <filesystem>
#include <utility>

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

MainWindow::MainWindow(std::filesystem::path library_file, QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("Project360 — Biblioteca");
    resize(1180, 760);
    setMinimumSize(880, 600);
    applyTheme();
    library_model_ = new GameLibraryModel(std::move(library_file), this);
    library_proxy_ = new QSortFilterProxyModel(this);
    library_proxy_->setSourceModel(library_model_);
    library_proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    library_proxy_->setFilterRole(Qt::DisplayRole);
    library_proxy_->setSortCaseSensitivity(Qt::CaseInsensitive);
    library_proxy_->setSortLocaleAware(true);
    library_proxy_->setSortRole(Qt::DisplayRole);
    library_proxy_->setDynamicSortFilter(true);
    library_proxy_->sort(0, Qt::AscendingOrder);

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
    search_ = new QLineEdit;
    search_->setObjectName("librarySearch");
    search_->setPlaceholderText("Buscar na biblioteca");
    search_->setAccessibleName("Buscar jogos pelo nome do arquivo");
    search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(150);
    search_->setMinimumHeight(40);
    topRow->addWidget(search_);
    importButton_ = new QPushButton("＋  Adicionar jogo");
    importButton_->setObjectName("primaryButton");
    importButton_->setMinimumHeight(42);
    importButton_->setCursor(Qt::PointingHandCursor);
    importButton_->setAccessibleName("Adicionar arquivo XEX à biblioteca");
    topRow->addWidget(importButton_);
    contentLayout->addLayout(topRow);
    hero_ = makeHero();
    contentLayout->addWidget(hero_);

    auto* libraryHeader = new QHBoxLayout;
    libraryHeader->addWidget(makeLabel("Adicionados", "sectionHeading"));
    libraryCount_ = makeLabel("0 arquivos", "mutedText");
    libraryHeader->addWidget(libraryCount_);
    libraryHeader->addStretch();
    libraryHeader->addWidget(makeLabel("ORDENAR", "sectionLabel"));
    sortOrder_ = new QComboBox;
    sortOrder_->setObjectName("sortOrder");
    sortOrder_->addItem("Nome (A–Z)", static_cast<int>(Qt::AscendingOrder));
    sortOrder_->addItem("Nome (Z–A)", static_cast<int>(Qt::DescendingOrder));
    sortOrder_->setAccessibleName("Ordenar os arquivos por nome crescente ou decrescente");
    libraryHeader->addWidget(sortOrder_);
    contentLayout->addLayout(libraryHeader);

    auto* libraryArea = new QHBoxLayout;
    libraryArea->setSpacing(18);
    library_ = new QListView;
    library_->setObjectName("libraryList");
    library_->setModel(library_proxy_);
    library_->setMinimumWidth(270);
    library_->setMaximumWidth(370);
    library_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    library_->setUniformItemSizes(true);
    library_->setSpacing(5);
    library_->setSelectionMode(QAbstractItemView::SingleSelection);
    library_->setSelectionBehavior(QAbstractItemView::SelectRows);
    library_->setEditTriggers(QAbstractItemView::NoEditTriggers);
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
    launchButton_->setObjectName("launchButton");
    launchButton_->setMinimumHeight(42);
    launchButton_->setEnabled(false);
    launchButton_->setToolTip("Indisponível: CPU, kernel e loader necessários ainda estão em desenvolvimento.");
    detailsLayout->addWidget(launchButton_);
    removeButton_ = new QPushButton("Remover da biblioteca");
    removeButton_->setObjectName("removeButton");
    removeButton_->setMinimumHeight(40);
    removeButton_->setEnabled(false);
    removeButton_->setToolTip("Remove apenas o caminho salvo; não apaga o arquivo XEX do disco.");
    removeButton_->setAccessibleName("Remover o caminho selecionado da biblioteca sem apagar o arquivo");
    detailsLayout->addWidget(removeButton_);
    libraryArea->addWidget(details, 1);
    contentLayout->addLayout(libraryArea, 1);

    rootLayout->addWidget(content, 1);
    setCentralWidget(root);

    connect(importButton_, &QPushButton::clicked, this, &MainWindow::importExecutable);
    connect(removeButton_, &QPushButton::clicked, this, &MainWindow::removeSelectedExecutable);
    connect(search_, &QLineEdit::textChanged, library_proxy_, &QSortFilterProxyModel::setFilterFixedString);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString&) {
        updateLibraryCount();
        if (!library_->currentIndex().isValid()) {
            showSelection();
        }
    });
    connect(sortOrder_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        const auto order = static_cast<Qt::SortOrder>(sortOrder_->itemData(index).toInt());
        library_proxy_->sort(0, order);
    });
    connect(library_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex&, const QModelIndex&) { showSelection(); });

    const auto load_result = library_model_->loadResult();
    if (load_result == project360::frontend::LibraryLoadResult::io_error ||
        load_result == project360::frontend::LibraryLoadResult::invalid_data) {
        selectionStatus_->setText("Não foi possível ler a biblioteca salva. Os arquivos originais não foram alterados.");
    }
    updateLibraryCount();
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

    const QString selected_executable = file.absoluteFilePath();
    const auto result = library_model_->addExecutable(
        std::filesystem::path(selected_executable.toStdWString()));
    if (result == project360::frontend::LibraryAddResult::persistence_error) {
        selectedTitle_->setText("Não foi possível salvar a biblioteca");
        selectedPath_->setText(selected_executable);
        selectionStatus_->setText("O caminho não foi adicionado. Verifique as permissões da pasta de dados do Project360.");
        return;
    }
    if (result == project360::frontend::LibraryAddResult::invalid_file ||
        result == project360::frontend::LibraryAddResult::unsupported_extension) {
        selectedTitle_->setText("Arquivo não adicionado");
        selectedPath_->setText("Selecione um arquivo .xex existente.");
        selectionStatus_->setText("O arquivo não passou na validação local e não foi salvo na biblioteca.");
        return;
    }
    search_->clear();
    const auto source_index = library_model_->indexOfPath(selected_executable);
    const auto visible_index = library_proxy_->mapFromSource(source_index);
    if (visible_index.isValid()) {
        library_->setCurrentIndex(visible_index);
        library_->scrollTo(visible_index);
    }
    updateLibraryCount();
}

void MainWindow::removeSelectedExecutable() {
    const QModelIndex visible_index = library_->currentIndex();
    if (!visible_index.isValid()) {
        return;
    }
    const QModelIndex source_index = library_proxy_->mapToSource(visible_index);
    const int row = source_index.row();
    if (!library_model_->removeRows(row, 1)) {
        selectionStatus_->setText("Não foi possível salvar a remoção da biblioteca.");
        return;
    }

    library_->setCurrentIndex({});
    updateLibraryCount();
    if (library_proxy_->rowCount() > 0) {
        library_->setCurrentIndex(library_proxy_->index(0, 0));
    } else {
        showSelection();
    }
}

void MainWindow::updateLibraryCount() {
    const int total = library_model_->rowCount();
    const int visible = library_proxy_->rowCount();
    if (visible == total) {
        libraryCount_->setText(QStringLiteral("%1 arquivos").arg(total));
    } else if (visible == 0) {
        libraryCount_->setText("Nenhum resultado");
    } else {
        libraryCount_->setText(QStringLiteral("%1 de %2 arquivos").arg(visible).arg(total));
    }
    sortOrder_->setEnabled(total > 1);
    hero_->setVisible(total == 0);
}

void MainWindow::showSelection() {
    const QModelIndex visible_index = library_->currentIndex();
    if (!visible_index.isValid()) {
        selectedTitle_->setText(library_model_->rowCount() == 0 ? "Nenhum arquivo selecionado"
                                                               : "Nenhum item selecionado");
        selectedPath_->setText("Selecione um arquivo na biblioteca para ver o caminho.");
        removeButton_->setEnabled(false);
        return;
    }
    const QModelIndex source_index = library_proxy_->mapToSource(visible_index);
    const QString selected_executable = library_model_->data(source_index, GameLibraryModel::PathRole).toString();
    const QString title = library_model_->data(source_index, GameLibraryModel::TitleRole).toString();
    const bool exists = library_model_->refreshAvailability(source_index.row());
    selectedTitle_->setText(title);
    selectedPath_->setText(selected_executable);
    removeButton_->setEnabled(true);
    if (!exists) {
        selectionStatus_->setText("Este caminho salvo não existe mais. O Project360 ainda não analisa nem executa arquivos XEX.");
    } else {
        selectionStatus_->setText(
            "Arquivo encontrado. O Project360 ainda não analisa metadados, mapeia nem executa este XEX.");
    }
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
        QListView#libraryList { background: #151a23; border: 1px solid #272f3d; border-radius: 11px; padding: 7px; outline: 0; }
        QListView#libraryList::item { color: #bfc8d5; border-radius: 7px; padding: 10px 8px; }
        QListView#libraryList::item:selected { background: #213c3c; color: #83e3d4; }
        QListView#libraryList::item:hover:!selected { background: #1d2430; }
        QLineEdit#librarySearch { background: #171c26; color: #e8edf7; border: 1px solid #303949; border-radius: 8px; padding: 9px 12px; }
        QLineEdit#librarySearch:focus { border: 1px solid #55d6be; }
        QComboBox#sortOrder { background: #171c26; color: #c8d0dd; border: 1px solid #303949; border-radius: 7px; padding: 6px 9px; }
        QComboBox#sortOrder::drop-down { border: 0; width: 20px; }
        QComboBox#sortOrder QAbstractItemView { background: #171c26; selection-background-color: #213c3c; }
        QPushButton#primaryButton { background: #55d6be; color: #081714; font-weight: 750; padding: 10px 17px; }
        QPushButton#primaryButton:hover { background: #79e5d1; }
        QPushButton#launchButton { background: #27313d; color: #788598; font-weight: 700; text-align: center; }
        QPushButton#launchButton:disabled { background: #222832; color: #707a89; }
        QPushButton#removeButton { background: #342329; color: #e8a7ac; font-weight: 650; text-align: center; }
        QPushButton#removeButton:hover:enabled { background: #482b33; }
        QPushButton#removeButton:disabled { background: #222832; color: #707a89; }
    )");
}
