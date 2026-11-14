#include "MainWindow.h"
#include "HistogramDock.h"
#include "ImageView.h"
#include "TimelineSlider.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageWriter>
#include <QKeyEvent>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QTransform>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace {
const char* kChannelNames[] = {"RGB", "R", "G", "B", "A", "Luminance"};

QString fmtFloat(float v)
{
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
    return QString::number(v, 'f', 5);
}

QString humanSize(qint64 b)
{
    if (b > (1ll << 30)) return QString::number(b / double(1ll << 30), 'f', 2) + " GB";
    if (b > (1 << 20)) return QString::number(b / double(1 << 20), 'f', 1) + " MB";
    return QString::number(b / 1024.0, 'f', 0) + " KB";
}
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle("ExrView");
    resize(1440, 900);
    buildUi();
    buildMenus();
    loadSettings();

    connect(&m_loader, &FrameLoader::loaded, this, &MainWindow::onLoaded);
    connect(&m_playTimer, &QTimer::timeout, this, &MainWindow::tick);
    m_playTimer.setTimerType(Qt::PreciseTimer);
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------- UI
void MainWindow::buildUi()
{
    m_view = new ImageView(&m_pipeline, this);
    connect(m_view, &ImageView::hovered, this, &MainWindow::updateHover);
    connect(m_view, &ImageView::regionSelected, this, &MainWindow::onRegionSelected);
    connect(m_view, &ImageView::fileDropped, this, &MainWindow::openFile);
    connect(m_view, &ImageView::rendererChanged, this, [this](const QString& r) { m_rendererLabel->setText(r); });
    connect(m_view, &ImageView::zoomChanged, this,
            [this](double z) { m_zoomLabel->setText(QString("%1%").arg(qRound(z * 100))); });

    auto* central = new QWidget(this);
    auto* lay = new QVBoxLayout(central);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    m_bBar = buildComparePanel();
    m_bBar->hide();
    lay->addWidget(m_bBar);

    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(m_view);
    m_splitter->addWidget(m_viewB);
    m_viewB->hide();
    lay->addWidget(m_splitter, 1);
    m_seqBar = buildSequenceBar();
    lay->addWidget(m_seqBar);
    m_seqBar->hide();
    setCentralWidget(central);

    // --- Toolbar 1: Layer / Channel / Inspect / Background
    auto* tb = addToolBar(tr("Layer"));
    tb->setObjectName("layerBar");
    tb->setMovable(false);
    tb->addWidget(new QLabel(tr(" Layer ")));
    m_layerCombo = new QComboBox;
    m_layerCombo->setMinimumWidth(200);
    m_layerCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_layerCombo->setMaxVisibleItems(30);
    tb->addWidget(m_layerCombo);
    tb->addWidget(new QLabel(tr("  Kênh ")));
    m_channelCombo = new QComboBox;
    for (auto* n : kChannelNames) m_channelCombo->addItem(n);
    tb->addWidget(m_channelCombo);

    tb->addSeparator();
    tb->addWidget(new QLabel(tr(" Nền ")));
    m_bgCombo = new QComboBox;
    m_bgCombo->addItem(tr("Tối"));
    m_bgCombo->addItem(tr("Đen"));
    m_bgCombo->addItem(tr("Xám 18%"));
    m_bgCombo->addItem(tr("Caro (Alpha)"));
    m_bgCombo->setToolTip(tr("Màu nền & hiển thị Alpha (phím T bật/tắt Caro)"));
    tb->addWidget(m_bgCombo);

    tb->addWidget(new QLabel(tr("  Kiểm tra ")));
    m_inspectCombo = new QComboBox;
    m_inspectCombo->addItem(tr("Bình thường"));
    m_inspectCombo->addItem(tr("False Color"));
    m_inspectCombo->addItem(tr("Clip / NaN / Âm"));
    m_inspectCombo->setToolTip(tr("Chế độ phân tích phơi sáng & lỗi pixel (Shift+F / Shift+Z)"));
    tb->addWidget(m_inspectCombo);

    connect(m_bgCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        auto m = (ImageView::BgMode)idx;
        m_view->setBgMode(m);
        if (m_viewB) m_viewB->setBgMode(m);
    });
    connect(m_inspectCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        auto m = (ImageView::InspectMode)idx;
        m_view->setInspectMode(m);
        if (m_viewB) m_viewB->setInspectMode(m);
    });

    connect(m_layerCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
        if (!m_frame || idx < 0) return;
        QPoint pl = m_layerCombo->itemData(idx).toPoint();
        const auto& parts = m_frame->info.parts;
        if (pl.x() < 0 || pl.x() >= (int)parts.size()) return;
        m_layerRef.part = pl.x();
        m_layerRef.partName = QString::fromStdString(parts[pl.x()].name);
        m_layerRef.layerName = QString::fromStdString(parts[pl.x()].layers[pl.y()].name);
        showIndex(m_index);
    });
    connect(m_channelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { render(); });

    // --- Toolbar 2: Màu / Exposure / Gamma
    addToolBarBreak();
    auto* cb = addToolBar(tr("Màu"));
    cb->setObjectName("colorBar");
    cb->setMovable(false);
    cb->addWidget(new QLabel(tr(" Input ")));
    m_inputCombo = new QComboBox;
    m_inputCombo->setMaxVisibleItems(30);
    m_inputCombo->setMinimumWidth(220);
    cb->addWidget(m_inputCombo);
    cb->addWidget(new QLabel(tr("  Display ")));
    m_displayCombo = new QComboBox;
    m_displayCombo->setMinimumWidth(150);
    cb->addWidget(m_displayCombo);
    cb->addWidget(new QLabel(tr("  View ")));
    m_viewCombo = new QComboBox;
    m_viewCombo->setMinimumWidth(190);
    cb->addWidget(m_viewCombo);

    cb->addWidget(new QLabel(tr("  Exposure ")));
    m_expSlider = new QSlider(Qt::Horizontal);
    m_expSlider->setRange(-100, 100);
    m_expSlider->setFixedWidth(150);
    cb->addWidget(m_expSlider);
    m_expSpin = new QDoubleSpinBox;
    m_expSpin->setRange(-20, 20);
    m_expSpin->setSingleStep(0.1);
    m_expSpin->setDecimals(2);
    m_expSpin->setSuffix(" EV");
    cb->addWidget(m_expSpin);
    cb->addWidget(new QLabel(tr("  Gamma ")));
    m_gammaSpin = new QDoubleSpinBox;
    m_gammaSpin->setRange(0.1, 5.0);
    m_gammaSpin->setSingleStep(0.05);
    m_gammaSpin->setDecimals(2);
    m_gammaSpin->setValue(1.0);
    cb->addWidget(m_gammaSpin);
    auto* reset = new QToolButton;
    reset->setText(tr("Reset"));
    reset->setToolTip(tr("Đưa exposure = 0, gamma = 1 (phím 0)"));
    cb->addWidget(reset);

    connect(m_inputCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (!m_updatingUi) render();
    });
    connect(m_displayCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (m_updatingUi) return;
        populateViews();
        render();
    });
    connect(m_viewCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (!m_updatingUi) render();
    });
    connect(m_expSlider, &QSlider::valueChanged, this, [this](int v) {
        if (m_updatingUi) return;
        m_updatingUi = true;
        m_expSpin->setValue(v / 10.0);
        m_updatingUi = false;
        render();
    });
    connect(m_expSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        if (m_updatingUi) return;
        m_updatingUi = true;
        m_expSlider->setValue(qRound(v * 10));
        m_updatingUi = false;
        render();
    });
    connect(m_gammaSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] { render(); });
    connect(reset, &QToolButton::clicked, this, [this] {
        m_expSpin->setValue(0);
        m_gammaSpin->setValue(1);
    });

    // --- Status bar: pixel inspector
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_pixelLabel = new QLabel;
    m_pixelLabel->setFont(mono);
    m_pixelLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusBar()->addWidget(m_pixelLabel, 1);
    m_infoLabel = new QLabel;
    statusBar()->addPermanentWidget(m_infoLabel);
    m_rendererLabel = new QLabel;
    statusBar()->addPermanentWidget(m_rendererLabel);
    m_configLabel = new QLabel;
    statusBar()->addPermanentWidget(m_configLabel);
    m_zoomLabel = new QLabel("100%");
    m_zoomLabel->setMinimumWidth(50);
    statusBar()->addPermanentWidget(m_zoomLabel);

    // --- Dock thông tin
    m_infoDock = new QDockWidget(tr("Thông tin file"), this);
    m_infoDock->setObjectName("infoDock");
    m_infoTree = new QTreeWidget;
    m_infoTree->setColumnCount(2);
    m_infoTree->setHeaderLabels({tr("Thuộc tính"), tr("Giá trị")});
    m_infoTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_infoTree->setAlternatingRowColors(true);
    m_infoDock->setWidget(m_infoTree);
    addDockWidget(Qt::RightDockWidgetArea, m_infoDock);
    m_infoDock->hide();
    connect(m_infoDock, &QDockWidget::visibilityChanged, this, [this](bool v) { if (v) updateInfoDock(); });

    // --- Dock Histogram & Thống kê
    m_histDock = new HistogramDock(this);
    addDockWidget(Qt::RightDockWidgetArea, m_histDock);
    m_histDock->hide();
    connect(m_histDock, &QDockWidget::visibilityChanged, this, [this](bool v) {
        if (v) updateHistogram();
    });
}

