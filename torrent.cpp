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

#include <vlc_common.h>
#include <vlc_url.h>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/extensions/metadata_transfer.hpp>
#include <libtorrent/extensions/ut_metadata.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/bencode.hpp>

#include "torrent.h"

TorrentAccess::~TorrentAccess()
{
    stopped_ = true;
    session_.pause();
    if (handle_.is_valid())
        session_.remove_torrent(handle_);
    if (thread_.joinable())
        thread_.join();
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
        // XXX depending on the version of libtorrent, torrent_info is either a
        // boost::intrusive_ptr or a boost::shared_ptr. Use decltype to handle them both.
        params.ti = decltype(params.ti){new lt::torrent_info{uri_decoded, ec}};
        if (ec)
            return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

int TorrentAccess::RetrieveMetadata()
{
    lt::error_code ec;

    session_.set_alert_mask(lta::status_notification);
    session_.add_extension(&lt::create_metadata_plugin);
    session_.add_extension(&lt::create_ut_metadata_plugin);
    handle_ = session_.add_torrent(params_, ec);
    if (ec)
        return VLC_EGENERIC;

    Run();
    session_.remove_torrent(handle_);

    const auto& metadata = handle_.get_torrent_info();
    // XXX depending on the version of libtorrent, torrent_info is either a
    // boost::intrusive_ptr or a boost::shared_ptr. Use decltype to handle them both.
    params_.ti = decltype(params_.ti){new lt::torrent_info{metadata}};

    // Create the torrent file.
    const auto torrent = lt::create_torrent{metadata};
    const auto filename = metadata.info_hash().to_string() + ".torrent";
    const auto path = SaveFileBencoded(filename, torrent.generate());
    if (path.length() == 0)
        return VLC_EGENERIC;
    uri_ = "torrent://" + path; // Change the initial URI to point to the torrent generated.

    return VLC_SUCCESS;
}

int TorrentAccess::StartDownload(int file_at)
{
    lt::error_code ec;

    assert(has_metadata() && file_at >= 0 && download_dir_ != nullptr);

    session_.set_alert_mask(lta::status_notification | lta::storage_notification | lta::progress_notification);
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

    auto lock = std::unique_lock<std::mutex>{queue_.mutex};
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

    auto lock = std::unique_lock<std::mutex>{status_.mutex};
    status_.state = a->state;
    status_.cond.notify_one();
}

void TorrentAccess::HandleReadPiece(const lt::alert* alert)
{
    const auto a = lt::alert_cast<lt::read_piece_alert>(alert);

    if (a->buffer == nullptr) { // Read error, try again.
        handle_.read_piece(a->piece);
        return;
    }

    auto lock = std::unique_lock<std::mutex>{queue_.mutex};

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
    auto timeout = std::chrono::milliseconds{500};
    eof = false;

    {
        auto lock = std::unique_lock<std::mutex>{status_.mutex};
        auto cond = status_.cond.wait_for(lock, timeout, [s = status_.state]{
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
        handle_.set_piece_deadline(next_piece.id, 0, lt::torrent_handle::alert_when_available);
        next_piece.requested = true;
        msg_Dbg(access_, "Piece requested: %d", next_piece.id);
    }
    if (!queue_.cond.wait_for(lock, timeout, [&next_piece]{ return next_piece.data != nullptr; }))
        return;

    piece = std::move(next_piece);
    queue_.pieces.pop_front();
    msg_Dbg(access_, "Got piece: %d", piece.id);
}

std::string TorrentAccess::SaveFileBencoded(const std::string& name, const lt::entry& entry) const
{
    if (cache_dir_ == nullptr)
        return {};

    const auto path = std::string{cache_dir_.get()} + "/" + name;
    std::ofstream file{path, std::ios_base::binary};
    if (!file.is_open())
        return {};
    lt::bencode(std::ostream_iterator<char>{file}, entry);

    return path;
}
