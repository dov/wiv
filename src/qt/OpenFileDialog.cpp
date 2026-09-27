#include "OpenFileDialog.h"

#include "GivThumbnailRenderer.h"
#include "ImagePluginHost.h"

#include "nanosvg.h"
#include "nanosvgrast.h"

#include "QtAwesome.h"

#include <QAbstractFileIconProvider>
#include <QAbstractItemView>
#include <QAbstractProxyModel>
#include <QAction>
#include <QBoxLayout>
#include <QCoreApplication>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrentRun>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace givqt
{

namespace
{

namespace fs = std::filesystem;

constexpr int kThumbnailDecodeSize = 256; // decode size; delegates scale down to display size
constexpr int kMinThumbnailSize = 32;
constexpr int kZoomStep = 16;

// No-op icon provider. QFileDialog's default QFileInfoGatherer resolves a
// theme/MIME icon for every entry while gathering a directory - the
// dominant cost for large directories on Linux, stalling the dialog's
// appearance. ThumbnailDelegate draws its own icons/thumbnails and ignores
// the model's decoration role, so this removes that cost entirely.
// Stateless, so a single shared static instance is safe.
class FastFileIconProvider : public QAbstractFileIconProvider
{
public:
    QIcon icon(IconType) const override { return QIcon(); }
    QIcon icon(const QFileInfo&) const override { return QIcon(); }

    QString type(const QFileInfo& info) const override
    {
        if (info.isDir()) return QObject::tr("Folder");
        const QString suffix = info.suffix();
        return suffix.isEmpty() ? QObject::tr("File") : suffix.toUpper() + QStringLiteral(" File");
    }
};

// True if `path` is a wiv-openable file: a .giv scene, an .svg (loaded as
// vector shapes, not through the plugin host), or anything
// giv::ImagePluginHost::isSupported() claims. All three checks are
// suffix-only (isSupported() never decodes), safe to call from a
// background directory-scan thread once the plugin list has been warmed on
// the GUI thread (see OpenFileDialog's constructor).
bool isSupportedWivFile(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix();
    if (suffix.compare("giv", Qt::CaseInsensitive) == 0) return true;
    if (suffix.compare("svg", Qt::CaseInsensitive) == 0) return true;
    return giv::ImagePluginHost::isSupported(path.toStdString());
}

// Rasterizes an .svg file straight to a thumbnail-sized QImage via nanosvg/
// nanosvgrast (the same vendored single-header libraries SvgLoader.cpp
// parses/rasterizes gradient fills with - implementation lives in that
// translation unit, this one only needs the declarations).
QImage rasterizeSvgThumbnail(const QString& path)
{
    NSVGimage* image = nsvgParseFromFile(path.toLocal8Bit().constData(), "px", 96.0f);
    if (!image || image->width <= 0 || image->height <= 0)
    {
        if (image) nsvgDelete(image);
        return QImage();
    }

    const float scale = static_cast<float>(kThumbnailDecodeSize) / std::max(image->width, image->height);
    const int w = std::max(1, static_cast<int>(image->width * scale));
    const int h = std::max(1, static_cast<int>(image->height * scale));

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    QImage result;
    if (rast)
    {
        std::vector<unsigned char> buffer(static_cast<size_t>(w) * h * 4, 0);
        nsvgRasterize(rast, image, 0, 0, scale, buffer.data(), w, h, w * 4);
        result = QImage(buffer.data(), w, h, w * 4, QImage::Format_RGBA8888).copy();
        nsvgDeleteRasterizer(rast);
    }
    nsvgDelete(image);
    return result;
}

// Decodes any raster format wiv's plugin host can read into a thumbnail-
// sized QImage.
QImage decodeRasterThumbnail(const QString& path)
{
    auto loaded = giv::ImagePluginHost::load(path.toStdString());
    if (!loaded) return QImage();

    QImage full(loaded->rgba.data(), loaded->width, loaded->height, loaded->width * 4, QImage::Format_RGBA8888);
    return full.scaled(kThumbnailDecodeSize, kThumbnailDecodeSize, Qt::KeepAspectRatio, Qt::SmoothTransformation).copy();
}

// Renders a .giv scene straight to a thumbnail via GivThumbnailRenderer - a
// lightweight QPainter rasterizer independent of the VSG/Vulkan
// SceneBuilder pipeline (see GivThumbnailRenderer.h), so it can run here on
// a background thread with no GPU device.
QImage decodeGivThumbnail(const QString& path)
{
    return giv::renderGivFileThumbnail(path.toStdString(), kThumbnailDecodeSize);
}

// Runs on a QtConcurrent worker thread - must not touch any QWidget.
QImage decodeThumbnail(const QString& path)
{
    if (path.endsWith(".giv", Qt::CaseInsensitive)) return decodeGivThumbnail(path);
    if (path.endsWith(".svg", Qt::CaseInsensitive)) return rasterizeSvgThumbnail(path);
    return decodeRasterThumbnail(path);
}

} // namespace

// Paints an inline thumbnail (or a folder/file/queued/failed fallback icon)
// for column 0 of the file dialog's own list/tree view, in either a
// thumbnail-grid layout (GridIcon, used for List view) or a thumbnail-plus-
// text row layout (RowWithThumb, used for Detail view).
class ThumbnailDelegate : public QStyledItemDelegate
{
public:
    enum class RenderMode { GridIcon, RowWithThumb };

    ThumbnailDelegate(OpenFileDialog* dialog, int thumbnailSize, RenderMode mode)
        : QStyledItemDelegate(dialog), thumbnailSize_(thumbnailSize), renderMode_(mode), dialog_(dialog)
    {
    }

    void setThumbnailSize(int size) { thumbnailSize_ = size; }
    void setRenderMode(RenderMode mode) { renderMode_ = mode; }

    void setFolderIcon(const QIcon& icon) { folderIcon_ = icon; }
    void setFolderOutlineIcon(const QIcon& icon) { folderOutlineIcon_ = icon; }
    void setFileIcon(const QIcon& icon) { fileIcon_ = icon; }
    void setQueuedIcon(const QIcon& icon) { queuedIcon_ = icon; }
    void setFailedIcon(const QIcon& icon) { failedIcon_ = icon; }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        if (index.column() != 0)
        {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        painter->save();

        auto textColor = option.palette.text().color();
        if (option.state & QStyle::State_Selected)
        {
            painter->fillRect(option.rect, option.palette.highlight());
            textColor = option.palette.highlightedText().color();
        }
        else
        {
            painter->fillRect(option.rect, option.palette.window());
        }

        const QString filePath = index.data(QFileSystemModel::FilePathRole).toString();
        const QFileInfo fileInfo(filePath); // only fileName() used below

        // Read isDir from QFileSystemModel's already-gathered node cache
        // (O(1), no I/O) rather than QFileInfo::isDir(), which stat()s the
        // path - a syscall per visible item per repaint.
        bool isDir = false;
        {
            const QAbstractItemModel* m = index.model();
            QModelIndex srcIdx = index;
            while (auto* proxy = qobject_cast<const QAbstractProxyModel*>(m))
            {
                srcIdx = proxy->mapToSource(srcIdx);
                m = proxy->sourceModel();
            }
            if (auto* fsModel = qobject_cast<const QFileSystemModel*>(m))
                isDir = fsModel->isDir(srcIdx);
            else
                isDir = fileInfo.isDir();
        }

        QImage thumbnail;
        auto cacheIt = dialog_->thumbnailCache_.find(filePath);
        if (cacheIt != dialog_->thumbnailCache_.end()) thumbnail = cacheIt.value();

        const bool rowMode = (renderMode_ == RenderMode::RowWithThumb);
        QRect iconRect = option.rect;
        iconRect.setSize(QSize(thumbnailSize_, thumbnailSize_));
        if (!rowMode) iconRect.moveLeft(option.rect.left() + (option.rect.width() - thumbnailSize_) / 2);

        if (!thumbnail.isNull())
        {
            QImage scaled = thumbnail.scaled(thumbnailSize_, thumbnailSize_, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QRect imageRect = iconRect;
            imageRect.moveLeft(iconRect.left() + (thumbnailSize_ - scaled.width()) / 2);
            imageRect.moveTop(iconRect.top() + (thumbnailSize_ - scaled.height()) / 2);
            painter->drawImage(imageRect.topLeft(), scaled);
        }
        else if (isDir)
        {
            if (!folderIcon_.isNull()) painter->drawPixmap(iconRect.topLeft(), folderIcon_.pixmap(thumbnailSize_, thumbnailSize_));
            if (!folderOutlineIcon_.isNull())
                painter->drawPixmap(iconRect.topLeft(), folderOutlineIcon_.pixmap(thumbnailSize_, thumbnailSize_));
        }
        else
        {
            QIcon icon = fileIcon_;
            auto stateIt = dialog_->thumbnailState_.find(filePath);
            if (stateIt != dialog_->thumbnailState_.end())
            {
                if (stateIt.value() == ThumbnailState::Queued) icon = queuedIcon_;
                else if (stateIt.value() == ThumbnailState::Failed) icon = failedIcon_;
            }
            if (!icon.isNull()) painter->drawPixmap(iconRect.topLeft(), icon.pixmap(thumbnailSize_, thumbnailSize_));
        }

        const QString fileName = fileInfo.fileName();
        QTextOption textOption;
        QRect textRect = option.rect;
        if (rowMode)
        {
            textRect.setLeft(thumbnailSize_ + 16);
            textOption.setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            textOption.setWrapMode(QTextOption::WrapAnywhere);
        }
        else
        {
            textRect.setTop(textRect.top() + thumbnailSize_);
            textOption.setAlignment(Qt::AlignHCenter | Qt::AlignTop);
            textOption.setWrapMode(QTextOption::WrapAnywhere);
        }
        painter->setPen(textColor);
        painter->drawText(textRect, fileName, textOption);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QFontMetrics fm(option.font);
        const int width = thumbnailSize_ + 16;
        const QString fileName = index.data(Qt::DisplayRole).toString();

        if (renderMode_ == RenderMode::RowWithThumb)
        {
            int textWidth = option.rect.width() - thumbnailSize_ - 16;
            if (textWidth < 40) textWidth = 120;
            QRect textBounds = fm.boundingRect(QRect(0, 0, textWidth, 0), Qt::AlignLeft | Qt::TextWrapAnywhere, fileName);
            return QSize(width, std::max(thumbnailSize_, std::max(36, textBounds.height() + 4)));
        }

        QRect textBounds = fm.boundingRect(QRect(0, 0, width, 0), Qt::AlignHCenter | Qt::TextWrapAnywhere, fileName);
        return QSize(width, thumbnailSize_ + std::max(36, textBounds.height() + 4));
    }

private:
    int thumbnailSize_;
    RenderMode renderMode_;
    OpenFileDialog* dialog_;
    QIcon folderIcon_;
    QIcon folderOutlineIcon_;
    QIcon fileIcon_;
    QIcon queuedIcon_;
    QIcon failedIcon_;
};

// Custom data role tagging a row in the left-hand places tree as a visual
// separator - a thin divider line, like the ones Explorer draws between
// Quick access / This PC / Network. Rows carrying this role are non-
// selectable and painted as a single line by PlacesItemDelegate.
static constexpr int kPlacesSeparatorRole = Qt::UserRole + 100;

class PlacesItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        if (index.data(kPlacesSeparatorRole).toBool())
        {
            painter->save();
            painter->setPen(option.palette.color(QPalette::Mid));
            const int y = option.rect.center().y();
            painter->drawLine(option.rect.left() + 6, y, option.rect.right() - 6, y);
            painter->restore();
            return;
        }
        QStyledItemDelegate::paint(painter, option, index);
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        if (index.data(kPlacesSeparatorRole).toBool()) return QSize(option.rect.width(), 9);
        return QStyledItemDelegate::sizeHint(option, index);
    }
};