QWidget* MainWindow::buildSequenceBar()
{
    auto* bar = new QWidget;
    auto* l = new QHBoxLayout(bar);
    l->setContentsMargins(8, 4, 8, 4);
    l->setSpacing(4);
    auto mk = [&](const QString& text, const QString& tip, auto fn) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, fn);
        l->addWidget(b);
        return b;
    };
    mk("⏮", tr("Frame đầu (Home)"), [this] { showIndex(effectiveIn()); });
    mk("◀", tr("Frame trước (←)"), [this] { stepFrame(-1); });
    m_playBtn = mk("▶", tr("Play / Pause (Space)"), [this] { togglePlay(); });
    mk("▶|", tr("Frame sau (→)"), [this] { stepFrame(1); });
    mk("⏭", tr("Frame cuối (End)"), [this] { showIndex(effectiveOut()); });
    m_loopBtn = mk("🔁", tr("Chế độ lặp: Loop (bấm để đổi Loop / Ping-Pong / Once)"), [this] { cycleLoopMode(); });

    mk("In", tr("Đặt điểm In tại frame hiện tại"), [this] {
        if (m_frames.size() < 2) return;
        m_inFrame = m_index;
        if (m_outFrame >= 0 && m_outFrame < m_inFrame) m_outFrame = m_inFrame;
        m_seqSlider->setInOutRange(m_inFrame, m_outFrame);
    });
    mk("Out", tr("Đặt điểm Out tại frame hiện tại"), [this] {
        if (m_frames.size() < 2) return;
        m_outFrame = m_index;
        if (m_inFrame > m_outFrame) m_inFrame = m_outFrame;
        m_seqSlider->setInOutRange(m_inFrame, m_outFrame);
    });
    mk("×", tr("Xoá giới hạn In/Out"), [this] {
        m_inFrame = 0;
        m_outFrame = -1;
        m_seqSlider->setInOutRange(0, -1);
    });

    m_seqSlider = new TimelineSlider;
    l->addWidget(m_seqSlider, 1);
    connect(m_seqSlider, &QSlider::valueChanged, this, [this](int v) {
        if (!m_updatingUi && v != m_index) showIndex(v);
    });
    m_frameLabel = new QLabel;
    m_frameLabel->setMinimumWidth(140);
    l->addWidget(m_frameLabel);
    l->addWidget(new QLabel("FPS"));
    m_fpsSpin = new QDoubleSpinBox;
    m_fpsSpin->setRange(1, 120);
    m_fpsSpin->setValue(24);
    m_fpsSpin->setDecimals(3);
    connect(m_fpsSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double f) {
        Q_UNUSED(f);
        if (m_playTimer.isActive()) m_playTimer.setInterval(playTickMs());
    });
    l->addWidget(m_fpsSpin);
    m_cacheLabel = new QLabel;
    m_cacheLabel->setMinimumWidth(170);
    l->addWidget(m_cacheLabel);
    return bar;
}

QWidget* MainWindow::buildComparePanel()
{
    auto* bar = new QWidget;
    bar->setStyleSheet("background: #28282b; border-bottom: 1px solid #1c1c1e;");
    auto* row1 = new QHBoxLayout(bar);
    row1->setContentsMargins(6, 3, 6, 3);
    row1->setSpacing(6);
    auto combo = [](int minChars) {
        auto* c = new QComboBox;
        c->setMaxVisibleItems(30);
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(minChars);
        return c;
    };

    m_bModeCombo = new QComboBox;
    m_bModeCombo->addItem(tr("Cạnh nhau"));
    m_bModeCombo->addItem(tr("Wipe"));
    m_bModeCombo->addItem(tr("Hiệu |A − B|"));
    m_bModeCombo->setToolTip(tr("Cạnh nhau: 2 khung riêng · Wipe: kéo đường chia trên 1 khung (W) · "
                                "Hiệu: |A − B| × 2^exposure của A, tím = NaN/Inf (D)"));
    row1->addWidget(m_bModeCombo);
    row1->addWidget(new QLabel(tr(" <b>B</b>  Layer")));
    m_bLayerCombo = combo(14);
    row1->addWidget(m_bLayerCombo, 2);
    row1->addWidget(new QLabel(tr("Kênh")));
    m_bChannelCombo = new QComboBox;
    for (auto* n : kChannelNames) m_bChannelCombo->addItem(n);
    row1->addWidget(m_bChannelCombo);

    row1->addWidget(new QLabel(tr("Input")));
    m_bInputCombo = combo(14);
    row1->addWidget(m_bInputCombo, 2);
    row1->addWidget(new QLabel(tr("Display")));
    m_bDisplayCombo = combo(8);
    row1->addWidget(m_bDisplayCombo, 1);
    row1->addWidget(new QLabel(tr("View")));
    m_bViewCombo = combo(10);
    row1->addWidget(m_bViewCombo, 1);
    row1->addWidget(new QLabel(tr("Exp")));
    m_bExpSpin = new QDoubleSpinBox;
    m_bExpSpin->setRange(-20, 20);
    m_bExpSpin->setSingleStep(0.1);
    m_bExpSpin->setDecimals(2);
    m_bExpSpin->setSuffix(" EV");
    row1->addWidget(m_bExpSpin);

    auto* openBBtn = new QToolButton;
    openBBtn->setText(tr("Mở B…"));
    openBBtn->setToolTip(tr("Mở file EXR riêng cho khung B (Ctrl+Shift+O)"));
    row1->addWidget(openBBtn);

    auto* copyBtn = new QToolButton;
    copyBtn->setText(tr("B = A"));
    copyBtn->setToolTip(tr("Đưa B về cùng file/layer và cài đặt màu của A"));
    row1->addWidget(copyBtn);

    m_viewB = new ImageView(&m_pipelineB, this);
    m_viewB->setLabel("B");
    m_view->setSecondPipeline(&m_pipelineB);

    connect(m_bLayerCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
        if (!m_frameB || idx < 0) return;
        QPoint pl = m_bLayerCombo->itemData(idx).toPoint();
        const auto& parts = m_frameB->info.parts;
        if (pl.x() < 0 || pl.x() >= (int)parts.size()) return;
        m_layerRefB.part = pl.x();
        m_layerRefB.partName = QString::fromStdString(parts[pl.x()].name);
        m_layerRefB.layerName = QString::fromStdString(parts[pl.x()].layers[pl.y()].name);
        showIndexB(m_index);
    });
    connect(m_bChannelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { renderB(); });
    connect(m_bInputCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (!m_updatingUi) renderB();
    });
    connect(m_bDisplayCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (m_updatingUi) return;
        populateViewsB();
        renderB();
    });
    connect(m_bViewCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (!m_updatingUi) renderB();
    });
    connect(m_bExpSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] {
        if (!m_updatingUi) renderB();
    });
    connect(openBBtn, &QToolButton::clicked, this, [this] {
        QSettings s;
        QString f = QFileDialog::getOpenFileName(this, tr("Mở file EXR cho khung B"), s.value("lastDir").toString(),
                                                 tr("OpenEXR (*.exr *.sxr);;Tất cả (*)"));
        if (!f.isEmpty()) openFileB(f);
    });
    connect(copyBtn, &QToolButton::clicked, this, &MainWindow::copyAtoB);
    connect(m_bModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { applyCompareLayout(); });

    connect(m_viewB, &ImageView::hovered, this, &MainWindow::updateHover);
    connect(m_viewB, &ImageView::fileDropped, this, &MainWindow::openFileB);
    connect(m_view, &ImageView::viewChanged, this, [this](double z, QPointF o, bool fit) {
        if (m_compare && !m_syncingView) m_viewB->setViewTransform(z, o, fit);
    });
    connect(m_viewB, &ImageView::viewChanged, this, [this](double z, QPointF o, bool fit) {
        if (m_compare && !m_syncingView) m_view->setViewTransform(z, o, fit);
    });
    return bar;
}

