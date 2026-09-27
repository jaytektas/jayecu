// Minimal newlib syscall stubs for bare-metal operation.
//
// newlib-nano's stdio/retargeting layer references a handful of POSIX
// syscalls (_write, _read, _close, ...). With no OS underneath there is
// nothing to call, so the library emits "X is not implemented and will
// always fail" link warnings. We provide the stubs explicitly: each returns
// an error with errno=ENOSYS, which is the standard bare-metal behaviour and
// silences the warnings without changing semantics (these paths are never
// exercised — we have no filesystem-backed stdio).
//
// Note: _sbrk is deliberately NOT provided here. The heap is managed by
// FreeRTOS heap_4.c; newlib's malloc is unused, so _sbrk is never referenced.

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

int _close(int fd)                         { (void)fd; errno = ENOSYS; return -1; }
int _fstat(int fd, struct stat* st)        { (void)fd; if (st) st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd)                        { (void)fd; return 1; }
off_t _lseek(int fd, off_t off, int whence){ (void)fd; (void)off; (void)whence; errno = ENOSYS; return -1; }
int _open(const char* p, int f, int m)     { (void)p; (void)f; (void)m; errno = ENOSYS; return -1; }
int _read(int fd, char* buf, int len)      { (void)fd; (void)buf; (void)len; return 0; }
int _write(int fd, const char* buf, int len){ (void)fd; (void)buf; return len; }

int _getpid(void)                          { return 1; }
int _kill(int pid, int sig)                { (void)pid; (void)sig; errno = ENOSYS; return -1; }
int _gettimeofday(void* tv, void* tz)      { (void)tv; (void)tz; errno = ENOSYS; return -1; }
clock_t _times(void* buf)                  { (void)buf; return (clock_t)-1; }
