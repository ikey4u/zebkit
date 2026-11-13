// The same libraries from C++: their APIs in zebkit::vendor::<library>, so
// that they cannot be confused with another copy an application has.

#include "harness.h"

#include <cstring>
#include <string>

#include <zebkit/vendor/sqlite.h>
#include <zebkit/vendor/freetype.h>
#include <zebkit/vendor/zlib.h>

namespace zlib = zebkit::vendor::zlib;
namespace sql = zebkit::vendor::sqlite;
namespace ft = zebkit::vendor::freetype;

int main() {
    const std::string text(1000, 'z');
    unsigned char packed[1024];
    zlib::uLongf size = sizeof(packed);
    const bool compressed =
        zlib::compress(packed, &size, reinterpret_cast<const zlib::Bytef*>(text.data()), text.size()) == Z_OK;
    check(compressed && size < 100, "zlib from C++: compress", NULL);

    sql::sqlite3* db = nullptr;
    const bool opened = sql::sqlite3_initialize() == SQLITE_OK && sql::sqlite3_open(":memory:", &db) == SQLITE_OK;
    check(opened, "sqlite from C++: sqlite3_open", NULL);
    sql::sqlite3_close(db);
    ft::FT_Library font_library = nullptr;
    const bool font_opened = ft::FT_Init_FreeType(&font_library) == 0;
    check(font_opened, "freetype from C++: isolated library", NULL);
    if (font_opened) ft::FT_Done_FreeType(font_library);
    return summary();
}
