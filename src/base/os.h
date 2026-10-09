#ifndef BASE_OS_H
#define BASE_OS_H

#if defined(_WIN32)
#define OS_WIN 1
#define OS_MAC 0
#define OS_LINUX 0
#elif defined(__APPLE__)
#define OS_WIN 0
#define OS_MAC 1
#define OS_LINUX 0
#elif defined(__linux__)
#define OS_WIN 0
#define OS_MAC 0
#define OS_LINUX 1
#else
#error "Unsupported operating system"
#endif

#endif
