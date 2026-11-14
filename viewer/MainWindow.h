#pragma once

#include "FrameLoader.h"
#include "exrcore.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QStringList>
#include <QTimer>
#include <QVector>

class ImageView;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSlider;
class QSpinBox;
class QToolButton;
class QTreeWidget;
class QDockWidget;
class QMenu;
class QWidget;
class QSplitter;
class QAction;
struct Frame;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openFile(const QString& path);

protected:
    void closeEvent(QCloseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
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
    void detectSequence(const QString& path);
    QString keyForIndex(int i) const;
    void showIndex(int i);
    void setFrame(FramePtr f);
    void onLoaded(const QString& key, FramePtr f);
    void prefetchAround(int i, int ahead, int behind);
    void populateLayers();
    void updateInfoDock();
    void render();
    void updateHover(int x, int y, bool inside);
    void updateTitle();
    QString autoInputFor(const Frame& f, bool* dataLayer) const;
    void fillLayerCombo(QComboBox* combo, QString& signature, const Frame& f);

    // So sánh A | B: khung B có layer/pass và cài đặt màu riêng, zoom/pan đồng bộ với A
    void setCompare(bool on);
    void copyAtoB();
    void showIndexB(int i);
    void setFrameB(FramePtr f);
    void populateColorCombosB();
    void populateViewsB();
    void syncPipelineB();
    QString resolvedInputB() const;
    exrv::ViewSettings viewSettingsB();
    void renderB();
    void applyCompareLayout();           // bố trí theo chế độ: cạnh nhau / wipe / difference
    void setCompareViewMode(int mode);   // 0 cạnh nhau, 1 wipe, 2 difference (bật so sánh nếu đang tắt)

    // Sequence
    void togglePlay();
    void tick();
    void stepFrame(int d);

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
    QMenu* m_recentMenu = nullptr;

    QWidget* m_seqBar = nullptr;
    QSlider* m_seqSlider = nullptr;
    QLabel* m_frameLabel = nullptr;
    QToolButton* m_playBtn = nullptr;
    QDoubleSpinBox* m_fpsSpin = nullptr;
    QLabel* m_cacheLabel = nullptr;

    QStringList m_frames;
    QVector<long long> m_frameNumbers;
    int m_index = -1;
    LayerRef m_layerRef;
    FramePtr m_frame;
    QString m_wantedKey;
    QString m_defaultInput; // dùng khi không đoán được color space
    QString m_layerSignature;
    QString m_lastGuess;

    QTimer m_playTimer;
    QElapsedTimer m_fpsClock;
    int m_fpsFrames = 0;
    double m_actualFps = 0;
    bool m_updatingUi = false;
    int m_hoverX = -1, m_hoverY = -1;
    bool m_hoverInside = false;

    // So sánh A | B
    exrv::ColorPipeline m_pipelineB; // riêng cho B: shader GPU/processor CPU của mỗi khung độc lập
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
    LayerRef m_layerRefB;
    FramePtr m_frameB;
    QString m_wantedKeyB;
    QString m_lastGuessB;
    QString m_layerSignatureB;
    bool m_syncingView = false;
};