void MainWindow::buildMenus()
{
    auto* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Mở..."), QKeySequence::Open, this, [this] {
        QSettings s;
        QString f = QFileDialog::getOpenFileName(this, tr("Mở file EXR"), s.value("lastDir").toString(),
                                                 tr("OpenEXR (*.exr *.sxr);;Tất cả (*)"));
        if (!f.isEmpty()) openFile(f);
    });
    file->addAction(tr("Mở file cho khung &B..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), this, [this] {
        QSettings s;
        QString f = QFileDialog::getOpenFileName(this, tr("Mở file EXR cho khung B"), s.value("lastDir").toString(),
                                                 tr("OpenEXR (*.exr *.sxr);;Tất cả (*)"));
        if (!f.isEmpty()) openFileB(f);
    });
    m_recentMenu = file->addMenu(tr("Mở gần đây"));
    rebuildRecentMenu();
    file->addAction(tr("Mở lại"), QKeySequence(Qt::Key_F5), this, [this] {
        m_loader.clear();
        updateTimelineCache();
        showIndex(m_index);
    });
    file->addSeparator();
    file->addAction(tr("&Xuất ảnh (PNG/JPG/TIFF)..."), QKeySequence::Save, this, &MainWindow::exportImage);
    file->addAction(tr("&Chép ảnh vào Clipboard"), QKeySequence::Copy, this, &MainWindow::copyImageToClipboard);
    file->addAction(tr("Chép giá trị pixel (RGBA float)"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this,
                    &MainWindow::copyPixelToClipboard);
    file->addSeparator();
    file->addAction(tr("Thoát"), QKeySequence::Quit, this, &QWidget::close);

    auto* view = menuBar()->addMenu(tr("&Xem"));
    view->addAction(tr("Vừa khung (F)"), QKeySequence(Qt::Key_F), this, [this] { m_view->fit(); });
    view->addAction(tr("100% (1)"), QKeySequence(Qt::Key_1), this, [this] { m_view->setZoom(1.0); });
    view->addAction(tr("200% (2)"), QKeySequence(Qt::Key_2), this, [this] { m_view->setZoom(2.0); });
    view->addAction(tr("Phóng to"), QKeySequence(Qt::Key_Plus), this, [this] { m_view->setZoom(m_view->zoom() * 1.25); });
    view->addAction(tr("Thu nhỏ"), QKeySequence(Qt::Key_Minus), this, [this] { m_view->setZoom(m_view->zoom() / 1.25); });
    view->addSeparator();
    auto* chMenu = view->addMenu(tr("Kênh"));
    const Qt::Key keys[] = {Qt::Key_C, Qt::Key_R, Qt::Key_G, Qt::Key_B, Qt::Key_A, Qt::Key_L};
    for (int i = 0; i < 6; ++i)
        chMenu->addAction(kChannelNames[i], QKeySequence(keys[i]), this, [this, i] {
            m_channelCombo->setCurrentIndex(m_channelCombo->currentIndex() == i && i != 0 ? 0 : i);
        });
    view->addAction(tr("Exposure +½ stop"), QKeySequence(Qt::Key_BracketRight), this,
                    [this] { m_expSpin->setValue(m_expSpin->value() + 0.5); });
    view->addAction(tr("Exposure −½ stop"), QKeySequence(Qt::Key_BracketLeft), this,
                    [this] { m_expSpin->setValue(m_expSpin->value() - 0.5); });
    view->addAction(tr("Reset exposure/gamma"), QKeySequence(Qt::Key_0), this, [this] {
        m_expSpin->setValue(0);
        m_gammaSpin->setValue(1);
    });
    view->addSeparator();
    view->addAction(tr("Bật/tắt nền Caro Alpha"), QKeySequence(Qt::Key_T), this, [this] {
        m_bgCombo->setCurrentIndex(m_bgCombo->currentIndex() == 3 ? 0 : 3);
    });
    view->addAction(tr("False Color (Heatmap phơi sáng)"), QKeySequence(Qt::SHIFT | Qt::Key_F), this, [this] {
        m_inspectCombo->setCurrentIndex(m_inspectCombo->currentIndex() == 1 ? 0 : 1);
    });
    view->addAction(tr("Đánh dấu Clip / NaN / Âm"), QKeySequence(Qt::SHIFT | Qt::Key_Z), this, [this] {
        m_inspectCombo->setCurrentIndex(m_inspectCombo->currentIndex() == 2 ? 0 : 2);
    });
    auto* gridAct = view->addAction(tr("Hiện lưới pixel khi zoom ≥ 800%"));
    gridAct->setCheckable(true);
    gridAct->setChecked(true);
    gridAct->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_G));
    connect(gridAct, &QAction::toggled, this, [this](bool on) {
        m_view->setPixelGrid(on);
        if (m_viewB) m_viewB->setPixelGrid(on);
    });

    auto* xform = view->addMenu(tr("Xoay / Lật"));
    xform->addAction(tr("Lật ngang (H)"), QKeySequence(Qt::Key_H), this, [this] {
        bool on = !m_view->flipH();
        m_view->setFlipH(on);
        if (m_viewB) m_viewB->setFlipH(on);
    });
    xform->addAction(tr("Lật dọc (Shift+H)"), QKeySequence(Qt::SHIFT | Qt::Key_H), this, [this] {
        bool on = !m_view->flipV();
        m_view->setFlipV(on);
        if (m_viewB) m_viewB->setFlipV(on);
    });
    xform->addAction(tr("Xoay 90° phải"), QKeySequence(Qt::CTRL | Qt::Key_R), this, [this] {
        int r = (m_view->rot90() + 1) % 4;
        m_view->setRot90(r);
        if (m_viewB) m_viewB->setRot90(r);
    });
    xform->addAction(tr("Xoay 90° trái"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), this, [this] {
        int r = (m_view->rot90() + 3) % 4;
        m_view->setRot90(r);
        if (m_viewB) m_viewB->setRot90(r);
    });
    xform->addAction(tr("Reset xoay / lật"), this, [this] {
        m_view->setFlipH(false);
        m_view->setFlipV(false);
        m_view->setRot90(0);
        if (m_viewB) {
            m_viewB->setFlipH(false);
            m_viewB->setFlipV(false);
            m_viewB->setRot90(0);
        }
    });

    view->addSeparator();
    auto* overscan = view->addAction(tr("Hiện vùng overscan (data window ngoài display window)"));
    overscan->setCheckable(true);
    overscan->setShortcut(QKeySequence(Qt::Key_O));
    m_loader.setOverscan(QSettings().value("overscan", false).toBool());
    overscan->setChecked(m_loader.overscan());
    connect(overscan, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("overscan", on);
        m_loader.setOverscan(on);
        if (m_index >= 0) showIndex(m_index);
    });
    view->addSeparator();
    m_compareAct = view->addAction(tr("So sánh 2 khung A | B"));
    m_compareAct->setCheckable(true);
    m_compareAct->setShortcut(QKeySequence(Qt::Key_V));
    m_compareAct->setToolTip(tr("Chia đôi màn hình: mỗi khung chọn layer/pass, file và color space riêng"));
    connect(m_compareAct, &QAction::toggled, this, &MainWindow::setCompare);
    view->addAction(tr("So sánh kiểu Wipe (kéo đường chia)"), QKeySequence(Qt::Key_W), this, [this] { setCompareViewMode(1); });
    view->addAction(tr("So sánh kiểu Hiệu |A − B|"), QKeySequence(Qt::Key_D), this, [this] { setCompareViewMode(2); });
    view->addAction(tr("Chia ngang / dọc"), QKeySequence(Qt::SHIFT | Qt::Key_V), this, [this] {
        int o = QSettings().value("compareOrientation", (int)Qt::Horizontal).toInt();
        QSettings().setValue("compareOrientation", o == Qt::Horizontal ? (int)Qt::Vertical : (int)Qt::Horizontal);
        applyCompareLayout();
    });
    m_splitter->setOrientation((Qt::Orientation)QSettings().value("compareOrientation", (int)Qt::Horizontal).toInt());
    view->addAction(tr("B = A (chép cài đặt khung A sang B)"), this, &MainWindow::copyAtoB);
    view->addSeparator();
    view->addAction(m_infoDock->toggleViewAction());
    m_infoDock->toggleViewAction()->setShortcut(QKeySequence(Qt::Key_I));
    view->addAction(m_histDock->toggleViewAction());
    m_histDock->toggleViewAction()->setShortcut(QKeySequence(Qt::Key_F8));

    auto* color = menuBar()->addMenu(tr("&Màu"));
    color->addAction(tr("Nạp OCIO config..."), this, [this] {
        QString f = QFileDialog::getOpenFileName(this, tr("Chọn OCIO config"), QString(),
                                                 tr("OCIO config (*.ocio *.ocioz);;Tất cả (*)"));
        if (!f.isEmpty()) loadOcioConfig(f);
    });
    color->addAction(tr("Dùng config ACES dựng sẵn"), this, [this] { loadOcioConfig(QString()); });
    color->addSeparator();
    color->addAction(tr("Đặt input hiện tại làm mặc định (khi file không ghi color space)"), this,
                     &MainWindow::setDefaultInput);

    auto* exp = menuBar()->addMenu(tr("&Explorer"));
    exp->addAction(tr("Dùng cài đặt màu hiện tại cho thumbnail/preview của Explorer"), this,
                   &MainWindow::applyToExplorerPreview);
#ifdef Q_OS_WIN
    exp->addSeparator();
    exp->addAction(tr("Đăng ký thumbnail/preview (cần quyền Admin)"), this, [this] { registerShellExtension(false); });
    exp->addAction(tr("Gỡ đăng ký thumbnail/preview"), this, [this] { registerShellExtension(true); });
