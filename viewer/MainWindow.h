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
};