OpenFileDialog::OpenFileDialog(QWidget* parent, QSettings* settings)
    : QFileDialog(parent), settings_(settings), fileSystemWatcher_(new QFileSystemWatcher(this))
{
    setOption(QFileDialog::DontUseNativeDialog, true);
    setFileMode(QFileDialog::ExistingFiles);
    setAcceptMode(QFileDialog::AcceptOpen);
    setWindowTitle("Open");
    setNameFilters({
        "Supported files (*.giv *.svg *.png *.jpg *.jpeg *.bmp *.tga *.gif *.psd *.tif *.tiff *.webp *.pgm *.ppm)",
        "giv files (*.giv)",
        "Image files (*.svg *.png *.jpg *.jpeg *.bmp *.tga *.gif *.psd *.tif *.tiff *.webp *.pgm *.ppm)",
        "All files (*)",
    });

    // Warm giv::ImagePluginHost's lazily-loaded plugin list on the GUI thread
    // before any background directory scan or QtConcurrent decode can call
    // isSupported()/load() concurrently - the plugin list itself is not
    // built under a lock, only safe to read from multiple threads once
    // scanned once. isSupported() on a harmless placeholder name is enough
    // to force that scan without decoding anything.
    giv::ImagePluginHost::isSupported("warmup.png");

    static FastFileIconProvider s_fastIconProvider;
    setIconProvider(&s_fastIconProvider);

    listView_ = findChild<QListView*>("listView");
    treeView_ = findChild<QTreeView*>();

    thumbnailRepaintTimer_ = new QTimer(this);
    thumbnailRepaintTimer_->setInterval(33); // ~1 frame
    thumbnailRepaintTimer_->setSingleShot(true);
    connect(thumbnailRepaintTimer_, &QTimer::timeout, this, [this]() {
        if (listView_) listView_->viewport()->update();
        if (treeView_) treeView_->viewport()->update();
    });

    thumbnailSize_ = settings_ ? settings_->value("openDialog/thumbnailSize", 96).toInt() : 96;

    connect(fileSystemWatcher_, &QFileSystemWatcher::directoryChanged, this, &OpenFileDialog::onDirectoryChanged);

    // QtAwesome-themed icons, matching the reference application's own thumbnail-file-dialog
    // palette: pastel-yellow folder fill + dark outline, muted colors for the
    // queued/failed thumbnail states.
    awesome_ = new fa::QtAwesome(this);
    awesome_->initFontAwesome();
    auto tinted = [this](int style, int glyph, QColor color) {
        QVariantMap opts;
        opts.insert("color", color);
        opts.insert("color-active", color);
        opts.insert("color-selected", color);
        return awesome_->icon(style, glyph, opts);
    };
    const QIcon folderIcon = tinted(fa::fa_solid, fa::fa_folder, QColor(240, 220, 130));
    const QIcon folderOutlineIcon = tinted(fa::fa_regular, fa::fa_folder, QColor(80, 80, 80));
    const QIcon fileIcon = awesome_->icon(fa::fa_regular, fa::fa_file);
    const QIcon queuedIcon = tinted(fa::fa_solid, fa::fa_hourglass_half, QColor(160, 160, 160));
    const QIcon failedIcon = tinted(fa::fa_solid, fa::fa_triangle_exclamation, QColor(220, 120, 40));

    if (listView_)
    {
        listView_->setResizeMode(QListView::Adjust);
        listView_->setWrapping(true);
        listView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        listView_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        listView_->verticalScrollBar()->setSingleStep(16);
        listThumbnailDelegate_ = new ThumbnailDelegate(this, thumbnailSize_, ThumbnailDelegate::RenderMode::GridIcon);
        listThumbnailDelegate_->setFolderIcon(folderIcon);
        listThumbnailDelegate_->setFolderOutlineIcon(folderOutlineIcon);
        listThumbnailDelegate_->setFileIcon(fileIcon);
        listThumbnailDelegate_->setQueuedIcon(queuedIcon);
        listThumbnailDelegate_->setFailedIcon(failedIcon);
        listView_->setItemDelegate(listThumbnailDelegate_);
        listView_->viewport()->installEventFilter(this);
        listView_->installEventFilter(this);
    }

    if (treeView_)
    {
        treeThumbnailDelegate_ = new ThumbnailDelegate(this, thumbnailSize_, ThumbnailDelegate::RenderMode::RowWithThumb);
        treeThumbnailDelegate_->setFolderIcon(folderIcon);
        treeThumbnailDelegate_->setFolderOutlineIcon(folderOutlineIcon);
        treeThumbnailDelegate_->setFileIcon(fileIcon);
        treeThumbnailDelegate_->setQueuedIcon(queuedIcon);
        treeThumbnailDelegate_->setFailedIcon(failedIcon);
        treeView_->setItemDelegate(treeThumbnailDelegate_);
        treeView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        treeView_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        treeView_->verticalScrollBar()->setSingleStep(16);
        treeView_->header()->setSectionResizeMode(QHeaderView::Interactive);
        treeView_->setSortingEnabled(true);
        treeView_->viewport()->installEventFilter(this);
        treeView_->installEventFilter(this);
    }
    installEventFilter(this);

    loadingOverlay_ = new QLabel(tr("Loading..."), this);
    loadingOverlay_->setAlignment(Qt::AlignCenter);
    loadingOverlay_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    loadingOverlay_->setStyleSheet("QLabel { color: white; background-color: rgba(40,40,40,190); font-size: 18pt; }");
    loadingOverlay_->hide();

    installViewModeToolbar();

    if (auto* fsModel = findChild<QFileSystemModel*>())
    {
        // A file *open* dialog does not need live directory refresh; skip
        // the model's own change-watching (fewer background syscalls).
        fsModel->setOption(QFileSystemModel::DontWatchForChanges, true);
        connect(fsModel, &QFileSystemModel::rootPathChanged, this, &OpenFileDialog::fillThumbnailCache);
        connect(fsModel, &QFileSystemModel::directoryLoaded, this, [this](const QString& p) {
            const QString loaded = QFileInfo(p).absoluteFilePath();
            const QString current = QFileInfo(directory().absolutePath()).absoluteFilePath();
            if (loaded != current) return;
            directoryLoaded_ = true;
            loadedDirectories_.insert(current);
            updateLoadingOverlay();
            if (listView_) listView_->viewport()->update();
            if (treeView_) treeView_->viewport()->update();
        });
    }

    connect(this, &QFileDialog::directoryEntered, this, &OpenFileDialog::onDirectoryEnteredForRecents);
    connect(this, &QFileDialog::accepted, this, &OpenFileDialog::onAcceptedForRecents);

    buildPlacesModel();
    installPlacesTree();
}

