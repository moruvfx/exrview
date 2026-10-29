#include "MainWindow.h"
#include "ImageView.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
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
#include <QKeyEvent>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSlider>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>


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
    resize(1400, 860);
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
    connect(m_view, &ImageView::fileDropped, this, &MainWindow::openFile);
    connect(m_view, &ImageView::rendererChanged, this, [this](const QString& r) { m_rendererLabel->setText(r); });
    connect(m_view, &ImageView::zoomChanged, this,
            [this](double z) { m_zoomLabel->setText(QString("%1%").arg(qRound(z * 100))); });

    auto* central = new QWidget(this);
    auto* lay = new QVBoxLayout(central);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_view, 1);
    m_seqBar = buildSequenceBar();
    lay->addWidget(m_seqBar);
    m_seqBar->hide();
    setCentralWidget(central);

    // --- Toolbar: layer / channel
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

    // --- Toolbar: màu
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
    m_displayCombo->setMinimumWidth(160);
    cb->addWidget(m_displayCombo);
    cb->addWidget(new QLabel(tr("  View ")));
    m_viewCombo = new QComboBox;
    m_viewCombo->setMinimumWidth(200);
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
}

QWidget* MainWindow::buildSequenceBar()
{
    auto* bar = new QWidget;
    auto* l = new QHBoxLayout(bar);
    l->setContentsMargins(8, 4, 8, 4);
    auto mk = [&](const QString& text, const QString& tip, auto fn) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, fn);
        l->addWidget(b);
        return b;
    };
    mk("⏮", tr("Frame đầu (Home)"), [this] { showIndex(0); });
    mk("◀", tr("Frame trước (←)"), [this] { stepFrame(-1); });
    m_playBtn = mk("▶", tr("Play / Pause (Space)"), [this] { togglePlay(); });
    mk("▶|", tr("Frame sau (→)"), [this] { stepFrame(1); });
    mk("⏭", tr("Frame cuối (End)"), [this] { showIndex(m_frames.size() - 1); });

    m_seqSlider = new QSlider(Qt::Horizontal);
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
        if (m_playTimer.isActive()) m_playTimer.setInterval(qRound(1000.0 / f));
    });
    l->addWidget(m_fpsSpin);
    m_cacheLabel = new QLabel;
    m_cacheLabel->setMinimumWidth(170);
    l->addWidget(m_cacheLabel);
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
    m_recentMenu = file->addMenu(tr("Mở gần đây"));
    rebuildRecentMenu();
    file->addAction(tr("Mở lại"), QKeySequence(Qt::Key_F5), this, [this] {
        m_loader.clear();
        showIndex(m_index);
    });
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
    view->addAction(m_infoDock->toggleViewAction());
    m_infoDock->toggleViewAction()->setShortcut(QKeySequence(Qt::Key_I));

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
    cache->addAction(tr("Xoá cache"), this, [this] { m_loader.clear(); });

    auto* help = menuBar()->addMenu(tr("&Trợ giúp"));
    help->addAction(tr("Phím tắt"), this, [this] {
        QMessageBox::information(
            this, tr("Phím tắt"),
            tr("Ctrl+O  mở file\nF  vừa khung · 1 / 2  zoom 100% / 200% · lăn chuột  zoom · kéo  pan\n"
               "C R G B A L  kênh RGB / R / G / B / Alpha / Luminance\n"
               "[ ]  exposure ∓½ stop · 0  reset exposure/gamma\n"
               "← →  frame trước/sau · Space  play/pause · Home/End  frame đầu/cuối\n"
               "I  bảng thông tin file · F5  đọc lại file"));
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
    populateColorCombos();
    render();
}

void MainWindow::populateColorCombos()
{
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
    m_layerSignature.clear();
    m_frame.reset();
    detectSequence(fi.absoluteFilePath());
    showIndex(m_index);
}

void MainWindow::detectSequence(const QString& path)
{
    m_frames.clear();
    m_frameNumbers.clear();
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
            if (pad2 != pad && QString::number(f2).size() != pad2) continue; // khác padding
            found.push_back({f2, dir.absoluteFilePath(e)});
        }
        std::sort(found.begin(), found.end());
        if (found.size() > 1) {
            for (auto& p : found) {
                m_frameNumbers.push_back(p.first);
                m_frames.push_back(p.second);
            }
        }
    }
    if (m_frames.isEmpty()) {
        m_frames << fi.absoluteFilePath();
        m_frameNumbers << 0;
    }
    m_index = std::max(0, (int)m_frames.indexOf(fi.absoluteFilePath()));
    m_seqBar->setVisible(m_frames.size() > 1);
    m_updatingUi = true;
    m_seqSlider->setRange(0, m_frames.size() - 1);
    m_seqSlider->setValue(m_index);
    m_updatingUi = false;
}

