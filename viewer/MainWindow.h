#pragma once

#include "FrameLoader.h"
#include "exrcore.h"

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QMainWindow>
#include <QPair>
#include <QStringList>
#include <QTimer>
#include <QVector>

class HistogramDock;
class ImageView;
class TimelineSlider;
class QAction;
class QComboBox;
class QDockWidget;
class QDoubleSpinBox;
class QLabel;
class QMenu;
class QSlider;
class QSpinBox;
class QSplitter;
class QToolButton;
class QTreeWidget;
class QWidget;
struct Frame;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openFile(const QString& path);
    void openFileB(const QString& path);

protected:
    void closeEvent(QCloseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    enum class LoopMode { Loop = 0, PingPong, Once };

    // UI
    void buildUi();
    void buildMenus();
    QWidget* buildSequenceBar();
    QWidget* buildComparePanel();

    // OCIO
    void loadOcioConfig(const QString& path);
    void populateColorCombos();
    void populateViews();
    exrv::ViewSettings currentViewSettings();
    QString resolvedInput() const;

    // Frame
    void detectSequence(const QString& path, QStringList& outFrames, QVector<long long>& outNumbers, int& outIndex);
    QString keyForIndex(int i) const;
    void showIndex(int i);
    void setFrame(FramePtr f);
    void onLoaded(const QString& key, FramePtr f);
    void prefetchAround(int i, int ahead, int behind);
    void updateTimelineCache();
    void populateLayers();
    void updateInfoDock();
    void updateHistogram();
    void render();
    void updateHover(int x, int y, bool inside);
    void onRegionSelected(const QRect& r);
    void updateTitle();
    QString autoInputFor(const Frame& f, bool* dataLayer) const;
    void fillLayerCombo(QComboBox* combo, QString& signature, const Frame& f);

    // Export / Clipboard
    QImage renderCurrentToQImage() const;
    void exportImage();
    void copyImageToClipboard();
    void copyPixelToClipboard();

    // So sánh A | B: khung B có thể xem layer khác của A hoặc mở file/sequence riêng
    void setCompare(bool on);
    void copyAtoB();
    void showIndexB(int i);
    void setFrameB(FramePtr f);
    void populateColorCombosB();
    void populateViewsB();
    void syncPipelineB();
    void ensurePipelineB();
    QString resolvedInputB() const;
    exrv::ViewSettings viewSettingsB();
    void renderB();
    void applyCompareLayout();
    void setCompareViewMode(int mode);

    // Sequence
    void togglePlay();
    void tick();
    void stepFrame(int d);
    int playTickMs() const;
    int effectiveIn() const;
    int effectiveOut() const;
    void cycleLoopMode();

    // Khác
    void applyToExplorerPreview();
    void registerShellExtension(bool unregister);
    void setDefaultInput();
    void addRecent(const QString& path);
    void rebuildRecentMenu();
    void saveSettings();
    void loadSettings();

    exrv::ColorPipeline m_pipeline;
    FrameLoader m_loader;

    ImageView* m_view = nullptr;
    QComboBox* m_layerCombo = nullptr;
    QComboBox* m_channelCombo = nullptr;
    QComboBox* m_inputCombo = nullptr;
    QComboBox* m_displayCombo = nullptr;
    QComboBox* m_viewCombo = nullptr;
    QComboBox* m_bgCombo = nullptr;
    QComboBox* m_inspectCombo = nullptr;
    QSlider* m_expSlider = nullptr;
    QDoubleSpinBox* m_expSpin = nullptr;
    QDoubleSpinBox* m_gammaSpin = nullptr;
    QLabel* m_pixelLabel = nullptr;
    QLabel* m_zoomLabel = nullptr;
    QLabel* m_configLabel = nullptr;
    QLabel* m_rendererLabel = nullptr;
    QLabel* m_infoLabel = nullptr;
    QTreeWidget* m_infoTree = nullptr;
    QDockWidget* m_infoDock = nullptr;
    HistogramDock* m_histDock = nullptr;
    QMenu* m_recentMenu = nullptr;

    QWidget* m_seqBar = nullptr;
    TimelineSlider* m_seqSlider = nullptr;
    QLabel* m_frameLabel = nullptr;
    QToolButton* m_playBtn = nullptr;
    QToolButton* m_loopBtn = nullptr;
    QDoubleSpinBox* m_fpsSpin = nullptr;
    QLabel* m_cacheLabel = nullptr;

    QStringList m_frames;
    QVector<long long> m_frameNumbers;
    int m_index = -1;
    int m_inFrame = 0;
    int m_outFrame = -1;
    LoopMode m_loopMode = LoopMode::Loop;
    int m_playDir = 1;
    LayerRef m_layerRef;
    FramePtr m_frame;
    QString m_wantedKey;
    QString m_defaultInput;
    QString m_layerSignature;
    QString m_lastGuess;

    QTimer m_playTimer;
    QElapsedTimer m_fpsClock;
    QElapsedTimer m_playClock;
    qint64 m_nextDueNs = 0;
    int m_fpsFrames = 0;
    double m_actualFps = 0;
    bool m_updatingUi = false;
    int m_hoverX = -1, m_hoverY = -1;
    bool m_hoverInside = false;

    // So sánh A | B
    exrv::ColorPipeline m_pipelineB{false};
    bool m_bPipelineReady = false;
    mutable QHash<QString, QPair<QString, bool>> m_guessCache;
    QSplitter* m_splitter = nullptr;
    QWidget* m_bPanel = nullptr;
    ImageView* m_viewB = nullptr;
    QWidget* m_bBar = nullptr;
    QAction* m_compareAct = nullptr;
    QComboBox* m_bModeCombo = nullptr;
    QComboBox* m_bLayerCombo = nullptr;
    QComboBox* m_bChannelCombo = nullptr;
    QComboBox* m_bInputCombo = nullptr;
    QComboBox* m_bDisplayCombo = nullptr;
    QComboBox* m_bViewCombo = nullptr;
    QDoubleSpinBox* m_bExpSpin = nullptr;
    bool m_compare = false;
    bool m_bInitialized = false;
    QStringList m_framesB; // rỗng = dùng chung sequence với A
    QVector<long long> m_frameNumbersB;
    LayerRef m_layerRefB;
    FramePtr m_frameB;
    QString m_wantedKeyB;
    QString m_lastGuessB;
    QString m_layerSignatureB;
    bool m_syncingView = false;
};
