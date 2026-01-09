#ifndef TEST_UTILS_H_
#define TEST_UTILS_H_

#include <string>
#include <cstdlib>
#include <cerrno>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <filesystem>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

// Cross-platform temporary directory
inline std::string get_temp_dir() {
#ifdef _WIN32
    const char* tmp = std::getenv("TEMP");
    if (tmp) return std::string(tmp);
    tmp = std::getenv("TMP");
    if (tmp) return std::string(tmp);
    return "C:\\Windows\\Temp";
#else
    const char* tmp = std::getenv("TMPDIR");
    if (tmp) return std::string(tmp);
    return "/tmp";
#endif
}

// Cross-platform file deletion
inline int unlink_file(const std::string& path) {
#ifdef _WIN32
    return ::_unlink(path.c_str());
#else
    return ::unlink(path.c_str());
#endif
}

// Cross-platform directory removal
inline bool remove_directory(const std::string& path) {
#ifdef _WIN32
    try {
        std::filesystem::remove_all(path);
        return true;
    } catch (...) {
        return false;
    }
#else
    std::string cmd = "rm -rf \"" + path + "\"";
    return std::system(cmd.c_str()) == 0;
#endif
}

// Cross-platform directory creation
inline bool create_directory(const std::string& path) {
#ifdef _WIN32
    return ::_mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

// Cross-platform path join
inline std::string join_path(const std::string& a, const std::string& b) {
#ifdef _WIN32
    return a + "\\" + b;
#else
    return a + "/" + b;
#endif
}

#endif // TEST_UTILS_H_
