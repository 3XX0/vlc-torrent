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

#include <cassert>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <functional>
#include <fstream>
#include <chrono>
#include <future>
#include <unordered_map>

#include <vlc_common.h>
#include <vlc_url.h>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/extensions/metadata_transfer.hpp>
#include <libtorrent/extensions/ut_metadata.hpp>
#include <libtorrent/extensions/ut_pex.hpp>
#include <libtorrent/extensions/smart_ban.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/bencode.hpp>

#include "torrent.h"

TorrentAccess::~TorrentAccess()
{
    const auto keep_files = var_InheritBool(access_, "keep-files");

    session_.pause();
    if (handle_.is_valid()) {
        SaveSessionStates(keep_files);
        if (keep_files)
            session_.remove_torrent(handle_);
        else {
            session_.remove_torrent(handle_, lt::session::delete_files);
            CacheDel(torrent_hash() + ".torrent");
        }
    }

    stopped_ = true;
    if (thread_.joinable())
        thread_.join();
}

void TorrentAccess::SaveSessionStates(bool save_resume_data) const
{
    std::future<void> f;

    // Save the DHT state.
    try {
        const auto policy = save_resume_data ? std::launch::async : std::launch::deferred;
        f = std::async(policy, [this]{
            lt::entry state;
            session_.save_state(state, lt::session::save_dht_state);
            CacheSave("dht_state.dat", state);
        });
    }
    catch (std::system_error&) {}

    // Save resume data.
    if (save_resume_data) {
        handle_.save_resume_data(lth::flush_disk_cache);
        auto lock = std::unique_lock<std::mutex>{resume_data_.mutex};
        resume_data_.cond.wait(lock, [&s = resume_data_.saved]{ return s; });
    }

    if (f.valid())
        f.wait();
}

