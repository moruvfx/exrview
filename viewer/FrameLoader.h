// FrameLoader — đọc EXR ở thread nền + cache LRU theo dung lượng (cho xem sequence mượt).
#pragma once

#include "exrcore.h"

#include <QCache>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include <memory>

// Layer người dùng đang chọn — giữ theo tên để áp dụng cho mọi frame trong sequence.
struct LayerRef {
    int part = -1; // -1 = để file tự chọn layer mặc định
    QString partName;
    QString layerName;
};

struct Frame {
    QString path;
    exrv::FileInfo info;
    std::shared_ptr<exrv::Image> image;
    int part = 0, layer = 0;
    std::string csAttr;
    bool hasChroma = false;
    float chroma[8] = {};
    QString error, warning;
    qint64 fileSize = 0;
    size_t bytes() const { return image ? image->bytes() : 0; }
};
using FramePtr = std::shared_ptr<Frame>;

class FrameLoader : public QObject {
    Q_OBJECT
public:
    explicit FrameLoader(QObject* parent = nullptr);
    ~FrameLoader() override;

    static QString key(const QString& path, const LayerRef& ref);
    FramePtr cached(const QString& key) const;
    bool isPending(const QString& key) const { return m_pending.contains(key); }
    int pendingCount() const { return m_pending.size(); }
    void request(const QString& path, const LayerRef& ref);
    void clear();
    void setBudgetMB(int mb) { m_cache.setMaxCost(mb); }
    int budgetMB() const { return (int)m_cache.maxCost(); }
    int usedMB() const { return (int)m_cache.totalCost(); }

    static FramePtr load(const QString& path, const LayerRef& ref);

signals:
    void loaded(const QString& key, FramePtr frame);

private:
    void finished(const QString& key, FramePtr frame);

    QThreadPool m_pool;
    QCache<QString, FramePtr> m_cache;
    QSet<QString> m_pending;
};
