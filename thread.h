#pragma once

#include <mutex>
#include <functional>

#include <vlc_access.h>
#include <vlc_threads.h>

namespace VLC {

class Mutex
{
    friend class CondVar;

    public:
        Mutex() {
            vlc_mutex_init(&lock_);
        }
        ~Mutex() {
            vlc_mutex_destroy(&lock_);
        }

        void lock() {
            vlc_mutex_lock(&lock_);
        }
        void unlock() noexcept {
            vlc_mutex_unlock(&lock_);
        }

    private:
        vlc_mutex_t lock_;
};

class CondVar
{
    public:
        CondVar() {
            vlc_cond_init(&cond_);
        }
        ~CondVar() {
            vlc_cond_destroy(&cond_);
        }

        template <class Predicate>
        void wait_for(std::unique_lock<Mutex>& m, mtime_t t, Predicate p) {
            while (!p())
                vlc_cond_timedwait(&cond_, &m.mutex()->lock_, t);
        }
        void signal() {
            vlc_cond_signal(&cond_);
        }

    private:
        vlc_cond_t        cond_;
};

class JoinableThread
{
    public:
        JoinableThread() = default;
        ~JoinableThread() {
            if (joinable_)
                vlc_join(thread_, nullptr);
        }

        template <class Functor>
        int Start(access_t* access, const Functor& func);

    private:
        vlc_thread_t  thread_;
        bool          joinable_ = false;
};

template <class Functor>
int JoinableThread::Start(access_t* access, const Functor& func)
{
    static auto trampoline = func;

    auto f = [](void*) -> void* {
        trampoline();
        return nullptr;
    };
    if (vlc_clone(&thread_, f, access, VLC_THREAD_PRIORITY_INPUT) != VLC_SUCCESS)
        return VLC_EGENERIC;

    joinable_ = true;
    return VLC_SUCCESS;
}

}
