#include "FrameLoader.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

FrameLoader::FrameLoader(QObject* parent) : QObject(parent)
{
    // OpenEXR đã tự giải nén đa luồng bên trong; 3 file song song là đủ để prefetch.
    m_pool.setMaxThreadCount(qBound(1, QThread::idealThreadCount() / 4, 3));
    m_cache.setMaxCost(2048); // MB
}

FrameLoader::~FrameLoader()
{
    m_gen.fetch_add(1, std::memory_order_relaxed);
    m_pool.clear();
    m_pool.waitForDone();
}

QString FrameLoader::key(const QString& path, const LayerRef& ref) const
{
    return path + QChar(0x1f) + QString::number(ref.part) + QChar(0x1f) + ref.partName + QChar(0x1f) + ref.layerName +
           (m_overscan ? QString(QChar(0x1f)) + QStringLiteral("overscan") : QString());
}

FramePtr FrameLoader::cached(const QString& k) const
{
    FramePtr* p = m_cache.object(k);
    return p ? *p : FramePtr();
}

void FrameLoader::request(const QString& path, const LayerRef& ref, int priority)
{
    QString k = key(path, ref);
    if (m_pending.contains(k) || m_cache.contains(k)) return;
    m_pending.insert(k);
    QPointer<FrameLoader> self(this);
    const bool overscan = m_overscan;
    const uint64_t gen = m_gen.load(std::memory_order_relaxed);
    m_pool.start(
        [self, gen, k, path, ref, overscan] {
            if (!self || self->m_gen.load(std::memory_order_relaxed) != gen) return;
            FramePtr f = FrameLoader::load(path, ref, overscan);
            if (!self) return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, gen, k, f] {
                    if (self) self->finished(gen, k, f);
                },
                Qt::QueuedConnection);
        },
        priority);
}

void FrameLoader::finished(uint64_t gen, const QString& k, FramePtr f)
{
    if (gen != m_gen.load(std::memory_order_relaxed)) return;
    m_pending.remove(k);
    if (f && f->error.isEmpty()) {
        int cost = (int)qMax<size_t>(1, f->bytes() >> 20);
        if (cost <= m_cache.maxCost()) m_cache.insert(k, new FramePtr(f), cost);
    }
    emit loaded(k, f);
}

void FrameLoader::clear()
{
    m_gen.fetch_add(1, std::memory_order_relaxed);
    m_pool.clear();
    m_pending.clear();
    m_cache.clear();
}

FramePtr FrameLoader::load(const QString& path, const LayerRef& ref, bool overscan)
{
    auto f = std::make_shared<Frame>();
    f->path = path;
    f->fileSize = QFileInfo(path).size();
    std::string err;
    auto file = exrv::ExrFile::openPath(path.toUtf8().toStdString(), &err);
    if (!file) {
        f->error = QString::fromStdString(err);
        return f;
    }
    f->info = file->info();
    int part = 0, layer = 0;
    file->defaultLayer(part, layer);

    // Tìm lại layer theo tên (ưu tiên đúng chỉ số part, sau đó theo tên part).
    const auto& parts = f->info.parts;
    if (ref.part >= 0) {
        int p = -1;
        if (ref.part < (int)parts.size() && QString::fromStdString(parts[ref.part].name) == ref.partName) p = ref.part;
        for (int i = 0; p < 0 && i < (int)parts.size(); ++i)
            if (QString::fromStdString(parts[i].name) == ref.partName) p = i;
        if (p < 0 && ref.part < (int)parts.size()) p = ref.part;
        if (p >= 0) {
            for (int l = 0; l < (int)parts[p].layers.size(); ++l)
                if (QString::fromStdString(parts[p].layers[l].name) == ref.layerName) {
                    part = p;
                    layer = l;
                    break;
                }
        }
    }
    f->part = part;
    f->layer = layer;

    auto img = std::make_shared<exrv::Image>();
    err.clear();
    exrv::ReadOptions opt;
    opt.overscan = overscan;
    if (!file->readLayer(part, layer, *img, &err, opt)) {
        f->error = QString::fromStdString(err);
        return f;
    }
    if (!err.empty()) f->warning = QString::fromStdString(err);
    f->image = img;
    f->csAttr = file->colorspaceAttribute(part);
    f->hasChroma = file->chromaticities(part, f->chroma);
    return f;
}