#endif

    auto* cache = menuBar()->addMenu(tr("&Cache"));
    auto* grp = new QActionGroup(this);
    for (int mb : {1024, 2048, 4096, 8192, 16384}) {
        auto* a = cache->addAction(tr("Bộ nhớ sequence: %1 GB").arg(mb / 1024));
        a->setCheckable(true);
        a->setData(mb);
        grp->addAction(a);
        connect(a, &QAction::triggered, this, [this, mb] { m_loader.setBudgetMB(mb); });
    }
    connect(cache, &QMenu::aboutToShow, this, [this, grp] {
        for (auto* a : grp->actions()) a->setChecked(a->data().toInt() == m_loader.budgetMB());
    });
    cache->addAction(tr("Xoá cache"), this, [this] {
        m_loader.clear();
        updateTimelineCache();
    });

    auto* help = menuBar()->addMenu(tr("&Trợ giúp"));
    help->addAction(tr("Phím tắt"), this, [this] {
        QMessageBox::information(
            this, tr("Phím tắt"),
            tr("Ctrl+O  mở file · Ctrl+Shift+O  mở file cho khung B\n"
               "Ctrl+S  xuất ảnh (PNG/JPG/TIFF) · Ctrl+C  chép ảnh · Ctrl+Shift+C  chép pixel\n"
               "F  vừa khung · 1 / 2  zoom 100% / 200% · lăn chuột  zoom · kéo  pan · Shift+kéo  đo vùng\n"
               "C R G B A L  kênh RGB / R / G / B / Alpha / Luminance\n"
               "[ ]  exposure ∓½ stop · 0  reset exposure/gamma\n"
               "T  nền caro Alpha · Shift+F  False Color · Shift+Z  Clip/NaN/Âm · Shift+G  lưới pixel\n"
               "H / Shift+H  lật ngang/dọc · Ctrl+R / Ctrl+Shift+R  xoay 90°\n"
               "← →  frame trước/sau · Space  play/pause · Home/End  frame đầu/cuối\n"
               "I  thông tin file · F8  Histogram & Thống kê · F5  đọc lại · O  overscan\n"
               "V  so sánh A | B · Shift+V  chia ngang/dọc · W  wipe · D  hiệu |A − B|"));
    });
    help->addAction(tr("Giới thiệu"), this, [this] {
        QMessageBox::about(this, "ExrView",
                           tr("<b>ExrView %1</b><br>Xem OpenEXR với quản lý màu OpenColorIO.<br>"
                              "Config: %2")
                               .arg(QCoreApplication::applicationVersion(),
                                    QString::fromStdString(m_pipeline.configName()).toHtmlEscaped()));
    });
}

// ---------------------------------------------------------------- OCIO
void MainWindow::loadOcioConfig(const QString& path)
{
    std::string err;
    if (!m_pipeline.loadConfig(path.toUtf8().toStdString(), &err)) {
        QMessageBox::warning(this, tr("OCIO"), tr("Không nạp được config:\n%1").arg(QString::fromStdString(err)));
        return;
    }
    QSettings().setValue("ocioConfig", path);
    m_view->colorConfigChanged();
    syncPipelineB();
    populateColorCombos();
    render();
}

void MainWindow::populateColorCombos()
{
    m_guessCache.clear();
    m_updatingUi = true;
    QString oldInput = m_inputCombo->currentData().toString();
    QString oldDisplay = m_displayCombo->currentText();
    if (oldInput.isEmpty() && m_inputCombo->count() == 0) oldInput = QSettings().value("input").toString();
    if (oldDisplay.isEmpty()) oldDisplay = QSettings().value("display").toString();

    m_inputCombo->clear();
    m_inputCombo->addItem(tr("Tự động"), QString());
    for (auto& cs : m_pipeline.colorSpaces()) {
        QString n = QString::fromStdString(cs);
        m_inputCombo->addItem(n, n);
    }
    int ii = oldInput.isEmpty() ? 0 : m_inputCombo->findData(oldInput);
    m_inputCombo->setCurrentIndex(ii < 0 ? 0 : ii);

    m_displayCombo->clear();
    for (auto& dsp : m_pipeline.displays()) m_displayCombo->addItem(QString::fromStdString(dsp));
    int di = m_displayCombo->findText(oldDisplay);
    if (di < 0) di = m_displayCombo->findText(QString::fromStdString(m_pipeline.defaultDisplay()));
    m_displayCombo->setCurrentIndex(qMax(0, di));

    if (!m_pipeline.hasColorSpace(m_defaultInput.toStdString())) m_defaultInput.clear();
    m_updatingUi = false;
    populateViews();

    QString cfg = QString::fromStdString(m_pipeline.configName());
    m_configLabel->setText("OCIO: " + (cfg.size() > 48 ? cfg.left(45) + "…" : cfg));
    m_configLabel->setToolTip(QString::fromStdString(m_pipeline.configPath()));
    populateColorCombosB();
}

void MainWindow::populateViews()
{
    bool prev = m_updatingUi;
    m_updatingUi = true;
    QString old = m_viewCombo->currentText();
    if (old.isEmpty()) old = QSettings().value("view").toString();
    std::string dsp = m_displayCombo->currentText().toStdString();
    m_viewCombo->clear();
    for (auto& v : m_pipeline.views(dsp)) m_viewCombo->addItem(QString::fromStdString(v));
    int vi = m_viewCombo->findText(old);
    if (vi < 0) vi = m_viewCombo->findText(QString::fromStdString(m_pipeline.defaultView(dsp)));
    m_viewCombo->setCurrentIndex(qMax(0, vi));
    m_updatingUi = prev;
}

QString MainWindow::resolvedInput() const
{
    QString chosen = m_inputCombo->currentData().toString();
    if (!chosen.isEmpty()) return chosen;
    if (!m_lastGuess.isEmpty()) return m_lastGuess;
    if (!m_defaultInput.isEmpty()) return m_defaultInput;
    return QString::fromStdString(m_pipeline.sceneLinear());
}

exrv::ViewSettings MainWindow::currentViewSettings()
{
    exrv::ViewSettings vs;
    vs.input = resolvedInput().toStdString();
    vs.display = m_displayCombo->currentText().toStdString();
    vs.view = m_viewCombo->currentText().toStdString();
    vs.exposure = (float)m_expSpin->value();
    vs.gamma = (float)m_gammaSpin->value();
    vs.channel = (exrv::ChannelMode)m_channelCombo->currentIndex();
    return vs;
}

void MainWindow::setDefaultInput()
{
    QString cs = resolvedInput();
    m_defaultInput = cs;
    QSettings().setValue("defaultInput", cs);
    statusBar()->showMessage(tr("Input mặc định: %1").arg(cs), 4000);
    render();
}

// ---------------------------------------------------------------- Mở file / sequence
void MainWindow::openFile(const QString& path)
{
    QFileInfo fi(path);
    if (!fi.exists()) {
        QMessageBox::warning(this, "ExrView", tr("Không tìm thấy file:\n%1").arg(path));
        return;
    }
    if (m_playTimer.isActive()) togglePlay();
    QSettings().setValue("lastDir", fi.absolutePath());
    addRecent(fi.absoluteFilePath());
    m_layerRef = LayerRef();
    m_guessCache.clear();
    m_layerSignature.clear();
    m_frame.reset();
    if (m_framesB.isEmpty()) {
        m_layerRefB = LayerRef();
        m_layerSignatureB.clear();
        m_frameB.reset();
    }
    m_inFrame = 0;
    m_outFrame = -1;
    m_playDir = 1;
    detectSequence(fi.absoluteFilePath(), m_frames, m_frameNumbers, m_index);
    m_seqBar->setVisible(m_frames.size() > 1);
    m_updatingUi = true;
    m_seqSlider->setRange(0, qMax(0, m_frames.size() - 1));
    m_seqSlider->setValue(m_index);
    m_seqSlider->setInOutRange(0, -1);
    m_updatingUi = false;
    updateTimelineCache();
    showIndex(m_index);
}

void MainWindow::openFileB(const QString& path)
{
    QFileInfo fi(path);
    if (!fi.exists()) {
        QMessageBox::warning(this, "ExrView", tr("Không tìm thấy file B:\n%1").arg(path));
        return;
    }
    QSettings().setValue("lastDir", fi.absolutePath());
    m_layerRefB = LayerRef();
    m_layerSignatureB.clear();
    m_frameB.reset();
    int dummyIdx = 0;
    detectSequence(fi.absoluteFilePath(), m_framesB, m_frameNumbersB, dummyIdx);
    if (!m_compare) {
        m_bInitialized = true; // không ghi đè file B bằng copyAtoB
        m_compareAct->setChecked(true);
    } else {
        showIndexB(m_index);
    }
}

void MainWindow::detectSequence(const QString& path, QStringList& outFrames, QVector<long long>& outNumbers, int& outIndex)
{
    outFrames.clear();
    outNumbers.clear();
    QFileInfo fi(path);
    std::string prefix, suffix;
    long long frame;
    int pad;
    if (exrv::splitFrameNumber(fi.fileName().toStdString(), prefix, frame, pad, suffix)) {
        QDir dir = fi.absoluteDir();
        QString pre = QString::fromStdString(prefix), suf = QString::fromStdString(suffix);
        QStringList entries = dir.entryList({pre + "*" + suf}, QDir::Files);
        QVector<QPair<long long, QString>> found;
        for (const QString& e : entries) {
            std::string p2, s2;
            long long f2;
            int pad2;
            if (!exrv::splitFrameNumber(e.toStdString(), p2, f2, pad2, s2)) continue;
            if (QString::fromStdString(p2) != pre || QString::fromStdString(s2) != suf) continue;
            if (pad2 != pad && QString::number(f2).size() != pad2) continue;
            found.push_back({f2, dir.absoluteFilePath(e)});
        }
        std::sort(found.begin(), found.end());
        if (found.size() > 1) {
            for (auto& p : found) {
                outNumbers.push_back(p.first);
                outFrames.push_back(p.second);
            }
        }
    }
    if (outFrames.isEmpty()) {
        outFrames << fi.absoluteFilePath();
        outNumbers << 0;
    }
    outIndex = std::max(0, (int)outFrames.indexOf(fi.absoluteFilePath()));
}

