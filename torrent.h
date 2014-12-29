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

#include <string>
#include <cstdlib>
#include <memory>
#include <deque>
#include <atomic>

#include <vlc_common.h>
#include <vlc_access.h>

#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>

#include "thread.h"

namespace lt = libtorrent;

using unique_cptr = std::unique_ptr<char, void (*)(void*)>;

struct Piece
{
    int       id;
    int       offset;
    int       length;
    block_t*  data;
};

struct PiecesQueue
{
    VLC::Mutex        mutex;
    VLC::CondVar      cond;
    std::deque<Piece> pieces;
};

struct Status
{
    VLC::Mutex                  mutex;
    VLC::CondVar                cond;
    lt::torrent_status::state_t state;
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
        VLC::JoinableThread               thread_;
        lt::fingerprint                   fingerprint_;
        lt::session                       session_;
        unique_cptr                       download_dir_;
        PiecesQueue                       queue_;
        Status                            status_;
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
