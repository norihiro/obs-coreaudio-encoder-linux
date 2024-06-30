/*
 * Copyright (C) 2024 Norihiro Kamae <norihiro@nagater.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENCODER_FLAG_ALLOW_HE_AAC (1 << 0)
#define ENCODER_FLAG_QUERY_ENCODE (1 << 1)
#define ENCODER_FLAG_QUERY_EXTRA_DATA (1 << 2)
#define ENCODER_FLAG_EXIT (1 << 3)

struct encoder_settings
{
	// Checked by the child process
	uint32_t struct_size;
	uint32_t proc_version;

	// Set from the main process
	uint32_t bitrate; // bps, not kbps
	uint32_t channels;
	uint32_t samplerate_in;
	uint32_t samplerate_out; // 0 to match samplerate_in
	uint32_t flags;

	// Set from the child process
	uint32_t out_frames_per_packet;
};

struct encoder_data_header
{
	uint32_t size;
	uint32_t frames;
	int64_t pts;
	uint32_t flags;
};

#ifdef __cplusplus
}
#endif
