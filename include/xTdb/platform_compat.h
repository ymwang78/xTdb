#ifndef XTDB_PLATFORM_COMPAT_H_
#define XTDB_PLATFORM_COMPAT_H_

#ifdef _WIN32
    // Windows includes
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <io.h>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <direct.h>
    
    // Prevent Windows.h macros from interfering with C++ std library
    #ifdef max
    #undef max
    #endif
    #ifdef min
    #undef min
    #endif
    
    // Windows-specific defines
    #define unlink _unlink
    #define stat _stat
    #define fstat _fstat
    #define open _open
    #define close _close
    #define read _read
    #define write _write
    #define lseek _lseek
    
    // Windows doesn't have pread/pwrite, need to use alternative
    #define pread(fd, buf, count, offset) \
        (lseek(fd, offset, SEEK_SET) == -1 ? -1 : read(fd, buf, count))
    
    #define pwrite(fd, buf, count, offset) \
        (lseek(fd, offset, SEEK_SET) == -1 ? -1 : write(fd, buf, count))
    
    // posix_memalign alternative for Windows
    #include <malloc.h>
    #define posix_memalign(memptr, alignment, size) \
        ((*(memptr) = _aligned_malloc(size, alignment)) ? 0 : errno)
    
    // fsync equivalent
    #include <io.h>
    #define fsync _commit
    
    // ftruncate equivalent
    #define ftruncate _chsize
    
    // Path separator
    #define PATH_SEPARATOR "\\"
    
    // ssize_t is not defined on Windows
    #ifndef _SSIZE_T_DEFINED
    #include <basetsd.h>
    typedef SSIZE_T ssize_t;
    #define _SSIZE_T_DEFINED
    #endif
    
#else
    // Unix/Linux includes
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <sys/uio.h>
    
    // Path separator
    #define PATH_SEPARATOR "/"
    
#endif

#endif // XTDB_PLATFORM_COMPAT_H_