QString MainWindow::keyForIndex(int i) const
{
    return (i >= 0 && i < m_frames.size()) ? m_loader.key(m_frames[i], m_layerRef) : QString();
}

void MainWindow::showIndex(int i)
{
    if (m_frames.isEmpty()) return;
    i = qBound(0, i, m_frames.size() - 1);
    m_index = i;
    m_updatingUi = true;
    m_seqSlider->setValue(i);
    m_updatingUi = false;
    m_frameLabel->setText(QString("%1  (%2/%3)")
                              .arg(m_frameNumbers.value(i))
                              .arg(i + 1)
                              .arg(m_frames.size()));
    QString k = keyForIndex(i);
    m_wantedKey = k;
    if (FramePtr f = m_loader.cached(k)) {
        setFrame(f);
    } else {
        m_loader.request(m_frames[i], m_layerRef, 10); // ưu tiên cao nhất cho frame đang xem
        if (!m_frame) m_view->setMessage(tr("Đang đọc %1…").arg(QFileInfo(m_frames[i]).fileName()));
    }
    if (m_compare) showIndexB(i);
    if (!m_playTimer.isActive()) prefetchAround(i, 3, 1);
}

void MainWindow::showIndexB(int i)
{
    if (!m_compare) return;
    const QStringList& srcB = m_framesB.isEmpty() ? m_frames : m_framesB;
    if (srcB.isEmpty()) return;
    int bi = qBound(0, i, srcB.size() - 1);
    QString k = m_loader.key(srcB[bi], m_layerRefB);
    m_wantedKeyB = k;
    if (FramePtr f = m_loader.cached(k)) {
        setFrameB(f);
    } else {
        m_loader.request(srcB[bi], m_layerRefB, 10);
        if (!m_frameB) m_viewB->setMessage(tr("Đang đọc %1…").arg(QFileInfo(srcB[bi]).fileName()));
    }
}

void MainWindow::prefetchAround(int i, int ahead, int behind)
{
    if (m_frames.size() < 2) return;
    const QStringList& srcB = m_framesB.isEmpty() ? m_frames : m_framesB;
    auto req = [this, &srcB](int j, int prio) {
        m_loader.request(m_frames[j], m_layerRef, prio);
        if (m_compare && !srcB.isEmpty())
            m_loader.request(srcB[qBound(0, j, srcB.size() - 1)], m_layerRefB, prio);
    };
    const int in = effectiveIn(), out = effectiveOut();
    const int span = qMax(1, out - in + 1);
    for (int k = 1; k <= ahead; ++k) {
        int idx = in + ((i - in + k * m_playDir) % span + span) % span;
        req(idx, 5 - k);
    }
    for (int k = 1; k <= behind; ++k)
        if (i - k >= 0) req(i - k, -k);
}

void MainWindow::updateTimelineCache()
{
    if (m_frames.size() <= 1) return;
    QVector<bool> mask(m_frames.size(), false);
    for (int i = 0; i < m_frames.size(); ++i)
        mask[i] = m_loader.isCached(m_loader.key(m_frames[i], m_layerRef));
    m_seqSlider->setCachedMask(mask);
}

void MainWindow::onLoaded(const QString& key, FramePtr f)
{
    m_cacheLabel->setText(tr("Cache %1 / %2 MB").arg(m_loader.usedMB()).arg(m_loader.budgetMB()));
    updateTimelineCache();
    if (m_compare && key == m_wantedKeyB) {
        if (!f->error.isEmpty()) {
            m_frameB.reset();
            m_viewB->clearSource();
            m_view->setSecondSource(nullptr);
            m_viewB->setMessage(tr("Không đọc được %1\n\n%2").arg(QFileInfo(f->path).fileName(), f->error));
        } else {
            setFrameB(f);
        }
    }
    if (key != m_wantedKey) return;
    if (!f->error.isEmpty()) {
        m_frame.reset();
        m_view->clearSource();
        m_view->setMessage(tr("Không đọc được %1\n\n%2").arg(QFileInfo(f->path).fileName(), f->error));
        updateTitle();
        return;
    }
    setFrame(f);
}

QString MainWindow::autoInputFor(const Frame& f, bool* dataLayer) const
{
    const QString key = QString::fromStdString(f.csAttr) + QChar(0x1f) +
                        (f.hasChroma ? QString::fromLatin1(QByteArray((const char*)f.chroma, sizeof(f.chroma)).toHex())
                                     : QString()) +
                        QChar(0x1f) + QString::number(f.part) + QChar(0x1f) + QString::number(f.layer) + QChar(0x1f) +
                        QString::fromStdString(f.info.parts[f.part].layers[f.layer].name);
    auto hit = m_guessCache.constFind(key);
    if (hit != m_guessCache.constEnd()) {
        if (dataLayer) *dataLayer = hit->second;
        return hit->first;
    }
    QString guess = QString::fromStdString(
        m_pipeline.guessInput(f.csAttr, f.hasChroma ? f.chroma : nullptr, QFileInfo(f.path).fileName().toStdString()));
    bool data = true;
    const auto& L = f.info.parts[f.part].layers[f.layer];
    for (const auto& c : L.channels) {
        QString b = QString::fromStdString(c).section('.', -1).toLower();
        if (b == "r" || b == "g" || b == "b" || b == "red" || b == "green" || b == "blue" ||
            (b == "y" && L.name == "RGBA"))
            data = false;
    }
    QString dataCs = QString::fromStdString(m_pipeline.canonicalColorSpace("data"));
    if (data && !dataCs.isEmpty()) guess = dataCs;
    const bool isData = data && !dataCs.isEmpty();
    m_guessCache.insert(key, qMakePair(guess, isData));
    if (dataLayer) *dataLayer = isData;
    return guess;
}

void MainWindow::setFrame(FramePtr f)
{
    bool newFile = !m_frame;
    bool sameSize = m_frame && m_frame->image && f->image && m_frame->image->width == f->image->width &&
                    m_frame->image->height == f->image->height;
    m_frame = f;
    bool dataLayer = false;
    m_lastGuess = autoInputFor(*f, &dataLayer);

    QString autoName = !m_lastGuess.isEmpty() ? m_lastGuess
                                              : (!m_defaultInput.isEmpty() ? m_defaultInput
                                                                           : QString::fromStdString(m_pipeline.sceneLinear()));
    m_inputCombo->setItemText(0, tr("Tự động → %1%2").arg(autoName, m_lastGuess.isEmpty() ? tr(" (mặc định)") : (dataLayer ? tr(" (layer dữ liệu)") : "")));

    populateLayers();
    if (!m_playTimer.isActive()) {
        updateInfoDock();
        updateHistogram();
    }
    m_view->setSource(f->image, !newFile && sameSize);
    render();
    const auto& P = f->info.parts[f->part];
    m_infoLabel->setText(QString("%1×%2 · %3 · %4")
                             .arg(P.width)
                             .arg(P.height)
                             .arg(QString::fromStdString(P.compression))
                             .arg(humanSize(f->fileSize)));
    if (!f->warning.isEmpty()) m_view->setMessage(f->warning);
    updateTitle();
}

void MainWindow::setFrameB(FramePtr f)
{
    bool newFile = !m_frameB;
    bool sameSize = m_frameB && m_frameB->image && f->image && m_frameB->image->width == f->image->width &&
                    m_frameB->image->height == f->image->height;
    m_frameB = f;
    bool dataLayer = false;
    m_lastGuessB = autoInputFor(*f, &dataLayer);
    QString autoName = !m_lastGuessB.isEmpty() ? m_lastGuessB
                                               : (!m_defaultInput.isEmpty() ? m_defaultInput
                                                                            : QString::fromStdString(m_pipelineB.sceneLinear()));
    m_bInputCombo->setItemText(0, tr("Tự động → %1%2").arg(autoName, m_lastGuessB.isEmpty() ? tr(" (mặc định)") : (dataLayer ? tr(" (layer dữ liệu)") : "")));
    fillLayerCombo(m_bLayerCombo, m_layerSignatureB, *f);
    m_syncingView = true;
    m_viewB->setSource(f->image, !newFile && sameSize);
    m_syncingView = false;
    if (newFile || !sameSize) m_viewB->setViewTransform(m_view->zoom(), m_view->viewOffset(), m_view->fitMode());
    if (m_view->compareMode() != ImageView::CompareMode::Off) m_view->setSecondSource(f->image);
    renderB();
    if (!f->warning.isEmpty()) m_viewB->setMessage(f->warning);
}

void MainWindow::populateLayers()
{
    if (m_frame) fillLayerCombo(m_layerCombo, m_layerSignature, *m_frame);
}

