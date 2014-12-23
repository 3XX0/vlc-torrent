#pragma once

#include <string>
#include <cstdlib>
#include <memory>
#include <deque>
#include <atomic>
#include <functional>

#include <vlc_common.h>
#include <vlc_access.h>
#include <vlc_threads.h>

#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>

namespace lt = libtorrent;

using unique_cptr = std::unique_ptr<char, void (*)(void*)>;

class JoinableThread
{
    public:
        JoinableThread() = default;
        ~JoinableThread() {
            if (joinable_)
                vlc_join(thread_, nullptr);
        }

        int Start(access_t* access, const std::function<void()>& func);

    private:
        vlc_thread_t  thread_;
        bool          joinable_ = false;
};

struct Piece
{
    int       id;
    int       offset;
    int       length;
    block_t*  data;
};

struct PiecesQueue
{
    PiecesQueue() {
        vlc_mutex_init(&lock);
        vlc_cond_init(&cond);
    }
    ~PiecesQueue() {
        vlc_mutex_destroy(&lock);
        vlc_cond_destroy(&cond);
    }

    vlc_mutex_t       lock;
    vlc_cond_t        cond;
    std::deque<Piece> pieces;
};

class TorrentAccess
{
    public:
        TorrentAccess(access_t* p_access) :
            access_{p_access},
            file_at_{0},
            stopped_{false},
            fingerprint_{"VO", LIBTORRENT_VERSION_MAJOR, LIBTORRENT_VERSION_MINOR, 0, 0},
            session_{fingerprint_},
            download_dir_{nullptr, std::free}
        {}
        ~TorrentAccess() {
            stopped_ = true;
        }

        static std::unique_ptr<lt::torrent_info> ParseURI(const std::string& uri);

        int StartDownload();
        Piece ReadNextPiece();

        void set_file(int file_at);
        void set_download_dir(unique_cptr dir);
        void set_info(std::unique_ptr<lt::torrent_info> info);
        const lt::torrent_info& info() const;

    private:
        void Run();
        void SelectPieces(size_t offset);
        void HandleStateChanged(const lt::alert* alert);
        void HandleReadPiece(const lt::alert* alert);

        access_t*                         access_;
        int                               file_at_;
        std::atomic_bool                  stopped_;
        JoinableThread                    thread_;
        lt::fingerprint                   fingerprint_;
        lt::session                       session_;
        unique_cptr                       download_dir_;
        PiecesQueue                       queue_;
        std::unique_ptr<lt::torrent_info> info_;
        lt::torrent_handle                handle_;
};

inline void TorrentAccess::set_file(int file_at)
{
    file_at_ = file_at;
}

inline void TorrentAccess::set_download_dir(unique_cptr dir)
{
    download_dir_ = std::move(dir);
}

inline void TorrentAccess::set_info(std::unique_ptr<lt::torrent_info> info)
{
    info_ = std::move(info);
}

inline const lt::torrent_info& TorrentAccess::info() const
{
    return *info_;
}