// Explorer-style pastel-yellow folder icon for the left-pane directory rows
// (Recent / Bookmark children) - the same fill color as the folder icons in
// the file list (see OpenFileDialog's constructor).
QIcon OpenFileDialog::placesFolderIcon() const
{
    QVariantMap opts;
    const QColor folderYellow(240, 220, 130);
    opts.insert("color", folderYellow);
    opts.insert("color-active", folderYellow);
    opts.insert("color-selected", folderYellow);
    return awesome_->icon(fa::fa_solid, fa::fa_folder, opts);
}

void OpenFileDialog::buildPlacesModel()
{
    placesModel_ = new QStandardItemModel(this);
    QFileIconProvider iconProvider;

    // A single muted-blue accent for the quick-access/section glyphs, so
    // they read as one coherent group against either palette.
    auto faIcon = [this](int glyph) {
        QVariantMap opts;
        const QColor accent(90, 130, 190);
        opts.insert("color", accent);
        opts.insert("color-active", accent);
        opts.insert("color-selected", accent);
        return awesome_->icon(fa::fa_solid, glyph, opts);
    };

    auto appendSeparator = [this]() {
        auto* sep = new QStandardItem();
        sep->setFlags(Qt::NoItemFlags);
        sep->setData(true, kPlacesSeparatorRole);
        placesModel_->appendRow(sep);
    };

    auto appendPlace = [this](const QString& label, const QString& path, const QIcon& icon) {
        if (path.isEmpty() || !QDir(path).exists()) return;
        auto* item = new QStandardItem(icon, label);
        item->setEditable(false);
        item->setData(path, Qt::UserRole);
        item->setToolTip(path);
        placesModel_->appendRow(item);
    };

    appendPlace(tr("Home"), QDir::homePath(), faIcon(fa::fa_house));
    appendPlace(tr("Desktop"), QStandardPaths::writableLocation(QStandardPaths::DesktopLocation), faIcon(fa::fa_desktop));
    appendPlace(tr("Downloads"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), faIcon(fa::fa_download));
    appendPlace(tr("Documents"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), faIcon(fa::fa_file_lines));

    appendSeparator();

    recentNode_ = new QStandardItem(faIcon(fa::fa_clock), tr("Recent"));
    recentNode_->setEditable(false);
    placesModel_->appendRow(recentNode_);
    refreshRecentNode();

    appendSeparator();

    bookmarksNode_ = new QStandardItem(faIcon(fa::fa_star), tr("Bookmarks"));
    bookmarksNode_->setEditable(false);
    placesModel_->appendRow(bookmarksNode_);
    refreshBookmarksNode();

    appendSeparator();

    auto* myComputer = new QStandardItem(tr("My Computer"));
    myComputer->setIcon(iconProvider.icon(QAbstractFileIconProvider::Computer));
    myComputer->setEditable(false);
    placesModel_->appendRow(myComputer);
    myComputerNode_ = myComputer;

    // Defer drive enumeration off the construction path - QStorageInfo::
    // mountedVolumes() can take tens of ms with network drives, delaying
    // the dialog's appearance.
    QTimer::singleShot(0, this, &OpenFileDialog::populateDrives);
}