void MainWindow::fillLayerCombo(QComboBox* combo, QString& signature, const Frame& f)
{
    const auto& parts = f.info.parts;
    QString sig;
    for (auto& p : parts) {
        sig += QString::fromStdString(p.name) + "|";
        for (auto& l : p.layers) sig += QString::fromStdString(l.name) + ",";
    }
    if (sig != signature) {
        signature = sig;
        combo->clear();
        bool multi = parts.size() > 1;
        for (int p = 0; p < (int)parts.size(); ++p) {
            if (!parts[p].supported) continue;
            for (int l = 0; l < (int)parts[p].layers.size(); ++l) {
                const auto& L = parts[p].layers[l];
                QStringList chans;
                for (auto& c : L.channels) {
                    QString s = QString::fromStdString(c);
                    chans << s.mid(s.lastIndexOf('.') + 1);
                }
                QString text = QString::fromStdString(L.name) + "  [" + chans.join(",") + "]";
                if (multi) {
                    QString pn = parts[p].name.empty() ? QString("part %1").arg(p) : QString::fromStdString(parts[p].name);
                    text = pn + " / " + text;
                }
                combo->addItem(text, QPoint(p, l));
            }
        }
    }
    int idx = combo->findData(QPoint(f.part, f.layer));
    if (idx >= 0) combo->setCurrentIndex(idx);
}

// ---------------------------------------------------------------- Export / Clipboard
QImage MainWindow::renderCurrentToQImage() const
{
    if (!m_frame || !m_frame->image || m_frame->image->empty()) return QImage();
    std::vector<uint8_t> bgra;
    std::string err;
    exrv::ViewSettings vs = const_cast<MainWindow*>(this)->currentViewSettings();
    if (!const_cast<exrv::ColorPipeline&>(m_pipeline).render(*m_frame->image, vs, bgra, &err)) {
        return QImage();
    }
    QImage img((const uchar*)bgra.data(), m_frame->image->width, m_frame->image->height,
               m_frame->image->width * 4, QImage::Format_RGB32);
    QImage out = img.copy();
    if (m_view->rot90() != 0) {
        QTransform tr;
        tr.rotate(m_view->rot90() * 90);
        out = out.transformed(tr);
    }
    if (m_view->flipH() || m_view->flipV()) out = out.mirrored(m_view->flipH(), m_view->flipV());
    return out;
}

void MainWindow::exportImage()
{
    if (!m_frame || !m_frame->image) return;
    QFileInfo fi(m_frame->path);
    QString defPath = fi.absolutePath() + "/" + fi.completeBaseName() + ".png";
    QString outPath = QFileDialog::getSaveFileName(
        this, tr("Xuất ảnh đã áp dụng OCIO"), defPath,
        tr("PNG (*.png);;JPEG (*.jpg *.jpeg);;TIFF (*.tif *.tiff);;BMP (*.bmp);;WebP (*.webp)"));
    if (outPath.isEmpty()) return;
    QImage img = renderCurrentToQImage();
    if (img.isNull() || !img.save(outPath)) {
        QMessageBox::warning(this, tr("Xuất ảnh"), tr("Không ghi được file:\n%1").arg(outPath));
        return;
    }
    statusBar()->showMessage(tr("Đã xuất: %1").arg(QFileInfo(outPath).fileName()), 4000);
}

void MainWindow::copyImageToClipboard()
{
    QImage img = renderCurrentToQImage();
    if (img.isNull()) return;
    QApplication::clipboard()->setImage(img);
    statusBar()->showMessage(tr("Đã chép ảnh (%1×%2) vào Clipboard").arg(img.width()).arg(img.height()), 3000);
}

void MainWindow::copyPixelToClipboard()
{
    if (!m_frame || !m_frame->image || !m_hoverInside) return;
    const auto& im = *m_frame->image;
    if (m_hoverX < 0 || m_hoverY < 0 || m_hoverX >= im.width || m_hoverY >= im.height) return;
    float p[4];
    im.pixel(m_hoverX, m_hoverY, p);
    QString txt = QString("(%1, %2) R=%3 G=%4 B=%5 A=%6")
                      .arg(m_hoverX)
                      .arg(m_hoverY)
                      .arg(fmtFloat(p[0]), fmtFloat(p[1]), fmtFloat(p[2]), fmtFloat(p[3]));
    QApplication::clipboard()->setText(txt);
    statusBar()->showMessage(tr("Đã chép pixel: %1").arg(txt), 3000);
}

// ---------------------------------------------------------------- So sánh A | B
void MainWindow::setCompare(bool on)
{
    if (on == m_compare) return;
    m_compare = on;
    if (on) ensurePipelineB();
    m_bBar->setVisible(on);
    applyCompareLayout();
    if (!on) {
        m_frameB.reset();
        m_wantedKeyB.clear();
        m_viewB->clearSource();
        if (m_hoverInside) updateHover(m_hoverX, m_hoverY, true);
        return;
    }
    if (!m_bInitialized) {
        copyAtoB();
        m_bInitialized = true;
    } else {
        showIndexB(m_index);
    }
}

void MainWindow::setCompareViewMode(int mode)
{
    if (m_compare && m_bModeCombo->currentIndex() == mode) mode = 0;
    m_bModeCombo->setCurrentIndex(mode);
    if (!m_compare) m_compareAct->setChecked(true);
}

void MainWindow::applyCompareLayout()
{
    const int mode = m_compare ? m_bModeCombo->currentIndex() : 0;
    const bool single = m_compare && mode != 0;
    m_view->setLabel(m_compare && !single ? "A" : QString());
    m_view->setCompareMode(!m_compare || mode == 0 ? ImageView::CompareMode::Off
                           : mode == 1             ? ImageView::CompareMode::Wipe
                                                   : ImageView::CompareMode::Difference);
    if (single) {
        m_viewB->hide();
        m_view->setSecondSource(m_frameB ? m_frameB->image : nullptr);
        m_view->setSecondViewSettings(viewSettingsB());
    } else {
        m_viewB->setVisible(m_compare);
        m_splitter->setOrientation(
            (Qt::Orientation)QSettings().value("compareOrientation", (int)Qt::Horizontal).toInt());
        if (m_compare) {
            int total = m_splitter->orientation() == Qt::Horizontal ? m_splitter->width() : m_splitter->height();
            m_splitter->setSizes({total / 2, total - total / 2});
            m_viewB->colorConfigChanged();
            if (m_frameB) {
                m_syncingView = true;
                m_viewB->setSource(m_frameB->image, false);
                m_syncingView = false;
                m_viewB->setViewTransform(m_view->zoom(), m_view->viewOffset(), m_view->fitMode());
            }
        }
    }
    if (m_hoverInside) updateHover(m_hoverX, m_hoverY, true);
}

void MainWindow::copyAtoB()
{
    m_framesB.clear();
    m_frameNumbersB.clear();
    m_updatingUi = true;
    m_bChannelCombo->setCurrentIndex(m_channelCombo->currentIndex());
    int ii = m_bInputCombo->findData(m_inputCombo->currentData());
    m_bInputCombo->setCurrentIndex(qMax(0, ii));
    int di = m_bDisplayCombo->findText(m_displayCombo->currentText());
    if (di >= 0) m_bDisplayCombo->setCurrentIndex(di);
    m_updatingUi = false;
    populateViewsB();
    m_updatingUi = true;
    int vi = m_bViewCombo->findText(m_viewCombo->currentText());
    if (vi >= 0) m_bViewCombo->setCurrentIndex(vi);
    m_bExpSpin->setValue(m_expSpin->value());
    m_updatingUi = false;
    m_layerRefB = m_layerRef;
    if (m_frame && m_layerRefB.part < 0) {
        m_layerRefB.part = m_frame->part;
        m_layerRefB.partName = QString::fromStdString(m_frame->info.parts[m_frame->part].name);
        m_layerRefB.layerName = QString::fromStdString(m_frame->info.parts[m_frame->part].layers[m_frame->layer].name);
    }
    if (m_compare) {
        showIndexB(m_index);
        renderB();
    }
}

void MainWindow::ensurePipelineB()
{
    if (m_bPipelineReady) return;
    m_bPipelineReady = true;
    syncPipelineB();
    populateColorCombosB();
}

void MainWindow::syncPipelineB()
{
    if (!m_bPipelineReady) return;
    std::string err;
    if (!m_pipelineB.loadConfig(m_pipeline.configPath(), &err)) m_pipelineB.loadConfig("", &err);
    if (m_viewB) m_viewB->colorConfigChanged();
    if (m_view) m_view->secondColorConfigChanged();
}

void MainWindow::populateColorCombosB()
{
    if (!m_bInputCombo || !m_bPipelineReady) return;
    m_updatingUi = true;
    QString oldInput = m_bInputCombo->currentData().toString();
    QString oldDisplay = m_bDisplayCombo->currentText();
    if (m_bInputCombo->count() == 0) oldInput = m_inputCombo->currentData().toString();
    if (oldDisplay.isEmpty()) oldDisplay = m_displayCombo->currentText();

    m_bInputCombo->clear();
    m_bInputCombo->addItem(tr("Tự động"), QString());
    for (auto& cs : m_pipelineB.colorSpaces()) {
        QString n = QString::fromStdString(cs);
        m_bInputCombo->addItem(n, n);
    }
    int ii = oldInput.isEmpty() ? 0 : m_bInputCombo->findData(oldInput);
    m_bInputCombo->setCurrentIndex(ii < 0 ? 0 : ii);

    m_bDisplayCombo->clear();
    for (auto& dsp : m_pipelineB.displays()) m_bDisplayCombo->addItem(QString::fromStdString(dsp));
    int di = m_bDisplayCombo->findText(oldDisplay);
    if (di < 0) di = m_bDisplayCombo->findText(QString::fromStdString(m_pipelineB.defaultDisplay()));
    m_bDisplayCombo->setCurrentIndex(qMax(0, di));
    m_updatingUi = false;
    populateViewsB();
    renderB();
}

