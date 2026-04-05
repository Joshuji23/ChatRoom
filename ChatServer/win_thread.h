#ifndef WIN_THREAD_H
#define WIN_THREAD_H

#include <windows.h>
#include <functional>

class WinThread {
public:
    WinThread() : handle(NULL) {}
    
    template<typename Func, typename... Args>
    explicit WinThread(Func&& func, Args&&... args) {
        auto* params = new std::function<void()>(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        handle = CreateThread(NULL, 0, ThreadProc, params, 0, NULL);
    }
    
    ~WinThread() {
        if (handle) {
            CloseHandle(handle);
        }
    }
    
    void detach() {
        if (handle) {
            CloseHandle(handle);
            handle = NULL;
        }
    }
    
    void join() {
        if (handle) {
            WaitForSingleObject(handle, INFINITE);
            CloseHandle(handle);
            handle = NULL;
        }
    }
    
    bool joinable() const { return handle != NULL; }

private:
    static DWORD WINAPI ThreadProc(LPVOID param) {
        auto* func = static_cast<std::function<void()>*>(param);
        (*func)();
        delete func;
        return 0;
    }
    
    HANDLE handle;
    WinThread(const WinThread&) = delete;
    WinThread& operator=(const WinThread&) = delete;
};

#endif
