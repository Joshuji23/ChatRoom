#ifndef WIN_MUTEX_H
#define WIN_MUTEX_H

#include <windows.h>

class WinMutex {
public:
    WinMutex() { InitializeCriticalSection(&cs); }
    ~WinMutex() { DeleteCriticalSection(&cs); }
    void lock() { EnterCriticalSection(&cs); }
    void unlock() { LeaveCriticalSection(&cs); }
private:
    CRITICAL_SECTION cs;
    WinMutex(const WinMutex&) = delete;
    WinMutex& operator=(const WinMutex&) = delete;
};

template<typename Mutex>
class WinLockGuard {
public:
    explicit WinLockGuard(Mutex& m) : mutex(m) { mutex.lock(); }
    ~WinLockGuard() { mutex.unlock(); }
private:
    Mutex& mutex;
    WinLockGuard(const WinLockGuard&) = delete;
    WinLockGuard& operator=(const WinLockGuard&) = delete;
};

#endif