void MainWindow::populateViewsB()
{
    bool prev = m_updatingUi;
    m_updatingUi = true;
    QString old = m_bViewCombo->currentText();
    if (old.isEmpty()) old = m_viewCombo->currentText();
    std::string dsp = m_bDisplayCombo->currentText().toStdString();
    m_bViewCombo->clear();
    for (auto& v : m_pipelineB.views(dsp)) m_bViewCombo->addItem(QString::fromStdString(v));
    int vi = m_bViewCombo->findText(old);
    if (vi < 0) vi = m_bViewCombo->findText(QString::fromStdString(m_pipelineB.defaultView(dsp)));
    m_bViewCombo->setCurrentIndex(qMax(0, vi));
    m_updatingUi = prev;
}

QString MainWindow::resolvedInputB() const
{
    QString chosen = m_bInputCombo->currentData().toString();
    if (!chosen.isEmpty()) return chosen;
    if (!m_lastGuessB.isEmpty()) return m_lastGuessB;
    if (!m_defaultInput.isEmpty()) return m_defaultInput;
    return QString::fromStdString(m_pipelineB.sceneLinear());
}

exrv::ViewSettings MainWindow::viewSettingsB()
{
    exrv::ViewSettings vs;
    vs.input = resolvedInputB().toStdString();
    vs.display = m_bDisplayCombo->currentText().toStdString();
    vs.view = m_bViewCombo->currentText().toStdString();
    vs.exposure = (float)m_bExpSpin->value();
    vs.gamma = (float)m_gammaSpin->value();
    vs.channel = (exrv::ChannelMode)m_bChannelCombo->currentIndex();
    return vs;
}

void MainWindow::renderB()
{
    if (!m_compare || m_updatingUi) return;
    const exrv::ViewSettings vs = viewSettingsB();
    m_viewB->setViewSettings(vs);
    m_view->setSecondViewSettings(vs);
    if (m_hoverInside) updateHover(m_hoverX, m_hoverY, true);
}

void MainWindow::updateInfoDock()
{
    if (!m_infoDock->isVisible()) return;
    m_infoTree->clear();
    if (!m_frame) return;
    auto add = [](QTreeWidgetItem* parent, const QString& k, const QString& v) {
        auto* it = new QTreeWidgetItem(parent, {k, v});
        it->setToolTip(1, v);
        return it;
    };
    auto* fileItem = new QTreeWidgetItem(m_infoTree, {tr("File"), QFileInfo(m_frame->path).fileName()});
    add(fileItem, tr("Đường dẫn"), m_frame->path);
    add(fileItem, tr("Dung lượng"), humanSize(m_frame->fileSize));
    add(fileItem, tr("Input color space"), resolvedInput());
    if (!m_frame->csAttr.empty()) add(fileItem, tr("Color space trong header"), QString::fromStdString(m_frame->csAttr));
    fileItem->setExpanded(true);
    const auto& parts = m_frame->info.parts;
    for (int p = 0; p < (int)parts.size(); ++p) {
        const auto& P = parts[p];
        auto* pi = new QTreeWidgetItem(m_infoTree, {tr("Part %1").arg(p), QString::fromStdString(P.name)});
        add(pi, tr("Loại"), QString::fromStdString(P.type) + (P.supported ? "" : tr(" (chưa hỗ trợ)")));
        add(pi, tr("Kích thước"), QString("%1 × %2").arg(P.width).arg(P.height));
        add(pi, tr("Data window"),
            QString("(%1, %2) – (%3, %4)").arg(P.dataX0).arg(P.dataY0).arg(P.dataX1).arg(P.dataY1));
        add(pi, tr("Nén"), QString::fromStdString(P.compression));
        auto* li = add(pi, tr("Layers"), QString::number(P.layers.size()));
        for (auto& L : P.layers) {
            QStringList ch;
            for (auto& c : L.channels) ch << QString::fromStdString(c);
            add(li, QString::fromStdString(L.name), ch.join("  "));
        }
        auto* ai = add(pi, tr("Attributes"), QString::number(P.attributes.size()));
        for (auto& a : P.attributes)
            add(ai, QString::fromStdString(a.name), QString::fromStdString(a.value))
                ->setToolTip(0, QString::fromStdString(a.type));
        pi->setExpanded(true);
        li->setExpanded(true);
        ai->setExpanded(true);
    }
}

void MainWindow::updateHistogram()
{
    if (!m_histDock || !m_histDock->isVisible()) return;
    if (!m_frame || !m_frame->image) {
        m_histDock->clear();
        return;
    }
    m_histDock->setImage(m_frame->image, (float)m_expSpin->value());
}

void MainWindow::render()
{
    if (m_updatingUi) return;
    m_view->setViewSettings(currentViewSettings());
    if (m_compare) {
        const exrv::ViewSettings vsB = viewSettingsB();
        m_viewB->setViewSettings(vsB);
        m_view->setSecondViewSettings(vsB);
    }
    if (m_hoverInside) updateHover(m_hoverX, m_hoverY, true);
    if (!m_playTimer.isActive()) updateHistogram();
    if (m_infoDock->isVisible() && !m_playTimer.isActive()) {
        auto items = m_infoTree->findItems(tr("Input color space"), Qt::MatchExactly | Qt::MatchRecursive);
        for (auto* it : items) it->setText(1, resolvedInput());
    }
}

void MainWindow::updateHover(int x, int y, bool inside)
{
    m_hoverX = x;
    m_hoverY = y;
    m_hoverInside = inside;
    auto sample = [x, y](const FramePtr& f, exrv::ColorPipeline& pipe, const exrv::ViewSettings& vs,
                         QString& coords) -> QString {
        if (!f || !f->image) return QString();
        const auto& im = *f->image;
        if (x < 0 || y < 0 || x >= im.width || y >= im.height) return QString();
        float p[4];
        im.pixel(x, y, p);
        exrv::Image one;
        one.width = one.height = 1;
        one.rgba.assign(p, p + 4);
        std::vector<uint8_t> out;
        QRgb d = 0;
        if (pipe.render(one, vs, out, nullptr)) d = qRgb(out[2], out[1], out[0]);
        const auto& P = f->info.parts[f->part];
        if (coords.isEmpty())
            coords = QString("x %1  y %2").arg(x - im.dispX + P.dispX, 5).arg(y - im.dispY + P.dispY, 5);
        return QString("R %1  G %2  B %3  A %4  →  %5 %6 %7")
            .arg(fmtFloat(p[0]), 10)
            .arg(fmtFloat(p[1]), 10)
            .arg(fmtFloat(p[2]), 10)
            .arg(fmtFloat(p[3]), 8)
            .arg(qRed(d), 3)
            .arg(qGreen(d), 3)
            .arg(qBlue(d), 3);
    };
    if (!inside) {
        m_pixelLabel->clear();
        return;
    }
    QString coords;
    QString a = sample(m_frame, m_pipeline, currentViewSettings(), coords);
    if (!m_compare) {
        if (!a.isEmpty()) m_pixelLabel->setText(coords + "   " + a);
        else m_pixelLabel->clear();
        return;
    }
    QString b = sample(m_frameB, m_pipelineB, viewSettingsB(), coords);
    m_pixelLabel->setText(coords + "   A: " + (a.isEmpty() ? QStringLiteral("—") : a) + "    B: " +
                          (b.isEmpty() ? QStringLiteral("—") : b));
}

void MainWindow::onRegionSelected(const QRect& r)
{
    if (!m_frame || !m_frame->image || r.isEmpty()) return;
    const auto& im = *m_frame->image;
    int x0 = qBound(0, r.left(), im.width - 1);
    int y0 = qBound(0, r.top(), im.height - 1);
    int x1 = qBound(x0, r.right(), im.width - 1);
    int y1 = qBound(y0, r.bottom(), im.height - 1);

    double sum[4] = {0, 0, 0, 0};
    float mn[3] = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                   std::numeric_limits<float>::infinity()};
    float mx[3] = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                   -std::numeric_limits<float>::infinity()};
    size_t count = 0;
    float p[4];
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            im.pixel(x, y, p);
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
            for (int c = 0; c < 4; ++c) sum[c] += p[c];
            for (int c = 0; c < 3; ++c) {
                mn[c] = std::min(mn[c], p[c]);
                mx[c] = std::max(mx[c], p[c]);
            }
            ++count;
        }
    }
    if (count == 0) return;
    const double inv = 1.0 / (double)count;
    m_pixelLabel->setText(
        tr("Vùng %1×%2 (%3 px)   Mean: R %4  G %5  B %6  A %7   |   Min: %8 %9 %10   Max: %11 %12 %13")
            .arg(x1 - x0 + 1)
            .arg(y1 - y0 + 1)
            .arg(count)
            .arg(sum[0] * inv, 0, 'f', 4)
            .arg(sum[1] * inv, 0, 'f', 4)
            .arg(sum[2] * inv, 0, 'f', 4)
            .arg(sum[3] * inv, 0, 'f', 3)
            .arg(mn[0], 0, 'f', 3)
            .arg(mn[1], 0, 'f', 3)
            .arg(mn[2], 0, 'f', 3)
            .arg(mx[0], 0, 'f', 3)
            .arg(mx[1], 0, 'f', 3)
            .arg(mx[2], 0, 'f', 3));
}