void OpenFileDialog::populateDrives()
{
    if (!myComputerNode_) return;

    QPointer<OpenFileDialog> self(this);
    std::thread([self]() {
        struct DriveEntry
        {
            QString rootPath;
            QString label;
        };
        std::vector<DriveEntry> drives;
        for (const QStorageInfo& info : QStorageInfo::mountedVolumes())
        {
            if (!info.isValid() || !info.isReady()) continue;
            QString label = info.displayName();
            if (label.isEmpty()) label = info.rootPath();
            drives.push_back({info.rootPath(), label});
        }

        QMetaObject::invokeMethod(qApp, [self, drives]() {
            if (!self || !self->myComputerNode_) return;
            QFileIconProvider iconProvider;
            for (const auto& d : drives)
            {
                auto* item = new QStandardItem(iconProvider.icon(QFileInfo(d.rootPath)), d.label);
                item->setEditable(false);
                item->setData(d.rootPath, Qt::UserRole);
                item->setToolTip(d.rootPath);
                self->myComputerNode_->appendRow(item);
            }
            if (self->placesTree_) self->placesTree_->expand(self->myComputerNode_->index());
        });
    }).detach();
}

void OpenFileDialog::installPlacesTree()
{
    QListView* originalSidebar = findChild<QListView*>("sidebar");
    if (!originalSidebar) return;

    placesTree_ = new QTreeView(this);
    placesTree_->setObjectName("placesTree");
    placesTree_->setHeaderHidden(true);
    placesTree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    placesTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    placesTree_->setUniformRowHeights(false); // separator rows are shorter than normal rows
    placesTree_->setItemDelegate(new PlacesItemDelegate(placesTree_));
    placesTree_->setModel(placesModel_);
    for (int row = 0; row < placesModel_->rowCount(); ++row) placesTree_->expand(placesModel_->index(row, 0));

    auto* leftContainer = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(leftContainer);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);
    leftLayout->addWidget(placesTree_, 1);

    addBookmarkButton_ = new QPushButton(tr("Add bookmark"), leftContainer);
    addBookmarkButton_->setToolTip(tr("Bookmark the current directory"));
    addBookmarkButton_->hide();
    connect(addBookmarkButton_, &QPushButton::clicked, this, &OpenFileDialog::onAddBookmarkClicked);
    leftLayout->addWidget(addBookmarkButton_);

    connect(placesTree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current, const QModelIndex&) {
                if (!addBookmarkButton_ || !bookmarksNode_) return;
                const QModelIndex bookmarksIdx = bookmarksNode_->index();
                const bool inBookmarks = current.isValid() && (current == bookmarksIdx || current.parent() == bookmarksIdx);
                addBookmarkButton_->setVisible(inBookmarks);
            });

    QWidget* parent = originalSidebar->parentWidget();
    if (auto* splitter = qobject_cast<QSplitter*>(parent))
    {
        int idx = splitter->indexOf(originalSidebar);
        splitter->replaceWidget(idx, leftContainer);
        leftSplitter_ = splitter;
        connect(leftSplitter_, &QSplitter::splitterMoved, this, [this](int, int) {
            if (settings_ && leftSplitter_) settings_->setValue("openDialog/placesSplitterState", leftSplitter_->saveState());
        });
    }
    else if (parent && parent->layout())
    {
        QLayoutItem* replaced = parent->layout()->replaceWidget(originalSidebar, leftContainer);
        delete replaced;
    }

    // Keep the original sidebar alive as a hidden child so QFileDialog
    // internals that may still reference it do not dangle.
    originalSidebar->setParent(this);
    originalSidebar->hide();

    connect(placesTree_, &QTreeView::clicked, this, &OpenFileDialog::onPlaceClicked);
    connect(placesTree_, &QTreeView::activated, this, &OpenFileDialog::onPlaceClicked);

    placesTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(placesTree_, &QWidget::customContextMenuRequested, this, &OpenFileDialog::onPlacesContextMenu);

    auto* deleteShortcut = new QShortcut(QKeySequence(Qt::Key_Delete), placesTree_);
    deleteShortcut->setContext(Qt::WidgetShortcut);
    connect(deleteShortcut, &QShortcut::activated, this, &OpenFileDialog::onRemoveSelectedRecent);
}

