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

#include <libtorrent/alert_types.hpp>

#include "torrent.h"

std::unique_ptr<lt::torrent_info> TorrentAccess::ParseURI(const std::string& uri)
{
    lt::error_code ec;

    auto info = std::make_unique<lt::torrent_info>(uri, ec);
    if (ec)
        return nullptr;
    return info;
}

int TorrentAccess::StartDownload()
{
    lt::add_torrent_params params;
    lt::error_code ec;

    assert(file_at_ > 0 && info_ != nullptr && download_dir_ != nullptr);

    params.ti = new lt::torrent_info{*info_};
    params.save_path = download_dir_.get();
    params.storage_mode = lt::storage_mode_allocate;
    session_.set_alert_mask(lt::alert::status_notification | lt::alert::storage_notification);
    handle_ = session_.add_torrent(params, ec);
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
        if (!session_.wait_for_alert(lt::time_duration(100))) // TODO time
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
            }
        }
        alerts.clear();
    }
}

void TorrentAccess::SelectPieces(size_t offset)
{
    const auto& file = info_->file_at(file_at_ - 1);
    auto req = info_->map_file(file_at_ - 1, offset, file.size - offset);
    auto piece_size = info_->piece_length();
    auto num_pieces = info_->num_pieces();
    auto req_pieces = std::ceil((float) req.length / piece_size);

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
        queue_.pieces.push_back({i, off, len, nullptr});
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
    status_.cond.signal();
}

void TorrentAccess::HandleReadPiece(const lt::alert* alert) // TODO read error
{
    const auto a = lt::alert_cast<lt::read_piece_alert>(alert);

    std::unique_lock<VLC::Mutex> lock{queue_.mutex};

    auto p = std::find_if(std::begin(queue_.pieces), std::end(queue_.pieces),
      [a](const Piece& p) { return a->piece == p.id; }
    );
    assert(p != std::end(queue_.pieces) && a->size >= p->length);

    p->data = block_Alloc(p->length);
    std::memcpy(p->data->p_buffer, a->buffer.get() + p->offset, p->length);
    if (p->id == queue_.pieces.front().id)
        queue_.cond.signal();
}

Piece TorrentAccess::ReadNextPiece()
{
    std::unique_lock<VLC::Mutex> s_lock{status_.mutex};
    status_.cond.wait_for(s_lock, 100, [this]{ return status_.state == lt::torrent_status::downloading; }); // TODO time
    s_lock.unlock();

    auto& next_piece = queue_.pieces.front();
    handle_.set_piece_deadline(next_piece.id, 0, lt::torrent_handle::alert_when_available);
    msg_Dbg(access_, "Piece requested: %d", next_piece.id);

    std::unique_lock<VLC::Mutex> q_lock{queue_.mutex};
    queue_.cond.wait_for(q_lock, 100, [&next_piece]{ return next_piece.data != nullptr; }); // TODO time
    auto p = std::move(next_piece);
    queue_.pieces.pop_front();

    msg_Dbg(access_, "Got piece: %d", p.id);
    return p;
}
