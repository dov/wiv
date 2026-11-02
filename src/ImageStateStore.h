#pragma once
//
// ImageStateStore.h - per-image view settings (contrast window, color table,
// pan/zoom) kept in a small SQLite file between runs. Rows are keyed by the
// image's path and carry its size and modification time: a record whose file
// has changed since is ignored, since a contrast window or view saved for the
// old pixels would be wrong for the new ones.
//
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace giv
{

class ImageStateStore
{
public:
    struct Contrast
    {
        float min = 0.0f;
        float max = 0.0f;
        int colormapId = 0;
        bool colormapEnabled = false;
    };
    struct View
    {
        double centerX = 0.0, centerY = 0.0;
        double scaleX = 1.0, scaleY = 1.0; // device pixels per world unit
    };

    // Opens (creating if need be) the database at `dbPath`. An unusable file
    // leaves the store disabled: every call then does nothing.
    explicit ImageStateStore(const std::string& dbPath);
    ~ImageStateStore();
    ImageStateStore(const ImageStateStore&) = delete;
    ImageStateStore& operator=(const ImageStateStore&) = delete;

    // The key a path is stored under: absolute and normalized, so the same
    // file reached as "../x.fits" or "/abs/x.fits" is one image.
    static std::string canonicalPath(const std::string& imagePath);

    bool isOpen() const { return db_ != nullptr; }

    std::optional<Contrast> loadContrast(const std::string& imagePath);
    std::optional<View> loadView(const std::string& imagePath);

    void saveContrast(const std::string& imagePath, const Contrast& c);
    void saveView(const std::string& imagePath, const View& v);
    // Which image of a multi-image scene was showing. `sceneKey` names the
    // scene (one path, or several joined, see sceneKey()); `scenePaths` are
    // the files it was loaded from, the first of which gives the size/mtime
    // the record is checked against.
    static std::string sceneKey(const std::vector<std::string>& scenePaths);
    std::optional<int> loadImageIndex(const std::vector<std::string>& scenePaths);
    void saveImageIndex(const std::vector<std::string>& scenePaths, int index);

    // The slice of a volume (e.g. a multi-slice FITS or DICOM) last shown.
    std::optional<int> loadSlice(const std::string& imagePath);
    void saveSlice(const std::string& imagePath, int slice);

    void clearView(const std::string& imagePath);

private:
    sqlite3* db_ = nullptr;

    struct FileId
    {
        int64_t size = 0;
        int64_t mtime = 0; // ms since the epoch
    };
    static bool fileId(const std::string& imagePath, FileId& out);
    bool exec(const char* sql);
    void prune(int keep);
};

} // namespace giv