void OpenFileDialog::onPlaceClicked(const QModelIndex& index)
{
    if (!index.isValid()) return;
    const QString path = index.data(Qt::UserRole).toString();
    if (!path.isEmpty()) setDirectory(path);
}

void OpenFileDialog::refreshRecentNode()
{
    if (!recentNode_ || !settings_) return;
    recentNode_->removeRows(0, recentNode_->rowCount());

    const QIcon folderIcon = placesFolderIcon();
    const QStringList paths = settings_->value("openDialog/recentDirectories").toStringList();
    for (const QString& path : paths)
    {
        if (!QDir(path).exists()) continue;
        QString label = QFileInfo(path).fileName();
        if (label.isEmpty()) label = path;
        auto* item = new QStandardItem(folderIcon, label);
        item->setEditable(false);
        item->setData(path, Qt::UserRole);
        item->setToolTip(path);
        recentNode_->appendRow(item);
    }
}

void OpenFileDialog::addRecentDirectory(const QString& path)
{
    if (path.isEmpty() || !settings_) return;
    const QString normalized = QDir(path).absolutePath();
    if (normalized.isEmpty()) return;

    QStringList paths = settings_->value("openDialog/recentDirectories").toStringList();
    paths.removeAll(normalized);
    paths.prepend(normalized);
    while (paths.size() > kMaxRecents) paths.removeLast();
    settings_->setValue("openDialog/recentDirectories", paths);
}

void OpenFileDialog::onDirectoryEnteredForRecents(const QString& path)
{
    addRecentDirectory(path);
    refreshRecentNode();
    if (placesTree_ && recentNode_) placesTree_->expand(recentNode_->index());
}

void OpenFileDialog::onAcceptedForRecents()
{
    addRecentDirectory(directory().absolutePath());
    refreshRecentNode();
}

void OpenFileDialog::removeRecentDirectory(const QString& path)
{
    if (path.isEmpty() || !settings_) return;
    QStringList paths = settings_->value("openDialog/recentDirectories").toStringList();
    if (paths.removeAll(path) > 0) settings_->setValue("openDialog/recentDirectories", paths);
}

void OpenFileDialog::onPlacesContextMenu(const QPoint& pos)
{
    if (!placesTree_) return;
    const QModelIndex idx = placesTree_->indexAt(pos);
    if (!idx.isValid()) return;
    const QPoint globalPos = placesTree_->viewport()->mapToGlobal(pos);

    if (recentNode_ && idx.parent() == recentNode_->index())
    {
        QMenu menu(placesTree_);
        QAction* removeAction = menu.addAction(tr("Remove from Recent"));
        if (menu.exec(globalPos) == removeAction)
        {
            removeRecentDirectory(idx.data(Qt::UserRole).toString());
            refreshRecentNode();
        }
        return;
    }

    if (bookmarksNode_ && idx.parent() == bookmarksNode_->index())
    {
        QMenu menu(placesTree_);
        QAction* removeAction = menu.addAction(tr("Remove bookmark..."));
        if (menu.exec(globalPos) == removeAction)
        {
            const QString path = idx.data(Qt::UserRole).toString();
            if (confirmBookmarkRemoval(path))
            {
                removeBookmark(path);
                refreshBookmarksNode();
            }
        }
    }
}

