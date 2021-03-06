/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPLAYER_ASPECT_H
#define MPLAYER_ASPECT_H

struct mp_log;
struct mp_vo_opts;
struct mp_image_params;
struct mp_rect;
struct mp_osd_res;
void mp_get_src_dst_rects(struct mp_log *log, struct mp_vo_opts *opts,
                          int vo_caps, struct mp_image_params *video,
                          int window_w, int window_h, double monitor_par,
                          struct mp_rect *out_src,
                          struct mp_rect *out_dst,
                          struct mp_osd_res *out_osd);

#endif /* MPLAYER_ASPECT_H */
