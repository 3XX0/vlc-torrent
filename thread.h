/*****************************************************************************
 * Copyright (C) 2014 Videolan Team
 *
 * Authors: Jonathan Calmels <exxo@videolabs.io>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

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

        template <class Predicate, typename Rep, typename Period>
        bool WaitFor(std::unique_lock<Mutex>& m, std::chrono::duration<Rep, Period> timeout, Predicate pred);
        void Signal() {
            vlc_cond_signal(&cond_);
        }

    private:
        vlc_cond_t  cond_;
};

template <class Predicate, typename Rep, typename Period>
bool CondVar::WaitFor(std::unique_lock<Mutex>& m, std::chrono::duration<Rep, Period> timeout, Predicate pred)
{
    using namespace std::chrono;

    if (pred())
        return true;

    mtime_t t = mdate() + duration_cast<microseconds>(timeout).count();
    if (vlc_cond_timedwait(&cond_, &m.mutex()->lock_, t) == ETIMEDOUT)
        return false;
    return pred();
}

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
    static std::function<void()> trampoline;

    trampoline = func;
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
