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
#include <libtorrent/add_torrent_params.hpp>

#include "thread.h"

namespace lt = libtorrent;

using namespace std::string_literals;
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
            download_dir_{nullptr, std::free} {
            uri_ = "torrent://"s + p_access->psz_location;
        }
        ~TorrentAccess() {
            stopped_ = true;
        }

        static int ParseURI(const std::string& uri, lt::add_torrent_params& params);
        int RetrieveMetadata();
        int StartDownload();
        Piece ReadNextPiece();

        void set_file(int file_at);
        void set_download_dir(unique_cptr&& dir);
        void set_parameters(lt::add_torrent_params&& params);
        const lt::torrent_info& metadata() const;
        bool has_metadata() const;
        const std::string& uri() const;

    private:
        void Run();
        void SelectPieces(size_t offset);
        void HandleStateChanged(const lt::alert* alert);
        void HandleReadPiece(const lt::alert* alert);

        access_t*               access_;
        int                     file_at_;
        std::string             uri_;
        std::atomic_bool        stopped_;
        VLC::JoinableThread     thread_;
        lt::fingerprint         fingerprint_;
        lt::session             session_;
        unique_cptr             download_dir_;
        PiecesQueue             queue_;
        Status                  status_;
        lt::add_torrent_params  params_;
        lt::torrent_handle      handle_;
};

inline void TorrentAccess::set_file(int file_at)
{
    file_at_ = file_at;
}

inline void TorrentAccess::set_download_dir(unique_cptr&& dir)
{
    download_dir_ = std::move(dir);
}

inline void TorrentAccess::set_parameters(lt::add_torrent_params&& params)
{
    params_ = std::move(params);
}

inline const lt::torrent_info& TorrentAccess::metadata() const
{
    return *params_.ti;
}

inline bool TorrentAccess::has_metadata() const
{
    return (params_.ti == nullptr) ? false : true;
}

inline const std::string& TorrentAccess::uri() const
{
    return uri_;
}
