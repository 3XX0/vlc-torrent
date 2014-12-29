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
static block_t* Block(access_t*);

struct access_sys_t
{
    TorrentAccess torrent;
};

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/

#define DLDIR_TEXT N_("Download directory")
#define DLDIR_LONGTEXT N_("Directory used to store dowloaded files")

vlc_module_begin()

    set_shortname(N_("Torrent"))
    set_description(N_("Torrent file"))
    set_capability("access", 51)
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_ACCESS)
    add_shortcut("torrent", "file")
    set_callbacks(Open, Close)

    add_integer("file_at", nullptr, nullptr, nullptr, false)
    change_private()
    add_string("download_dir", nullptr, DLDIR_TEXT, DLDIR_LONGTEXT, false)

    // TODO
    //add_submodule()
    //    set_section( N_("Magnet input" ), NULL )
    //    set_capability( "access", 0 )
    //    add_shortcut( "magnet" )
    //set_callbacks( Open, Close )

vlc_module_end()

/*****************************************************************************
 * Open:
 *****************************************************************************/

static unique_cptr var_GetDownloadDir(const access_t* p_access)
{
    auto dir = var_InheritString(p_access, "download_dir");
    if (dir == nullptr)
        dir = config_GetUserDir(VLC_DOWNLOAD_DIR);
    return {dir, std::free};
}

static int open(access_t* p_access)
{
    auto dir = var_GetDownloadDir(p_access);
    if (dir == nullptr)
        return VLC_EGENERIC;
    auto info = TorrentAccess::ParseURI(p_access->psz_location);
    if (info == nullptr)
        return VLC_EGENERIC;
    p_access->p_sys = new access_sys_t{{p_access}};

    auto& torrent = p_access->p_sys->torrent;
    auto file_at = var_InheritInteger(p_access, "file_at");

    torrent.set_download_dir(std::move(dir));
    torrent.set_info(std::move(info));

    if (file_at == 0) { // Browse the torrent file and list the files in it.
        ACCESS_SET_CALLBACKS(nullptr, nullptr, Control, nullptr);
        p_access->pf_readdir = ReadDir;
        return VLC_SUCCESS;
    }

    // Torrent file has been browsed, start the download.
    ACCESS_SET_CALLBACKS(nullptr, Block, Control, nullptr);
    torrent.set_file(file_at);
    return torrent.StartDownload();
}

static int Open(vlc_object_t* p_this)
{
    auto p_access = (access_t*) p_this;
    access_InitFields(p_access);

    try {
        auto r = open(p_access);
        if (r != VLC_SUCCESS)
            delete p_access->p_sys;
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
    const auto& torrent = p_access->p_sys->torrent;
    const auto& info = torrent.info();

    msg_Info(p_access, "Browsing torrent file '%s'", info.name().c_str());

    auto i = 1;
    for (auto f = info.begin_files(); f != info.end_files(); ++f, ++i) {
        const auto psz_uri = p_node->p_item->psz_uri;
        const auto psz_name = f->filename();
        const auto psz_option = "file_at=" + std::to_string(i);

        auto p_item = input_item_New(psz_uri, psz_name.c_str());
        input_item_AddOption(p_item, psz_option.c_str(), VLC_INPUT_OPTION_TRUSTED);
        input_item_node_AppendItem(p_node, p_item);
        input_item_Release(p_item);

        msg_Info(p_access, "Adding '%s'", psz_name.c_str());
    }
    return VLC_SUCCESS;
}

static int Control(access_t* p_access, int i_query, va_list args)
{
    switch(i_query) {
    case ACCESS_CAN_SEEK:
    case ACCESS_CAN_FASTSEEK:
        *va_arg(args, bool*) = false;
        break;

    case ACCESS_CAN_PAUSE:
    case ACCESS_CAN_CONTROL_PACE:
        *va_arg(args, bool*) = true;
        break;

    case ACCESS_GET_PTS_DELAY:
        *va_arg(args, int64_t *) = DEFAULT_PTS_DELAY * 1000;
        break;

    case ACCESS_SET_PAUSE_STATE:
    case ACCESS_GET_TITLE_INFO:
    case ACCESS_SET_TITLE:
    case ACCESS_SET_SEEKPOINT:
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
    auto& torrent = p_access->p_sys->torrent;
    auto p = torrent.ReadNextPiece();
    return p.data;
}