QString MainWindow::keyForIndex(int i) const
{
    return (i >= 0 && i < m_frames.size()) ? FrameLoader::key(m_frames[i], m_layerRef) : QString();
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
        m_loader.request(m_frames[i], m_layerRef);
        if (!m_frame) m_view->setMessage(tr("Đang đọc %1…").arg(QFileInfo(m_frames[i]).fileName()));
    }
    if (!m_playTimer.isActive()) prefetchAround(i, 3, 1);
}

void MainWindow::prefetchAround(int i, int ahead, int behind)
{
    if (m_frames.size() < 2) return;
    for (int k = 1; k <= ahead; ++k) {
        int j = (i + k) % m_frames.size();
        m_loader.request(m_frames[j], m_layerRef);
    }
    for (int k = 1; k <= behind; ++k) {
        int j = i - k;
        if (j >= 0) m_loader.request(m_frames[j], m_layerRef);
    }
}

void MainWindow::onLoaded(const QString& key, FramePtr f)
{
    m_cacheLabel->setText(tr("Cache %1 / %2 MB").arg(m_loader.usedMB()).arg(m_loader.budgetMB()));
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

void MainWindow::setFrame(FramePtr f)
{
    bool newFile = !m_frame;
    bool sameSize = m_frame && m_frame->image && f->image && m_frame->image->width == f->image->width &&
                    m_frame->image->height == f->image->height;
    m_frame = f;
    m_lastGuess = QString::fromStdString(
        m_pipeline.guessInput(f->csAttr, f->hasChroma ? f->chroma : nullptr, QFileInfo(f->path).fileName().toStdString()));

    // Layer không phải màu (depth, N, P, motion...) -> dùng role "data" (Raw) để thấy đúng giá trị.
    bool dataLayer = true;
    const auto& L = f->info.parts[f->part].layers[f->layer];
    for (const auto& c : L.channels) {
        QString b = QString::fromStdString(c).section('.', -1).toLower();
        if (b == "r" || b == "g" || b == "b" || b == "red" || b == "green" || b == "blue" ||
            (b == "y" && L.name == "RGBA"))
            dataLayer = false;
    }
    QString dataCs = QString::fromStdString(m_pipeline.canonicalColorSpace("data"));
    if (dataLayer && !dataCs.isEmpty()) m_lastGuess = dataCs;

    // Nhãn mục "Tự động" cho biết đang dùng color space nào.
    QString autoName = !m_lastGuess.isEmpty() ? m_lastGuess
                                              : (!m_defaultInput.isEmpty() ? m_defaultInput
                                                                           : QString::fromStdString(m_pipeline.sceneLinear()));
    m_inputCombo->setItemText(0, tr("Tự động → %1%2").arg(autoName, m_lastGuess.isEmpty() ? tr(" (mặc định)") : (dataLayer && m_lastGuess == dataCs ? tr(" (layer dữ liệu)") : "")));

    populateLayers();
    if (!m_playTimer.isActive()) updateInfoDock();
    m_view->setSource(f->image, !newFile && sameSize);
    render();
    const auto& P = f->info.parts[f->part];
    m_infoLabel->setText(QString("%1×%2 · %3 · %4")
                             .arg(f->image->width)
                             .arg(f->image->height)
                             .arg(QString::fromStdString(P.compression))
                             .arg(humanSize(f->fileSize)));
    if (!f->warning.isEmpty()) m_view->setMessage(f->warning);
    updateTitle();
}

void MainWindow::populateLayers()
{
    if (!m_frame) return;
    const auto& parts = m_frame->info.parts;
    QString sig;
    for (auto& p : parts) {
        sig += QString::fromStdString(p.name) + "|";
        for (auto& l : p.layers) sig += QString::fromStdString(l.name) + ",";
    }
    if (sig != m_layerSignature) {
        m_layerSignature = sig;
        m_layerCombo->clear();
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
                m_layerCombo->addItem(text, QPoint(p, l));
            }
        }
    }
    int idx = m_layerCombo->findData(QPoint(m_frame->part, m_frame->layer));
    if (idx >= 0) m_layerCombo->setCurrentIndex(idx);
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

