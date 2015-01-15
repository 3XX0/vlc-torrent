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

#include <cassert>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <functional>
#include <fstream>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/extensions/metadata_transfer.hpp>
#include <libtorrent/extensions/ut_metadata.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/bencode.hpp>

#include "torrent.h"

#include <vlc_url.h>

int TorrentAccess::ParseURI(const std::string& uri, lt::add_torrent_params& params)
{
    std::string prefix = "magnet:?";
    lt::error_code ec;

    auto pos = uri.find_last_of("/");
    auto stripped_uri = (pos == std::string::npos) ? uri : uri.substr(pos + 1);
    stripped_uri = decode_URI_duplicate(stripped_uri.c_str());

    if (!stripped_uri.compare(0, prefix.size(), prefix)) {
        lt::parse_magnet_uri(stripped_uri, params, ec);
        if (ec)
            return VLC_EGENERIC;
    }
    else {
        params.ti = new lt::torrent_info{uri, ec};
        if (ec)
            return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

int TorrentAccess::RetrieveMetadata()
{
    lt::error_code ec;

    assert(download_dir_ != nullptr);

    session_.set_alert_mask(lt::alert::status_notification);
    session_.add_extension(&lt::create_metadata_plugin);
    session_.add_extension(&lt::create_ut_metadata_plugin);
    params_.url = access_->psz_location;
    handle_ = session_.add_torrent(params_, ec);
    if (ec)
        return VLC_EGENERIC;

    Run();
    session_.remove_torrent(handle_);

    const auto& metadata = handle_.get_torrent_info();
    params_.ti = new lt::torrent_info{metadata};

    // Create the torrent file.
    auto torrent = lt::create_torrent{metadata};
    auto path = download_dir_.get() + "/"s + metadata.name() + ".torrent";
    std::ofstream file{path, std::ios_base::binary};
    if (!file.is_open())
        return VLC_EGENERIC;
    lt::bencode(std::ostream_iterator<char>{file}, torrent.generate());
    uri_ = "torrent://" + path; // Change the initial URI to point to the torrent generated.

    msg_Info(access_, "Metadata successfully retrieved, torrent file created");
    return VLC_SUCCESS;
}

int TorrentAccess::StartDownload()
{
    lt::error_code ec;

    assert(has_metadata() && file_at_ > 0 && download_dir_ != nullptr);

    session_.set_alert_mask(lt::alert::status_notification | lt::alert::storage_notification);
    params_.save_path = download_dir_.get();
    params_.storage_mode = lt::storage_mode_allocate;
    handle_ = session_.add_torrent(params_, ec);
    if (ec)
        return VLC_EGENERIC;

    SelectPieces(0);
    handle_.set_sequential_download(true);
    status_.state = handle_.status().state;

    auto run = std::bind(std::mem_fn(&TorrentAccess::Run), this);
    return thread_.Start(access_, run);
}

void TorrentAccess::Run()
{
    std::deque<lt::alert*> alerts;

    while (!stopped_) {
        if (!session_.wait_for_alert(lt::microsec(500)))
            continue;

        session_.pop_alerts(&alerts);
        for (const auto a : alerts) {
            switch (a->type()) {
                case lt::state_changed_alert::alert_type:
                    HandleStateChanged(a);
                    break;
                case lt::read_piece_alert::alert_type:
                    HandleReadPiece(a);
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
    const auto& meta = metadata();
    const auto& file = meta.file_at(file_at_ - 1);
    auto req = meta.map_file(file_at_ - 1, offset, file.size - offset);
    auto piece_size = meta.piece_length();
    auto num_pieces = meta.num_pieces();
    auto req_pieces = std::ceil((float) (req.length + req.start) / piece_size);

    std::unique_lock<VLC::Mutex> lock{queue_.mutex};
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
        queue_.pieces.push_back({i, off, len});
        req.length -= len;
    }
}

void TorrentAccess::HandleStateChanged(const lt::alert* alert)
{
    const auto a = lt::alert_cast<lt::state_changed_alert>(alert);
    const char* msg;

    switch (a->state) {
        case lt::torrent_status::queued_for_checking:
            msg = "Queued for checking";
            break;
        case lt::torrent_status::downloading_metadata:
            msg = "Downloading metadata";
            break;
        case lt::torrent_status::finished:
            msg = "Finished";
            break;
        case lt::torrent_status::allocating:
            msg = "Allocating space";
            break;
        case lt::torrent_status::checking_resume_data:
            msg = "Resuming";
            break;
        case lt::torrent_status::checking_files:
            msg = "Checking files";
            break;
        case lt::torrent_status::seeding:
            msg = "Seeding";
            break;
        case lt::torrent_status::downloading:
            msg = "Downloading";
            break;
        default:
            return;
    }
    msg_Info(access_, "State changed to: %s", msg);

    std::unique_lock<VLC::Mutex> lock{status_.mutex};
    status_.state = a->state;
    status_.cond.Signal();
}

void TorrentAccess::HandleReadPiece(const lt::alert* alert) // TODO read error
{
    const auto a = lt::alert_cast<lt::read_piece_alert>(alert);

    std::unique_lock<VLC::Mutex> lock{queue_.mutex};

    auto p = std::find_if(std::begin(queue_.pieces), std::end(queue_.pieces),
      [a](const Piece& p) { return a->piece == p.id; }
    );
    if (p == std::end(queue_.pieces) || p->data != nullptr)
        return;

    assert(a->size >= p->length);
    p->data = {block_Alloc(p->length), block_Release};
    std::memcpy(p->data->p_buffer, a->buffer.get() + p->offset, p->length);
    if (p->id == queue_.pieces.front().id)
        queue_.cond.Signal();
}

void TorrentAccess::ReadNextPiece(Piece& piece, bool& eof)
{
    eof = false;

    std::unique_lock<VLC::Mutex> s_lock{status_.mutex};
    if (!status_.cond.WaitFor(s_lock, 500ms, [this]{ return status_.state >= lt::torrent_status::downloading; }))
        return;
    s_lock.unlock();

    std::unique_lock<VLC::Mutex> q_lock{queue_.mutex};
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
    if (!queue_.cond.WaitFor(q_lock, 500ms, [&next_piece]{ return next_piece.data != nullptr; }))
        return;

    piece = std::move(next_piece);
    queue_.pieces.pop_front();
    msg_Dbg(access_, "Got piece: %d", piece.id);
}