void MainWindow::updateTitle()
{
    if (!m_frame) {
        setWindowTitle("ExrView");
        return;
    }
    setWindowTitle(QFileInfo(m_frame->path).fileName() + " — ExrView");
}

// ---------------------------------------------------------------- Play
int MainWindow::effectiveIn() const
{
    return qBound(0, m_inFrame, qMax(0, m_frames.size() - 1));
}

int MainWindow::effectiveOut() const
{
    if (m_frames.isEmpty()) return 0;
    return (m_outFrame < 0) ? (m_frames.size() - 1) : qBound(effectiveIn(), m_outFrame, m_frames.size() - 1);
}

void MainWindow::cycleLoopMode()
{
    int m = ((int)m_loopMode + 1) % 3;
    m_loopMode = (LoopMode)m;
    if (m_loopMode == LoopMode::Loop) {
        m_loopBtn->setText("🔁");
        m_playDir = 1;
    } else if (m_loopMode == LoopMode::PingPong) {
        m_loopBtn->setText("⇄");
    } else {
        m_loopBtn->setText("→|");
        m_playDir = 1;
    }
}

void MainWindow::togglePlay()
{
    if (m_frames.size() < 2) return;
    if (m_playTimer.isActive()) {
        m_playTimer.stop();
        m_playBtn->setText("▶");
        m_view->setPlayback(false);
        m_viewB->setPlayback(false);
        updateInfoDock();
        updateHistogram();
        m_frameLabel->setToolTip(QString());
        return;
    }
    m_playBtn->setText("⏸");
    m_fpsClock.restart();
    m_fpsFrames = 0;
    m_view->setPlayback(true);
    m_viewB->setPlayback(true);
    m_playClock.restart();
    m_nextDueNs = 0;
    m_playTimer.start(playTickMs());
    prefetchAround(m_index, 8, 0);
}

int MainWindow::playTickMs() const
{
    return qBound(1, (int)(1000.0 / m_fpsSpin->value() / 4.0), 10);
}

void MainWindow::tick()
{
    if (m_frames.size() < 2) return;
    const qint64 now = m_playClock.nsecsElapsed();
    if (now < m_nextDueNs) return;

    const int in = effectiveIn();
    const int out = effectiveOut();
    int next = m_index + m_playDir;
    if (m_loopMode == LoopMode::Loop) {
        if (next > out) next = in;
        else if (next < in) next = out;
    } else if (m_loopMode == LoopMode::PingPong) {
        if (next > out) {
            m_playDir = -1;
            next = qMax(in, out - 1);
        } else if (next < in) {
            m_playDir = 1;
            next = qMin(out, in + 1);
        }
    } else {
        if (next > out) {
            togglePlay();
            return;
        }
    }

    if (!m_loader.cached(keyForIndex(next))) return;
    if (m_compare) {
        const QStringList& srcB = m_framesB.isEmpty() ? m_frames : m_framesB;
        int bi = qBound(0, next, srcB.size() - 1);
        if (!m_loader.cached(m_loader.key(srcB[bi], m_layerRefB))) return;
    }
    showIndex(next);
    prefetchAround(next, 8, 0);
    const qint64 period = (qint64)(1e9 / m_fpsSpin->value());
    m_nextDueNs += period;
    if (m_nextDueNs + period < now) m_nextDueNs = now + period;
    ++m_fpsFrames;
    if (m_fpsClock.elapsed() > 1000) {
        m_actualFps = m_fpsFrames * 1000.0 / m_fpsClock.elapsed();
        m_fpsClock.restart();
        m_fpsFrames = 0;
        m_cacheLabel->setText(tr("%1 fps · cache %2 MB").arg(m_actualFps, 0, 'f', 1).arg(m_loader.usedMB()));
    }
}

void MainWindow::stepFrame(int d)
{
    if (m_frames.size() < 2) return;
    if (m_playTimer.isActive()) togglePlay();
    int n = m_frames.size();
    showIndex(((m_index + d) % n + n) % n);
}

void MainWindow::keyPressEvent(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Left: stepFrame(-1); return;
    case Qt::Key_Right: stepFrame(1); return;
    case Qt::Key_Space: togglePlay(); return;
    case Qt::Key_Home: showIndex(effectiveIn()); return;
    case Qt::Key_End: showIndex(effectiveOut()); return;
    default: QMainWindow::keyPressEvent(e);
    }
}

// ---------------------------------------------------------------- Explorer
void MainWindow::applyToExplorerPreview()
{
#ifdef Q_OS_WIN
    QSettings reg("HKEY_CURRENT_USER\\Software\\ExrView\\Preview", QSettings::NativeFormat);
#else
    QSettings reg("ExrView", "Preview");
#endif
    reg.setValue("OcioConfig", QString::fromStdString(m_pipeline.configPath()));
    reg.setValue("Display", m_displayCombo->currentText());
    reg.setValue("View", m_viewCombo->currentText());
    reg.setValue("Exposure", QString::number(m_expSpin->value()));
    reg.setValue("Gamma", QString::number(m_gammaSpin->value()));
    reg.setValue("DefaultInput", m_defaultInput);
    reg.sync();
    QMessageBox::information(
        this, tr("Explorer"),
        tr("Đã lưu: %1 / %2, exposure %3, gamma %4.\n\n"
           "Preview pane (Alt+P) dùng ngay cài đặt mới. Thumbnail đã tạo trước đó được Windows lưu cache — "
           "để làm mới, chạy Disk Cleanup và chọn \"Thumbnails\".")
            .arg(m_displayCombo->currentText(), m_viewCombo->currentText())
            .arg(m_expSpin->value())
            .arg(m_gammaSpin->value()));
}

void MainWindow::registerShellExtension(bool unregister)
{
#ifdef Q_OS_WIN
    QString dll = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/ExrViewShell.dll");
    if (!QFileInfo::exists(dll)) {
        QMessageBox::warning(this, "ExrView", tr("Không tìm thấy %1").arg(dll));
        return;
    }
    QString args = (unregister ? "/u \"" : "\"") + dll + "\"";
    HINSTANCE h = ShellExecuteW((HWND)winId(), L"runas", L"regsvr32.exe", (LPCWSTR)args.utf16(), nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)h <= 32) QMessageBox::warning(this, "ExrView", tr("Không chạy được regsvr32 (mã %1)").arg((INT_PTR)h));
#else
    Q_UNUSED(unregister);
#endif
}

// ---------------------------------------------------------------- Settings
void MainWindow::addRecent(const QString& path)
{
    QSettings s;
    QStringList r = s.value("recent").toStringList();
    r.removeAll(path);
    r.prepend(path);
    while (r.size() > 10) r.removeLast();
    s.setValue("recent", r);
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu()
{
    m_recentMenu->clear();
    for (const QString& f : QSettings().value("recent").toStringList())
        m_recentMenu->addAction(QDir::toNativeSeparators(f), this, [this, f] { openFile(f); });
    m_recentMenu->setEnabled(!m_recentMenu->isEmpty());
}

void MainWindow::loadSettings()
{
    QSettings s;
    m_defaultInput = s.value("defaultInput").toString();
    QString cfg = s.value("ocioConfig").toString();
    std::string err;
    if (!cfg.isEmpty() && !m_pipeline.loadConfig(cfg.toUtf8().toStdString(), &err)) {
        statusBar()->showMessage(tr("Không nạp được OCIO config đã lưu: %1").arg(QString::fromStdString(err)), 8000);
        m_pipeline.loadConfig("", &err);
    }
    syncPipelineB();
    populateColorCombos();
    m_updatingUi = true;
    m_expSpin->setValue(s.value("exposure", 0.0).toDouble());
    m_expSlider->setValue(qRound(m_expSpin->value() * 10));
    m_gammaSpin->setValue(s.value("gamma", 1.0).toDouble());
    m_fpsSpin->setValue(s.value("fps", 24.0).toDouble());
    m_loader.setBudgetMB(s.value("cacheMB", 2048).toInt());
    m_bgCombo->setCurrentIndex(s.value("bgMode", 0).toInt());
    m_updatingUi = false;
    restoreGeometry(s.value("geometry").toByteArray());
    restoreState(s.value("windowState").toByteArray());
}

void MainWindow::saveSettings()
{
    QSettings s;
    s.setValue("input", m_inputCombo->currentData().toString());
    s.setValue("display", m_displayCombo->currentText());
    s.setValue("view", m_viewCombo->currentText());
    s.setValue("exposure", m_expSpin->value());
    s.setValue("gamma", m_gammaSpin->value());
    s.setValue("fps", m_fpsSpin->value());
    s.setValue("cacheMB", m_loader.budgetMB());
    s.setValue("bgMode", m_bgCombo->currentIndex());
    s.setValue("geometry", saveGeometry());
    s.setValue("windowState", saveState());
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    m_playTimer.stop();
    saveSettings();
    e->accept();
}
