// The same libraries from C++: their APIs in zebkit::vendor::<library>, so
// that they cannot be confused with another copy an application has.

#include <cstring>
#include <iostream>
#include <string>

#include <zebkit/vendor/sqlite.h>
#include <zebkit/vendor/zlib.h>

namespace zlib = zebkit::vendor::zlib;
namespace sql = zebkit::vendor::sqlite;

int main() {
    int failures = 0;
    const std::string text(1000, 'z');
    unsigned char packed[1024];
    zlib::uLongf size = sizeof(packed);
    const bool compressed =
        zlib::compress(packed, &size, reinterpret_cast<const zlib::Bytef*>(text.data()), text.size()) == Z_OK;
    std::cout << (compressed && size < 100 ? "ok  " : "FAIL") << " zlib from C++: compress\n";
    failures += !(compressed && size < 100);

    sql::sqlite3* db = nullptr;
    const bool opened = sql::sqlite3_initialize() == SQLITE_OK && sql::sqlite3_open(":memory:", &db) == SQLITE_OK;
    std::cout << (opened ? "ok  " : "FAIL") << " sqlite from C++: sqlite3_open\n";
    failures += !opened;
    sql::sqlite3_close(db);
    return failures ? 1 : 0;
}
