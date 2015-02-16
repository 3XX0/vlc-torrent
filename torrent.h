/*****************************************************************************
 * Copyright (C) 2014 VLC authors, VideoLAN and Videolabs
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

#include <string>
#include <cstdlib>
#include <memory>
#include <deque>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_access.h>

#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/add_torrent_params.hpp>

namespace lt = libtorrent;

using lta = lt::alert;
using lts = lt::torrent_status;
using unique_char_ptr = std::unique_ptr<char, void (*)(void*)>;
using unique_block_ptr = std::unique_ptr<block_t, void (*)(block_t*)>;

struct Piece
{
    Piece() : Piece{0, 0, 0} {}
    Piece(int i, int off, int len) :
        id{i},
        offset{off},
        length{len},
        requested{false},
        data{nullptr, block_Release}
    {}

    int              id;
    int              offset;
    int              length;
    bool             requested;
    unique_block_ptr data;
};

struct PiecesQueue
{
    std::mutex              mutex;
    std::condition_variable cond;
    std::deque<Piece>       pieces;
};

struct Status
{
    std::mutex              mutex;
    std::condition_variable cond;
    lts::state_t            state;
};

class TorrentAccess
{
    public:
        TorrentAccess(access_t* p_access) :
            access_{p_access},
            file_at_{-1},
            stopped_{false},
            download_dir_{nullptr, std::free},
            cache_dir_{config_GetUserDir(VLC_CACHE_DIR), std::free},
            uri_{std::string{"torrent://"} + p_access->psz_location},
            fingerprint_{"VL", PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR,
                               PACKAGE_VERSION_REVISION, PACKAGE_VERSION_EXTRA},
            session_{fingerprint_}
        {}
        ~TorrentAccess();

        static int ParseURI(const std::string& uri, lt::add_torrent_params& params);
        int RetrieveMetadata();
        int StartDownload(int file_at);
        void ReadNextPiece(Piece& piece, bool& eof);
        void SelectPieces(uint64_t offset);

        void set_download_dir(unique_char_ptr&& dir);
        void set_parameters(lt::add_torrent_params&& params);
        const lt::torrent_info& metadata() const;
        bool has_metadata() const;
        const std::string& uri() const;

    private:
        void Run();
        void HandleStateChanged(const lt::alert* alert);
        void HandleReadPiece(const lt::alert* alert);
        std::string CacheSave(const std::string& name, const lt::entry& entry) const;
        std::string CacheLookup(const std::string& name) const;

        void set_uri(const std::string& uri);
        void set_metadata(const lt::torrent_info& metadata);
        void set_metadata(const std::string& path, lt::error_code& ec);

        access_t*               access_;
        int                     file_at_;
        std::atomic_bool        stopped_;
        unique_char_ptr         download_dir_;
        unique_char_ptr         cache_dir_;
        std::string             uri_;
        lt::fingerprint         fingerprint_;
        lt::session             session_;
        PiecesQueue             queue_;
        Status                  status_;
        lt::add_torrent_params  params_;
        lt::torrent_handle      handle_;
        std::thread             thread_;
};

inline void TorrentAccess::set_download_dir(unique_char_ptr&& dir)
{
    download_dir_ = std::move(dir);
}

inline void TorrentAccess::set_parameters(lt::add_torrent_params&& params)
{
    params_ = std::move(params);
}

inline void TorrentAccess::set_metadata(const lt::torrent_info& metadata)
{
    // XXX depending on the version of libtorrent, torrent_info is either a
    // boost::intrusive_ptr or a boost::shared_ptr. Use decltype to handle them both.
    params_.ti = decltype(params_.ti){new lt::torrent_info{metadata}};
}

inline void TorrentAccess::set_metadata(const std::string& path, lt::error_code& ec)
{
    set_metadata({path, ec});
    if (ec)
        params_.ti.reset();
}

inline const lt::torrent_info& TorrentAccess::metadata() const
{
    return *params_.ti;
}

inline bool TorrentAccess::has_metadata() const
{
    return params_.ti != nullptr;
}

inline void TorrentAccess::set_uri(const std::string& uri)
{
    uri_ = uri;
}

inline const std::string& TorrentAccess::uri() const
{
    return uri_;
}
