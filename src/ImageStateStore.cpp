#include "ImageStateStore.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>

#include "PathUtils.h"

namespace giv
{

namespace
{
constexpr int kMaxRows = 2000;

struct Stmt
{
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            spdlog::warn("image state: {}", sqlite3_errmsg(db));
    }
    ~Stmt() { sqlite3_finalize(s); }
    explicit operator bool() const { return s != nullptr; }
};

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

ImageStateStore::ImageStateStore(const std::string& dbPath)
{
    std::error_code ec;
    std::filesystem::create_directories(giv::utf8ToPath(dbPath).parent_path(), ec);
    if (sqlite3_open(dbPath.c_str(), &db_) != SQLITE_OK)
    {
        spdlog::warn("image state: cannot open {}: {}", dbPath, sqlite3_errmsg(db_));
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    // Several wiv instances may share the file.
    sqlite3_busy_timeout(db_, 2000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    if (!exec("CREATE TABLE IF NOT EXISTS image_state("
              "path TEXT PRIMARY KEY, size INTEGER NOT NULL, mtime INTEGER NOT NULL, last_used INTEGER NOT NULL,"
              "has_contrast INTEGER NOT NULL DEFAULT 0, cmin REAL, cmax REAL, colormap INTEGER, colormap_enabled INTEGER,"
              "has_view INTEGER NOT NULL DEFAULT 0, cx REAL, cy REAL, sx REAL, sy REAL)"))
    {
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    exec("CREATE TABLE IF NOT EXISTS scene_state("
         "path TEXT PRIMARY KEY, size INTEGER NOT NULL, mtime INTEGER NOT NULL, last_used INTEGER NOT NULL,"
         "image_index INTEGER NOT NULL)");
    exec("CREATE TABLE IF NOT EXISTS slice_state("
         "path TEXT PRIMARY KEY, size INTEGER NOT NULL, mtime INTEGER NOT NULL, last_used INTEGER NOT NULL,"
         "slice INTEGER NOT NULL)");
    prune(kMaxRows);
}

ImageStateStore::~ImageStateStore()
{
    if (db_) sqlite3_close(db_);
}

bool ImageStateStore::exec(const char* sql)
{
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK)
    {
        spdlog::warn("image state: {}", err ? err : "sql error");
        sqlite3_free(err);
        return false;
    }
    return true;
}

// Drops the least recently used rows beyond `keep`.
void ImageStateStore::prune(int keep)
{
    Stmt st(db_, "DELETE FROM image_state WHERE path NOT IN (SELECT path FROM image_state ORDER BY last_used DESC LIMIT ?1)");
    if (!st) return;
    sqlite3_bind_int(st.s, 1, keep);
    sqlite3_step(st.s);
    Stmt st3(db_, "DELETE FROM slice_state WHERE path NOT IN (SELECT path FROM slice_state ORDER BY last_used DESC LIMIT ?1)");
    if (st3)
    {
        sqlite3_bind_int(st3.s, 1, keep);
        sqlite3_step(st3.s);
    }
    Stmt st2(db_, "DELETE FROM scene_state WHERE path NOT IN (SELECT path FROM scene_state ORDER BY last_used DESC LIMIT ?1)");
    if (!st2) return;
    sqlite3_bind_int(st2.s, 1, keep);
    sqlite3_step(st2.s);
}

std::string ImageStateStore::canonicalPath(const std::string& imagePath)
{
    if (imagePath.empty()) return imagePath;
    std::error_code ec;
    auto p = std::filesystem::weakly_canonical(giv::utf8ToPath(imagePath), ec);
    return ec ? imagePath : p.generic_string();
}

bool ImageStateStore::fileId(const std::string& imagePath, FileId& out)
{
    std::error_code ec;
    const auto p = giv::utf8ToPath(imagePath);
    const auto size = std::filesystem::file_size(p, ec);
    if (ec) return false;
    const auto mtime = std::filesystem::last_write_time(p, ec);
    if (ec) return false;
    out.size = static_cast<int64_t>(size);
    out.mtime = std::chrono::duration_cast<std::chrono::milliseconds>(mtime.time_since_epoch()).count();
    return true;
}

std::optional<ImageStateStore::Contrast> ImageStateStore::loadContrast(const std::string& rawPath)
{
    const std::string imagePath = canonicalPath(rawPath);
    FileId id;
    if (!db_ || !fileId(imagePath, id)) return std::nullopt;
    Stmt st(db_, "SELECT cmin, cmax, colormap, colormap_enabled FROM image_state "
                 "WHERE path=?1 AND size=?2 AND mtime=?3 AND has_contrast=1");
    if (!st) return std::nullopt;
    sqlite3_bind_text(st.s, 1, imagePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, id.size);
    sqlite3_bind_int64(st.s, 3, id.mtime);
    if (sqlite3_step(st.s) != SQLITE_ROW) return std::nullopt;
    Contrast c;
    c.min = static_cast<float>(sqlite3_column_double(st.s, 0));
    c.max = static_cast<float>(sqlite3_column_double(st.s, 1));
    c.colormapId = sqlite3_column_int(st.s, 2);
    c.colormapEnabled = sqlite3_column_int(st.s, 3) != 0;
    return c;
}

std::optional<ImageStateStore::View> ImageStateStore::loadView(const std::string& rawPath)
{
    const std::string imagePath = canonicalPath(rawPath);
    FileId id;
    if (!db_ || !fileId(imagePath, id)) return std::nullopt;
    Stmt st(db_, "SELECT cx, cy, sx, sy FROM image_state WHERE path=?1 AND size=?2 AND mtime=?3 AND has_view=1");
    if (!st) return std::nullopt;
    sqlite3_bind_text(st.s, 1, imagePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, id.size);
    sqlite3_bind_int64(st.s, 3, id.mtime);
    if (sqlite3_step(st.s) != SQLITE_ROW) return std::nullopt;
    View v;
    v.centerX = sqlite3_column_double(st.s, 0);
    v.centerY = sqlite3_column_double(st.s, 1);
    v.scaleX = sqlite3_column_double(st.s, 2);
    v.scaleY = sqlite3_column_double(st.s, 3);
    if (!(v.scaleX > 0.0) || !(v.scaleY > 0.0)) return std::nullopt;
    return v;
}

// In an upsert's DO UPDATE, every expression sees the row's old values, so
// "size=excluded.size" below and the CASEs that drop the *other* group when
// the file changed are consistent.
void ImageStateStore::saveContrast(const std::string& rawPath, const Contrast& c)
{
    const std::string imagePath = canonicalPath(rawPath);
    FileId id;
    if (!db_ || !fileId(imagePath, id)) return;
    Stmt st(db_, "INSERT INTO image_state(path,size,mtime,last_used,has_contrast,cmin,cmax,colormap,colormap_enabled)"
                 " VALUES(?1,?2,?3,?4,1,?5,?6,?7,?8)"
                 " ON CONFLICT(path) DO UPDATE SET"
                 " has_view=CASE WHEN size=excluded.size AND mtime=excluded.mtime THEN has_view ELSE 0 END,"
                 " size=excluded.size, mtime=excluded.mtime, last_used=excluded.last_used, has_contrast=1,"
                 " cmin=excluded.cmin, cmax=excluded.cmax, colormap=excluded.colormap,"
                 " colormap_enabled=excluded.colormap_enabled");
    if (!st) return;
    sqlite3_bind_text(st.s, 1, imagePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, id.size);
    sqlite3_bind_int64(st.s, 3, id.mtime);
    sqlite3_bind_int64(st.s, 4, nowMs());
    sqlite3_bind_double(st.s, 5, c.min);
    sqlite3_bind_double(st.s, 6, c.max);
    sqlite3_bind_int(st.s, 7, c.colormapId);
    sqlite3_bind_int(st.s, 8, c.colormapEnabled ? 1 : 0);
    sqlite3_step(st.s);
}

void ImageStateStore::saveView(const std::string& rawPath, const View& v)
{
    const std::string imagePath = canonicalPath(rawPath);
    FileId id;
    if (!db_ || !fileId(imagePath, id)) return;
    Stmt st(db_, "INSERT INTO image_state(path,size,mtime,last_used,has_view,cx,cy,sx,sy)"
                 " VALUES(?1,?2,?3,?4,1,?5,?6,?7,?8)"
                 " ON CONFLICT(path) DO UPDATE SET"
                 " has_contrast=CASE WHEN size=excluded.size AND mtime=excluded.mtime THEN has_contrast ELSE 0 END,"
                 " size=excluded.size, mtime=excluded.mtime, last_used=excluded.last_used, has_view=1,"
                 " cx=excluded.cx, cy=excluded.cy, sx=excluded.sx, sy=excluded.sy");
    if (!st) return;
    sqlite3_bind_text(st.s, 1, imagePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, id.size);
    sqlite3_bind_int64(st.s, 3, id.mtime);
    sqlite3_bind_int64(st.s, 4, nowMs());
    sqlite3_bind_double(st.s, 5, v.centerX);
    sqlite3_bind_double(st.s, 6, v.centerY);
    sqlite3_bind_double(st.s, 7, v.scaleX);
    sqlite3_bind_double(st.s, 8, v.scaleY);
    sqlite3_step(st.s);
}

void ImageStateStore::clearView(const std::string& rawPath)
{
    const std::string imagePath = canonicalPath(rawPath);
    if (!db_) return;
    Stmt st(db_, "UPDATE image_state SET has_view=0 WHERE path=?1");
    if (!st) return;
    sqlite3_bind_text(st.s, 1, imagePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st.s);
}

std::string ImageStateStore::sceneKey(const std::vector<std::string>& scenePaths)
{
    std::string key;
    for (const auto& p : scenePaths)
    {
        if (!key.empty()) key += '\n';
        key += canonicalPath(p);
    }
    return key;
}

std::optional<int> ImageStateStore::loadImageIndex(const std::vector<std::string>& scenePaths)
{
    if (scenePaths.empty()) return std::nullopt;
    const std::string scenePath = sceneKey(scenePaths);
    FileId id;
    if (!db_ || !fileId(canonicalPath(scenePaths.front()), id)) return std::nullopt;
    Stmt st(db_, "SELECT image_index FROM scene_state WHERE path=?1 AND size=?2 AND mtime=?3");
    if (!st) return std::nullopt;
    sqlite3_bind_text(st.s, 1, scenePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, id.size);
    sqlite3_bind_int64(st.s, 3, id.mtime);
    if (sqlite3_step(st.s) != SQLITE_ROW) return std::nullopt;
    return sqlite3_column_int(st.s, 0);
}

void ImageStateStore::saveImageIndex(const std::vector<std::string>& scenePaths, int index)
{
    if (scenePaths.empty()) return;
    const std::string scenePath = sceneKey(scenePaths);
    FileId id;
    if (!db_ || !fileId(canonicalPath(scenePaths.front()), id)) return;
    Stmt st(db_, "INSERT INTO scene_state(path,size,mtime,last_used,image_index) VALUES(?1,?2,?3,?4,?5)"
                 " ON CONFLICT(path) DO UPDATE SET size=excluded.size, mtime=excluded.mtime,"
                 " last_used=excluded.last_used, image_index=excluded.image_index");
    if (!st) return;
    sqlite3_bind_text(st.s, 1, scenePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, id.size);
    sqlite3_bind_int64(st.s, 3, id.mtime);
    sqlite3_bind_int64(st.s, 4, nowMs());
    sqlite3_bind_int(st.s, 5, index);
    sqlite3_step(st.s);
}

std::optional<int> ImageStateStore::loadSlice(const std::string& rawPath)
{
    const std::string imagePath = canonicalPath(rawPath);
    FileId id;
    if (!db_ || !fileId(imagePath, id)) return std::nullopt;
    Stmt st(db_, "SELECT slice FROM slice_state WHERE path=?1 AND size=?2 AND mtime=?3");
    if (!st) return std::nullopt;
    sqlite3_bind_text(st.s, 1, imagePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, id.size);
    sqlite3_bind_int64(st.s, 3, id.mtime);
    if (sqlite3_step(st.s) != SQLITE_ROW) return std::nullopt;
    return sqlite3_column_int(st.s, 0);
}

void ImageStateStore::saveSlice(const std::string& rawPath, int slice)
{
    const std::string imagePath = canonicalPath(rawPath);
    FileId id;
    if (!db_ || !fileId(imagePath, id)) return;
    Stmt st(db_, "INSERT INTO slice_state(path,size,mtime,last_used,slice) VALUES(?1,?2,?3,?4,?5)"
                 " ON CONFLICT(path) DO UPDATE SET size=excluded.size, mtime=excluded.mtime,"
                 " last_used=excluded.last_used, slice=excluded.slice");
    if (!st) return;
    sqlite3_bind_text(st.s, 1, imagePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, id.size);
    sqlite3_bind_int64(st.s, 3, id.mtime);
    sqlite3_bind_int64(st.s, 4, nowMs());
    sqlite3_bind_int(st.s, 5, slice);
    sqlite3_step(st.s);
}

} // namespace giv
