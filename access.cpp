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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_access.h>
#include <vlc_plugin.h>
#include <vlc_url.h>
#include <vlc_input_item.h>
#include <vlc_configuration.h>

#include "torrent.h"

static int Open(vlc_object_t*);
static void Close(vlc_object_t*);
static int ReadDir(access_t*, input_item_node_t*);
static int Control(access_t*, int, va_list);
static int Seek(access_t*, uint64_t);
static block_t* Block(access_t*);

struct access_sys_t
{
    TorrentAccess torrent;
};

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/

vlc_module_begin()

    set_shortname(N_("Torrent streaming"))
    set_description(N_("Stream torrent files and magnet links"))
    set_capability("access", 51)
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACCESS)
    add_shortcut("torrent", "file", "magnet")
    set_callbacks(Open, Close)

    add_integer("torrent-file-index", -1, N_("Torrent file index"),
      N_("Index of the file to play within the torrent"), false)
    change_private()

    add_directory("download-dir", nullptr, N_("Download directory"),
      N_("Directory used to store dowloaded files"), false)
    add_bool("keep-files", true, N_("Keep downloaded files"),
      N_("Determine whether VLC keeps the dowloaded files or removes them after use"), false)
    add_integer("upload-rate-limit", 0, N_("Upload rate limit (kB/s) [0=unlimited]"),
      N_("Maximum upload rate in kilobytes per second"), false)
    add_integer("download-rate-limit", 0, N_("Download rate limit (kB/s) [0=unlimited]"),
      N_("Maximum download rate in kilobytes per second"), false)
    add_float("share-ratio-limit", 2.0, N_("Share ratio limit"),
      N_("Share ratio limit to maintain (uploaded bytes / downloaded bytes)"), false)

vlc_module_end()

/*****************************************************************************
 * Open:
 *****************************************************************************/

static unique_char_ptr var_GetDownloadDir(const access_t* p_access)
{
    auto dir = var_InheritString(p_access, "download-dir");
    if (dir == nullptr)
        dir = config_GetUserDir(VLC_DOWNLOAD_DIR);
    return {dir, std::free};
}

static int open(access_t* p_access)
{
    lt::add_torrent_params params;

    if (TorrentAccess::ParseURI(p_access->psz_location, params) != VLC_SUCCESS)
        return VLC_EGENERIC;

    auto dir = var_GetDownloadDir(p_access);
    if (dir == nullptr)
        return VLC_EGENERIC;

    p_access->p_sys = new access_sys_t{{p_access}};
    auto& torrent = p_access->p_sys->torrent;
    auto file_at = var_InheritInteger(p_access, "torrent-file-index");

    torrent.set_parameters(std::move(params));
    torrent.set_download_dir(std::move(dir));

    if (!torrent.has_torrent_metadata()) {
        // This is a magnet link, first we need to generate the torrent file.
        if (torrent.RetrieveTorrentMetadata() != VLC_SUCCESS)
            return VLC_EGENERIC;
    }
    if (file_at < 0) {
        // Browse the torrent metadata and generate a playlist with the files in it.
        ACCESS_SET_CALLBACKS(nullptr, nullptr, Control, nullptr);
        p_access->pf_readdir = ReadDir;
        return VLC_SUCCESS;
    }
    else {
        // Torrent file has been browsed, start the download.
        ACCESS_SET_CALLBACKS(nullptr, Block, Control, Seek);
        torrent.StartDownload(file_at);
        return VLC_SUCCESS;
    }
}

static int Open(vlc_object_t* p_this)
{
    auto p_access = (access_t*) p_this;

    try {
        auto r = open(p_access);
        if (r != VLC_SUCCESS)
            delete p_access->p_sys;
        else
            access_InitFields(p_access);
        return r;
    }
    catch (std::bad_alloc& e) {
        delete p_access->p_sys;
        return VLC_ENOMEM;
    }
}

/*****************************************************************************
 * Close:
 *****************************************************************************/

static void Close(vlc_object_t* p_this)
{
    auto p_access = (access_t*) p_this;
    delete p_access->p_sys;
}

/*****************************************************************************
 * Callbacks
 *****************************************************************************/

static int ReadDir(access_t* p_access, input_item_node_t* p_node)
{
    using ItemsHeap = std::map<uint64_t, input_item_t*>;

    const auto& torrent = p_access->p_sys->torrent;
    const auto& metadata = torrent.torrent_metadata();
    const auto& files = metadata.files();

    ItemsHeap items;
    for (auto i = 0; i < metadata.num_files(); ++i) {
        const auto f = metadata.file_at(i);
        const auto psz_uri = torrent.uri().c_str();
        const auto psz_name = files.file_name(i);
        const auto psz_option = "torrent-file-index=" + std::to_string(i);

        auto p_item = input_item_New(psz_uri, psz_name.c_str());
        input_item_AddOption(p_item, psz_option.c_str(), VLC_INPUT_OPTION_TRUSTED);
        items[f.size] = p_item;
    }
    std::for_each(items.rbegin(), items.rend(), [p_node](ItemsHeap::value_type& p) {
        input_item_node_AppendItem(p_node, p.second);
        input_item_Release(p.second);
    });

    return VLC_SUCCESS;
}

static int Control(access_t* p_access, int i_query, va_list args)
{
    switch(i_query) {
    case ACCESS_CAN_FASTSEEK:
        *va_arg(args, bool*) = false;
        break;

    case ACCESS_CAN_PAUSE:
    case ACCESS_CAN_SEEK:
    case ACCESS_CAN_CONTROL_PACE:
        *va_arg(args, bool*) = true;
        break;

    case ACCESS_GET_PTS_DELAY:
        *va_arg(args, int64_t*) = DEFAULT_PTS_DELAY * 1000;
        break;

    case ACCESS_SET_PAUSE_STATE:
        return VLC_SUCCESS;

    case ACCESS_GET_TITLE_INFO:
    case ACCESS_SET_TITLE:
    case ACCESS_SET_PRIVATE_ID_STATE:
        return VLC_EGENERIC;

    default:
        msg_Warn(p_access, "unimplemented query in control");
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static block_t* Block(access_t* p_access)
{
    Piece p;
    bool eof;

    auto& torrent = p_access->p_sys->torrent;
    torrent.ReadNextPiece(p, eof);

    p_access->info.b_eof = eof;
    if (eof || p.data == nullptr)
        return nullptr;
    p_access->info.i_pos += p.length;
    return p.data.release();
}

static int Seek(access_t *p_access, uint64_t i_pos)
{
    auto& torrent = p_access->p_sys->torrent;
    torrent.SelectPieces(i_pos);
    p_access->info.i_pos = i_pos;
    return VLC_SUCCESS;
}