void OpenFileDialog::onRemoveSelectedRecent()
{
    if (!placesTree_) return;
    const QModelIndex idx = placesTree_->currentIndex();
    if (!idx.isValid()) return;

    if (recentNode_ && idx.parent() == recentNode_->index())
    {
        removeRecentDirectory(idx.data(Qt::UserRole).toString());
        refreshRecentNode();
        return;
    }
    if (bookmarksNode_ && idx.parent() == bookmarksNode_->index())
    {
        const QString path = idx.data(Qt::UserRole).toString();
        if (confirmBookmarkRemoval(path))
        {
            removeBookmark(path);
            refreshBookmarksNode();
        }
    }
}

void OpenFileDialog::refreshBookmarksNode()
{
    if (!bookmarksNode_ || !settings_) return;
    bookmarksNode_->removeRows(0, bookmarksNode_->rowCount());

    const QIcon folderIcon = placesFolderIcon();
    const QStringList paths = settings_->value("openDialog/bookmarks").toStringList();
    for (const QString& path : paths)
    {
        QString label = QFileInfo(path).fileName();
        if (label.isEmpty()) label = path;
        auto* item = new QStandardItem(folderIcon, label);
        item->setEditable(false);
        item->setData(path, Qt::UserRole);
        item->setToolTip(path);
        bookmarksNode_->appendRow(item);
    }
}

void OpenFileDialog::addBookmark(const QString& path)
{
    if (path.isEmpty() || !settings_) return;
    const QString normalized = QDir(path).absolutePath();
    if (normalized.isEmpty()) return;
    QStringList paths = settings_->value("openDialog/bookmarks").toStringList();
    if (paths.contains(normalized)) return;
    paths.append(normalized);
    settings_->setValue("openDialog/bookmarks", paths);
}

void OpenFileDialog::removeBookmark(const QString& path)
{
    if (path.isEmpty() || !settings_) return;
    QStringList paths = settings_->value("openDialog/bookmarks").toStringList();
    if (paths.removeAll(path) > 0) settings_->setValue("openDialog/bookmarks", paths);
}