int TorrentAccess::ParseURI(const std::string& uri, lt::add_torrent_params& params)
{
    lt::error_code ec;

    const auto prefix = std::string{"magnet:?"};
    const auto uri_decoded = std::string{decode_URI_duplicate(uri.c_str())};

    if (!uri_decoded.compare(0, prefix.size(), prefix)) {
        lt::parse_magnet_uri(uri_decoded, params, ec);
        if (ec)
            return VLC_EGENERIC;
    }
    else {
        params.ti.reset(new lt::torrent_info{uri_decoded, ec});
        if (ec)
            return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

int TorrentAccess::RetrieveMetadata()
{
    lt::error_code ec;

    const auto filename = torrent_hash() + ".torrent";
    auto path = CacheLookup(filename);
    if (!path.empty()) {
        set_metadata(path, ec);
        if (!ec) {
            set_uri("torrent://" + path); // Change the initial URI to point to the torrent in cache.
            return VLC_SUCCESS;
        }
    }

    session_.set_alert_mask(lta::status_notification);
    session_.add_extension(&lt::create_metadata_plugin);
    session_.add_extension(&lt::create_ut_metadata_plugin);
    handle_ = session_.add_torrent(params_, ec);
    if (ec)
        return VLC_EGENERIC;

    Run();
    session_.remove_torrent(handle_);

    // Create the torrent file and save it in cache.
    const auto& metadata = handle_.get_torrent_info();
    set_metadata(metadata); // XXX must happen before create_torrent (create_torrent const_cast its args ...)
    const auto torrent = lt::create_torrent{metadata};
    path = CacheSave(filename, torrent.generate());
    if (path.empty())
        return VLC_EGENERIC;

    set_uri("torrent://" + path); // Change the initial URI to point to the torrent in cache.
    return VLC_SUCCESS;
}

int TorrentAccess::StartDownload(int file_at)
{
    lt::error_code ec;
    lt::lazy_entry entry;

    assert(has_metadata() && file_at >= 0 && download_dir_ != nullptr);

    session_.set_alert_mask(lta::status_notification | lta::storage_notification | lta::progress_notification);
    session_.add_extension(&lt::create_ut_pex_plugin);
    session_.add_extension(&lt::create_smart_ban_plugin);
    SetSessionSettings();

    // Start the DHT
    auto buf = CacheLoad("dht_state.dat");
    if (buf.size() > 0 && !lazy_bdecode(buf.data(), buf.data() + buf.size(), entry, ec) && !ec)
        session_.load_state(entry);
    session_.start_dht();

    // Attempt to fast resume the torrent.
    buf = CacheLoad(torrent_hash() + ".resume");
    if (buf.size() > 0)
        params_.resume_data = &buf;

    params_.save_path = download_dir_.get();
    params_.storage_mode = lt::storage_mode_allocate;
    handle_ = session_.add_torrent(params_, ec);
    if (ec)
        return VLC_EGENERIC;

    file_at_ = file_at;
    SelectPieces(0);
    handle_.set_sequential_download(true);
    status_.state = handle_.status().state;

    const auto run = std::bind(std::mem_fn(&TorrentAccess::Run), this);
    thread_ = std::thread{run};
    return VLC_SUCCESS;
}

void TorrentAccess::SetSessionSettings()
{
    auto s = session_.settings();

    const auto upload_rate = var_InheritInteger(access_, "upload-rate-limit");
    const auto download_rate = var_InheritInteger(access_, "download-rate-limit");
    const auto share_ratio = var_InheritFloat(access_, "share-ratio-limit");
    const auto user_agent = unique_char_ptr{var_InheritString(access_, "user-agent"), std::free};

    s.user_agent = std::string{user_agent.get()} + "/" VERSION " libtorrent/" LIBTORRENT_VERSION;
    s.active_downloads = 1;
    s.active_seeds = 1;
    s.announce_to_all_trackers = true;            // Announce in parallel to all trackers.
    s.use_dht_as_fallback = false;                // Use DHT regardless of trackers status.
    s.initial_picker_threshold = 0;               // Pieces to pick at random before doing rarest first picking.
    s.no_atime_storage = true;                    // Linux only O_NOATIME.
    s.no_recheck_incomplete_resume = true;        // Don't check the file when resume data is incomplete.
    s.max_queued_disk_bytes = 2 * 1024 * 1024;    // I/O thread buffer queue in bytes (may limit the download rate).
    s.cache_size = -1;                            // Disk read/write cache specified in units of 16 KiB (-1 for RAM/8).
    s.max_peerlist_size = 3000;                   // Maximum number of peers per torrent.
    s.num_want = 200;                             // Number of peers requested per tracker.
    s.torrent_connect_boost = s.num_want / 10;    // Number of peers to try to connect to immediately.
    s.share_ratio_limit = share_ratio;            // Share ratio limit (uploaded bytes / downloaded bytes).
    s.upload_rate_limit = upload_rate * 1024;     // Limits the upload speed in bytes/sec.
    s.download_rate_limit = download_rate * 1024; // Limits the download speed in bytes/sec.
    //s.recv_socket_buffer_size
    //s.send_socket_buffer_size

    session_.set_settings(s);

    const auto routers = std::unordered_map<std::string, int>{
        {"router.bittorrent.com", 6881},
        {"router.utorrent.com", 6881},
        {"router.bitcomet.com", 6881}
    };
    for (const auto& r : routers)
        session_.add_dht_router(r);
}

void TorrentAccess::Run()
{
    std::deque<lt::alert*> alerts;

    while (!stopped_) {
        if (!session_.wait_for_alert(lt::seconds(1)))
            continue;

        session_.pop_alerts(&alerts);
        for (const auto alert : alerts) {
            switch (alert->type()) {
                case lt::piece_finished_alert::alert_type: {
                    const auto a = lt::alert_cast<lt::piece_finished_alert>(alert);
                    msg_Dbg(access_, "Piece finished: %d", a->piece_index);
                    break;
                }
                case lt::state_changed_alert::alert_type:
                    HandleStateChanged(alert);
                    break;
                case lt::save_resume_data_alert::alert_type:
                    HandleSaveResumeData(alert);
                    break;
                case lt::read_piece_alert::alert_type:
                    HandleReadPiece(alert);
                    break;
                case lt::metadata_received_alert::alert_type: // Magnet file only.
                    return;
            }
        }
        alerts.clear();
    }
}

void TorrentAccess::SelectPieces(uint64_t offset)
{
    assert(has_metadata() && file_at_ >= 0);

    const auto& meta = metadata();
    const auto& file = meta.file_at(file_at_);
    auto req = meta.map_file(file_at_, offset, file.size - offset);
    const auto piece_size = meta.piece_length();
    const auto num_pieces = meta.num_pieces();
    const auto req_pieces = std::ceil((float) (req.length + req.start) / piece_size);

    const auto lock = std::unique_lock<std::mutex>{queue_.mutex};
    queue_.pieces.clear();

    for (auto i = 0; i < num_pieces; ++i) {
        if (i < req.piece || i >= req.piece + req_pieces) {
            handle_.piece_priority(i, 0); // Discard unwanted pieces.
            continue;
        }

        auto len = 0;
        auto off = 0;
        if (i == req.piece) { // First piece.
            off = req.start;
            len = (req.length < piece_size - off) ? req.length : piece_size - off;
        }
        else if (i == req.piece + req_pieces - 1) // Last piece.
            len = req.length;
        else
            len = piece_size;

        handle_.piece_priority(i, 7);
        queue_.pieces.emplace_back(i, off, len);
        req.length -= len;
    }
}

void TorrentAccess::HandleStateChanged(const lt::alert* alert)
{
    const auto a = lt::alert_cast<lt::state_changed_alert>(alert);
    const char* msg;

    switch (a->state) {
        case lts::queued_for_checking:
            msg = "Queued for checking";
            break;
        case lts::downloading_metadata:
            msg = "Downloading metadata";
            break;
        case lts::finished:
            msg = "Finished";
            break;
        case lts::allocating:
            msg = "Allocating space";
            break;
        case lts::checking_resume_data:
            msg = "Resuming";
            break;
        case lts::checking_files:
            msg = "Checking files";
            break;
        case lts::seeding:
            msg = "Seeding";
            break;
        case lts::downloading:
            msg = "Downloading";
            break;
        default:
            return;
    }
    msg_Info(access_, "State changed to: %s", msg);

    const auto lock = std::unique_lock<std::mutex>{status_.mutex};
    status_.state = a->state;
    status_.cond.notify_one();
}

void TorrentAccess::HandleSaveResumeData(const lt::alert* alert) const
{
    const auto a = lt::alert_cast<lt::save_resume_data_alert>(alert);

    if (a->resume_data != nullptr)
        CacheSave(torrent_hash() + ".resume", *a->resume_data);
    const auto lock = std::unique_lock<std::mutex>{resume_data_.mutex};
    resume_data_.saved = true;
    resume_data_.cond.notify_one();
}

void TorrentAccess::HandleReadPiece(const lt::alert* alert)
{
    const auto a = lt::alert_cast<lt::read_piece_alert>(alert);

    if (a->buffer == nullptr) { // Read error, try again.
        handle_.read_piece(a->piece);
        return;
    }

    const auto lock = std::unique_lock<std::mutex>{queue_.mutex};

    auto p = std::find_if(std::begin(queue_.pieces), std::end(queue_.pieces),
      [a](const Piece& p) { return a->piece == p.id; }
    );
    if (p == std::end(queue_.pieces) || p->data != nullptr)
        return;

    assert(a->size >= p->length);
    p->data = {block_Alloc(p->length), block_Release};
    std::memcpy(p->data->p_buffer, a->buffer.get() + p->offset, p->length);
    if (p->id == queue_.pieces.front().id)
        queue_.cond.notify_one();
}

void TorrentAccess::ReadNextPiece(Piece& piece, bool& eof)
{
    const auto timeout = std::chrono::milliseconds{500};
    eof = false;

    {
        auto lock = std::unique_lock<std::mutex>{status_.mutex};
        const auto cond = status_.cond.wait_for(lock, timeout, [&s = status_.state]{
                return s == lts::downloading || s == lts::finished || s == lts::seeding;
        });
        if (!cond)
            return;
    }

    auto lock = std::unique_lock<std::mutex>{queue_.mutex};
    if (queue_.pieces.empty()) {
        eof = true;
        return;
    }
    auto& next_piece = queue_.pieces.front();
    if (!next_piece.requested) {
        handle_.set_piece_deadline(next_piece.id, 0, lth::alert_when_available);
        next_piece.requested = true;
        msg_Dbg(access_, "Piece requested: %d", next_piece.id);
    }
    if (!queue_.cond.wait_for(lock, timeout, [&next_piece]{ return next_piece.data != nullptr; }))
        return;

    piece = std::move(next_piece);
    queue_.pieces.pop_front();
    msg_Dbg(access_, "Got piece: %d", piece.id);
}

std::string TorrentAccess::CacheSave(const std::string& name, const lt::entry& entry) const
{
    if (cache_dir_ == nullptr)
        return {};

    const auto path = std::string{cache_dir_.get()} + "/" + name;
    std::ofstream file{path, std::ios_base::binary | std::ios_base::trunc};
    if (!file)
        return {};

    lt::bencode(std::ostream_iterator<char>{file}, entry);
    return path;
}

std::string TorrentAccess::CacheLookup(const std::string& name) const
{
    if (cache_dir_ == nullptr)
        return {};

    const auto path = std::string{cache_dir_.get()} + "/" + name;
    std::ifstream file{path};
    if (!file.good())
        return {};

    return path;
}

std::vector<char> TorrentAccess::CacheLoad(const std::string& name) const
{
    if (cache_dir_ == nullptr)
        return {};

    const auto path = std::string{cache_dir_.get()} + "/" + name;
    std::ifstream file{path, std::ios_base::binary | std::ios_base::ate};
    if (!file.good())
        return {};

    const auto len = file.tellg();
    file.seekg(0);
    std::vector<char> buf(len);
    file.read(buf.data(), len);
    return buf;
}

void TorrentAccess::CacheDel(const std::string& name) const
{
    if (cache_dir_ == nullptr)
        return;

    const auto path = std::string{cache_dir_.get()} + "/" + name;
    std::remove(path.c_str());
}