void MainWindow::render()
{
    if (m_updatingUi) return;
    m_view->setViewSettings(currentViewSettings());
    if (m_hoverInside) updateHover(m_hoverX, m_hoverY, true);
    if (m_infoDock->isVisible() && !m_playTimer.isActive()) {
        // cập nhật dòng "Input color space" trong bảng thông tin
        auto items = m_infoTree->findItems(tr("Input color space"), Qt::MatchExactly | Qt::MatchRecursive);
        for (auto* it : items) it->setText(1, resolvedInput());
    }
}

void MainWindow::updateHover(int x, int y, bool inside)
{
    m_hoverX = x;
    m_hoverY = y;
    m_hoverInside = inside;
    if (!inside || !m_frame || !m_frame->image) {
        m_pixelLabel->clear();
        return;
    }
    const auto& im = *m_frame->image;
    if (x >= im.width || y >= im.height) return;
    const float* p = &im.rgba[((size_t)y * im.width + x) * 4];
    // Giá trị hiển thị: chạy đúng 1 pixel qua OCIO (CPU) với cùng cài đặt.
    exrv::Image one;
    one.width = one.height = 1;
    one.rgba.assign(p, p + 4);
    std::vector<uint8_t> out;
    QRgb d = 0;
    if (m_pipeline.render(one, currentViewSettings(), out, nullptr)) d = qRgb(out[2], out[1], out[0]);
    const auto& P = m_frame->info.parts[m_frame->part];
    m_pixelLabel->setText(QString("x %1  y %2   R %3  G %4  B %5  A %6   →  %7 %8 %9")
                              .arg(x + P.dispX, 5)
                              .arg(y + P.dispY, 5)
                              .arg(fmtFloat(p[0]), 10)
                              .arg(fmtFloat(p[1]), 10)
                              .arg(fmtFloat(p[2]), 10)
                              .arg(fmtFloat(p[3]), 8)
                              .arg(qRed(d), 3)
                              .arg(qGreen(d), 3)
                              .arg(qBlue(d), 3));
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
void MainWindow::togglePlay()
{
    if (m_frames.size() < 2) return;
    if (m_playTimer.isActive()) {
        m_playTimer.stop();
        m_playBtn->setText("▶");
        updateInfoDock();
        m_frameLabel->setToolTip(QString());
        return;
    }
    m_playBtn->setText("⏸");
    m_fpsClock.restart();
    m_fpsFrames = 0;
    m_playTimer.start(qRound(1000.0 / m_fpsSpin->value()));
    prefetchAround(m_index, 8, 0);
}

void MainWindow::tick()
{
    if (m_frames.size() < 2) return;
    int next = (m_index + 1) % m_frames.size();
    prefetchAround(m_index, 8, 0);
    if (!m_loader.cached(keyForIndex(next))) return; // chờ frame đọc xong — không nhảy cóc
    showIndex(next);
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
    case Qt::Key_Home: showIndex(0); return;
    case Qt::Key_End: showIndex(m_frames.size() - 1); return;
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
    // Lưu dạng chuỗi để shell extension đọc trực tiếp bằng RegGetValueW.
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
    populateColorCombos();
    m_updatingUi = true;
    m_expSpin->setValue(s.value("exposure", 0.0).toDouble());
    m_expSlider->setValue(qRound(m_expSpin->value() * 10));
    m_gammaSpin->setValue(s.value("gamma", 1.0).toDouble());
    m_fpsSpin->setValue(s.value("fps", 24.0).toDouble());
    m_loader.setBudgetMB(s.value("cacheMB", 2048).toInt());
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
    s.setValue("geometry", saveGeometry());
    s.setValue("windowState", saveState());
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    m_playTimer.stop();
    saveSettings();
    e->accept();
}