bool OpenFileDialog::confirmBookmarkRemoval(const QString& path)
{
    if (path.isEmpty()) return false;
    QString label = QFileInfo(path).fileName();
    if (label.isEmpty()) label = path;
    return QMessageBox::question(this, tr("Remove bookmark"), tr("Remove bookmark \"%1\"?\n%2").arg(label, path),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}

void OpenFileDialog::onAddBookmarkClicked()
{
    const QString currentDir = directory().absolutePath();
    if (currentDir.isEmpty()) return;
    addBookmark(currentDir);
    refreshBookmarksNode();
    if (placesTree_ && bookmarksNode_) placesTree_->expand(bookmarksNode_->index());
}

void OpenFileDialog::showEvent(QShowEvent* event)
{
    QFileDialog::showEvent(event);
    loadViewMode();

    if (settings_)
    {
        const QByteArray geom = settings_->value("openDialog/geometry").toByteArray();
        if (!geom.isEmpty()) restoreGeometry(geom);
        if (leftSplitter_)
        {
            const QByteArray splitterState = settings_->value("openDialog/placesSplitterState").toByteArray();
            if (!splitterState.isEmpty()) leftSplitter_->restoreState(splitterState);
        }
    }

    // rootPathChanged may not fire if setDirectory() was called with the
    // same path the dialog already has (e.g. on a second open), so make
    // sure the initial directory's thumbnails are requested regardless.
    fillThumbnailCache(directory().absolutePath());
    updateLoadingOverlay();

    // Seed Recent with the directory the dialog opens on, so it's populated
    // even if the user never navigates elsewhere and cancels without
    // opening a file - onDirectoryEnteredForRecents/onAcceptedForRecents
    // otherwise only fire on navigation or acceptance.
    addRecentDirectory(directory().absolutePath());
    refreshRecentNode();
}

void OpenFileDialog::closeEvent(QCloseEvent* event)
{
    ++generation_; // cancel/ignore any thumbnails still decoding
    saveViewMode();
    if (settings_)
    {
        settings_->setValue("openDialog/geometry", saveGeometry());
        if (leftSplitter_) settings_->setValue("openDialog/placesSplitterState", leftSplitter_->saveState());
    }
    QFileDialog::closeEvent(event);
}

void OpenFileDialog::saveViewMode()
{
    if (!settings_) return;
    settings_->setValue("openDialog/viewMode", viewMode_ == ViewMode::Detail ? "detail" : "list");
}

void OpenFileDialog::loadViewMode()
{
    const QString stored = settings_ ? settings_->value("openDialog/viewMode", "list").toString() : "list";
    applyViewMode(stored == "detail" ? ViewMode::Detail : ViewMode::List);
}

void OpenFileDialog::installViewModeToolbar()
{
    if (auto* btn = findChild<QToolButton*>("listModeButton")) btn->hide();
    if (auto* btn = findChild<QToolButton*>("detailModeButton")) btn->hide();

    // Find the QBoxLayout that directly contains newFolderButton. In Qt6 the
    // navigation buttons live in a QHBoxLayout placed in a cell of the
    // dialog's top QGridLayout - newFolderButton's QWidget parent is the
    // dialog itself, so walking the parent-widget chain doesn't reach the
    // hbox. Iterate all QBoxLayout descendants instead and find the one
    // whose indexOf() returns a hit.
    auto* newFolderBtn = findChild<QToolButton*>("newFolderButton");
    QBoxLayout* targetLayout = nullptr;
    int insertIndex = -1;
    if (newFolderBtn)
    {
        for (QBoxLayout* box : findChildren<QBoxLayout*>())
        {
            int idx = box->indexOf(newFolderBtn);
            if (idx >= 0)
            {
                targetLayout = box;
                insertIndex = idx + 1;
                break;
            }
        }
    }

    auto makeToolBtn = [this](const QIcon& icon, const QString& tooltip, bool checkable) {
        auto* b = new QToolButton(this);
        b->setIcon(icon);
        b->setToolTip(tooltip);
        b->setCheckable(checkable);
        b->setAutoRaise(true);
        return b;
    };

    listModeButton_ = makeToolBtn(awesome_->icon(fa::fa_solid, fa::fa_table_cells_large), tr("List view (thumbnail grid)"), true);
    detailModeButton_ = makeToolBtn(awesome_->icon(fa::fa_solid, fa::fa_list), tr("Detail view (thumbnail rows)"), true);
    connect(listModeButton_, &QToolButton::clicked, this, [this]() { onViewModeButtonClicked(ViewMode::List); });
    connect(detailModeButton_, &QToolButton::clicked, this, [this]() { onViewModeButtonClicked(ViewMode::Detail); });

    // Eyeglass/magnifying-glass zoom controls - purely a display zoom over
    // whatever is already in thumbnailCache_ (ThumbnailDelegate::paint()
    // rescales the cached QImage to thumbnailSize_), never a re-decode at a
    // different resolution.
    auto* zoomInButton = makeToolBtn(awesome_->icon(fa::fa_solid, fa::fa_magnifying_glass_plus), tr("Zoom in (Ctrl+wheel)"), false);
    auto* zoomOutButton = makeToolBtn(awesome_->icon(fa::fa_solid, fa::fa_magnifying_glass_minus), tr("Zoom out (Ctrl+wheel)"), false);
    connect(zoomInButton, &QToolButton::clicked, this, &OpenFileDialog::zoomIn);
    connect(zoomOutButton, &QToolButton::clicked, this, &OpenFileDialog::zoomOut);

    auto* separator = new QFrame(this);
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Sunken);

    if (targetLayout && insertIndex >= 0)
    {
        int idx = insertIndex;
        targetLayout->insertSpacing(idx++, 8);
        targetLayout->insertWidget(idx++, separator);
        targetLayout->insertSpacing(idx++, 8);
        targetLayout->insertWidget(idx++, listModeButton_);
        targetLayout->insertWidget(idx++, detailModeButton_);
        targetLayout->insertSpacing(idx++, 8);
        targetLayout->insertWidget(idx++, zoomOutButton);
        targetLayout->insertWidget(idx++, zoomInButton);
    }
    else
    {
        std::cerr << "wiv: could not locate Open dialog's navigation toolbar; view-mode buttons not installed\n";
        listModeButton_->hide();
        detailModeButton_->hide();
        zoomInButton->hide();
        zoomOutButton->hide();
        separator->hide();
    }

    updateViewModeButtonChecks();
}

void OpenFileDialog::applyViewMode(ViewMode mode)
{
    viewMode_ = mode;
    switch (mode)
    {
        case ViewMode::List:
            setViewMode(QFileDialog::List);
            if (listThumbnailDelegate_) listThumbnailDelegate_->setRenderMode(ThumbnailDelegate::RenderMode::GridIcon);
            break;
        case ViewMode::Detail:
            setViewMode(QFileDialog::Detail);
            if (treeThumbnailDelegate_) treeThumbnailDelegate_->setRenderMode(ThumbnailDelegate::RenderMode::RowWithThumb);
            break;
    }
    updateViewModeButtonChecks();
    updateThumbnails();
}

void OpenFileDialog::updateViewModeButtonChecks()
{
    if (listModeButton_) listModeButton_->setChecked(viewMode_ == ViewMode::List);
    if (detailModeButton_) detailModeButton_->setChecked(viewMode_ == ViewMode::Detail);
}

void OpenFileDialog::onViewModeButtonClicked(ViewMode mode)
{
    applyViewMode(mode);
}

bool OpenFileDialog::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::Wheel)
    {
        auto* wEvent = static_cast<QWheelEvent*>(event);
        if (wEvent->modifiers() & Qt::ControlModifier)
        {
            wheelEvent(wEvent);
            return true;
        }
    }
    return QFileDialog::eventFilter(obj, event);
}

void OpenFileDialog::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier)
    {
        if (event->angleDelta().y() > 0) zoomIn();
        else zoomOut();
        event->accept();
        return;
    }
    QFileDialog::wheelEvent(event);
}

void OpenFileDialog::resizeEvent(QResizeEvent* event)
{
    QFileDialog::resizeEvent(event);
    adjustGridSize();
    updateLoadingOverlay();
}

void OpenFileDialog::updateLoadingOverlay()
{
    if (!loadingOverlay_) return;

    QWidget* view = nullptr;
    if (treeView_ && treeView_->isVisible()) view = treeView_;
    else if (listView_ && listView_->isVisible()) view = listView_;

    if (directoryLoaded_ || !view)
    {
        loadingOverlay_->hide();
        return;
    }

    const QRect r(view->mapTo(this, QPoint(0, 0)), view->size());
    const bool wasVisible = loadingOverlay_->isVisible();
    loadingOverlay_->setGeometry(r);
    loadingOverlay_->raise();
    loadingOverlay_->show();
    if (!wasVisible) loadingOverlay_->repaint();
}

void OpenFileDialog::zoomIn()
{
    thumbnailSize_ += kZoomStep;
    if (settings_) settings_->setValue("openDialog/thumbnailSize", thumbnailSize_);
    updateThumbnails();
}

void OpenFileDialog::zoomOut()
{
    thumbnailSize_ = std::max(kMinThumbnailSize, thumbnailSize_ - kZoomStep);
    if (settings_) settings_->setValue("openDialog/thumbnailSize", thumbnailSize_);
    updateThumbnails();
}

void OpenFileDialog::fillThumbnailCache(const QString& directoryPath)
{
    if (directoryPath.isEmpty()) return;

    const QString normalized = QFileInfo(directoryPath).absoluteFilePath();
    // Deduplicate: rootPathChanged can fire more than once for the same
    // path. A second call would bump the generation and discard all
    // in-flight work from the first call.
    if (normalized == lastFillDirectory_) return;
    lastFillDirectory_ = normalized;

    directoryLoaded_ = loadedDirectories_.contains(normalized);
    updateLoadingOverlay();

    ++generation_; // invalidate/cancel pending decodes from the previous directory
    const quint64 generation = generation_;
    thumbnailCache_.clear();
    thumbnailState_.clear();

    fileSystemWatcher_->removePaths(fileSystemWatcher_->directories());

    // Scan the directory on a BACKGROUND thread (directory_iterator +
    // file_size() are syscalls, slow on network drives/huge dirs) and sort
    // smallest-first so quick thumbnails appear before large ones finish.
    QPointer<OpenFileDialog> self(this);
    const fs::path dirPath(directoryPath.toStdString());
    std::thread([self, directoryPath, dirPath, normalized, generation]() {
        struct FileEntry
        {
            QString filePath;
            uintmax_t size;
        };
        std::vector<FileEntry> files;

        std::error_code ec;
        for (fs::directory_iterator it(dirPath, ec), end; !ec && it != end; it.increment(ec))
        {
            const QString rawPath = QString::fromStdString(it->path().string());
            if (!isSupportedWivFile(rawPath)) continue;
            std::error_code entryEc;
            if (!it->is_regular_file(entryEc) || entryEc) continue;
            uintmax_t sz = it->file_size(entryEc);
            if (entryEc) sz = 0;
            files.push_back({QFileInfo(rawPath).absoluteFilePath(), sz});
        }
        std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) { return a.size < b.size; });

        QMetaObject::invokeMethod(qApp, [self, normalized, generation, files]() {
            if (!self) return;
            if (normalized != self->lastFillDirectory_) return; // superseded by a newer directory change

            self->directoryLoaded_ = true;
            self->loadedDirectories_.insert(normalized);
            self->updateLoadingOverlay();
            for (const auto& fe : files)
            {
                self->thumbnailState_[fe.filePath] = ThumbnailState::Queued;
                self->requestThumbnail(fe.filePath, generation);
            }
            if (self->listView_) self->listView_->viewport()->update();
            if (self->treeView_) self->treeView_->viewport()->update();
        });
    }).detach();
}

void OpenFileDialog::requestThumbnail(const QString& path, quint64 generation)
{
    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, path, generation]() {
        watcher->deleteLater();
        if (generation != generation_) return; // stale: navigated away before this decode finished
        const QImage image = watcher->result();
        onThumbnailReady(path, image, !image.isNull(), generation);
    });
    watcher->setFuture(QtConcurrent::run(decodeThumbnail, path));
}

void OpenFileDialog::onThumbnailReady(const QString& path, const QImage& image, bool success, quint64 generation)
{
    if (generation != generation_) return;

    if (success)
    {
        thumbnailState_[path] = ThumbnailState::Ready;
        setThumbnailCache(path, image);
    }
    else
    {
        thumbnailState_[path] = ThumbnailState::Failed;
    }

    // Coalesce repaints: a burst of arrivals triggers one viewport update
    // instead of one repaint per thumbnail, keeping the event loop free for
    // scrolling.
    if (!thumbnailRepaintTimer_->isActive()) thumbnailRepaintTimer_->start();
}

void OpenFileDialog::onDirectoryChanged(const QString& path)
{
    const quint64 generation = generation_;
    const fs::path dirPath(path.toStdString());
    std::error_code ec;
    for (fs::directory_iterator it(dirPath, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc) || entryEc) continue;
        const QString filePath = QFileInfo(QString::fromStdString(it->path().string())).absoluteFilePath();
        if (!isSupportedWivFile(filePath)) continue;
        if (!thumbnailState_.contains(filePath))
        {
            thumbnailState_[filePath] = ThumbnailState::Queued;
            requestThumbnail(filePath, generation);
        }
    }
    if (listView_) listView_->viewport()->update();
    if (treeView_) treeView_->viewport()->update();
}

void OpenFileDialog::adjustGridSize()
{
    if (listView_)
    {
        listView_->setFlow(QListView::LeftToRight);
        listView_->setWrapping(true);
    }
}

void OpenFileDialog::setThumbnailCache(const QString& filePath, const QImage& thumbnail)
{
    thumbnailCache_[filePath] = thumbnail;
}

void OpenFileDialog::updateThumbnails()
{
    if (listThumbnailDelegate_) listThumbnailDelegate_->setThumbnailSize(thumbnailSize_);
    if (treeThumbnailDelegate_) treeThumbnailDelegate_->setThumbnailSize(thumbnailSize_);
    adjustGridSize();

    if (listView_)
    {
        listView_->updateGeometry();
        listView_->doItemsLayout();
        listView_->viewport()->update();
    }
    if (treeView_)
    {
        treeView_->header()->resizeSection(0, thumbnailSize_ + 16);
        treeView_->updateGeometry();
        treeView_->doItemsLayout();
        treeView_->viewport()->update();
    }
}

} // namespace givqt
