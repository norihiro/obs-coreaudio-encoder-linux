/*
 * OBS CoreAudio Encoder Plugin for Linux
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

#include <obs-module.h>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include <util/util.hpp>
#include "plugin-macros.generated.h"
#include "encoder-proc/encoder-proc.h"
#include "encoder-proc/encoder-proc-version.h"
#include "run-proc.h"

namespace {

struct ca_encoder
{
	obs_encoder_t *encoder = nullptr;

	size_t out_frames_per_packet = 0;

	std::vector<uint8_t> encode_buffer;

	uint64_t samples_per_second = 0;

	std::vector<uint8_t> extra_data;

	pid_t pid = -1;
	int fd_req = -1;
	int fd_data = -1;
	int fd_err = -1;

	std::thread stderr_thread;

	~ca_encoder()
	{
		if (fd_req >= 0)
			close(fd_req);
		if (fd_data >= 0)
			close(fd_data);

		if (pid > 0) {
			int wstatus = 0;
			pid_t ret = waitpid(pid, &wstatus, 0);
			if (ret == pid) {
				blog(LOG_INFO, "[%s] process %d terminated", obs_encoder_get_name(encoder), (int)pid);
				pid = -1;
			}
		}

		if (stderr_thread.joinable())
			stderr_thread.join();

		if (fd_err >= 0)
			close(fd_err);
	}

	const char *name() const
	{
		if (encoder)
			return obs_encoder_get_name(encoder);
		else
			return "";
	}
};

} // namespace

static void stderr_thread_routine(ca_encoder *ca)
{
	std::vector<char> buf;

	while (true) {
		size_t start = buf.size();
		buf.resize(start + 1024);
		ssize_t n = read(ca->fd_err, buf.data() + start, buf.size() - start);
		if (n >= 0)
			buf.resize(start + n);

		start = 0;
		for (size_t i = 0; i < buf.size(); i++) {
			if (buf[i] == '\n') {
				buf[i] = 0;
				blog(LOG_INFO, "[%s] pipe: %s", ca->name(), buf.data() + start);
				start = i + 1;
			}
		}
		if (start)
			buf.erase(buf.begin(), buf.begin() + start);

		if (n <= 0) {
			blog(LOG_INFO, "[%s] pipe closed", ca->name());
			return;
		}
	}
}

static const char *aac_get_name(void *)
{
	return obs_module_text("CoreAudioAAC");
}

static void aac_destroy(void *data)
{
	ca_encoder *ca = static_cast<ca_encoder *>(data);

	delete ca;
}

static inline bool start_proc(ca_encoder *ca)
{
	BPtr<char> proc_path = obs_module_file("obs-coreaudio-encoder-proc.exe");
	ca->pid = run_proc(proc_path, &ca->fd_req, &ca->fd_data, &ca->fd_err, NULL);
	if (ca->pid < 0) {
		blog(LOG_ERROR, "Failed to create Wine process for '%s'", proc_path.Get());
		return false;
	}

	ca->stderr_thread = std::thread([ca] { stderr_thread_routine(ca); });

	return true;
}

static inline bool transfer_encoder_settings(ca_encoder *ca, struct encoder_settings *settings)
{
	if (write(ca->fd_req, settings, sizeof(*settings)) != sizeof(*settings)) {
		blog(LOG_ERROR, "[%s] Failed to write encoder-settings to the co-process", ca->name());
		return false;
	}

	if (read(ca->fd_data, settings, sizeof(*settings)) != sizeof(*settings)) {
		blog(LOG_ERROR, "[%s] Failed to read encoder-settings from the co-process", ca->name());
		return false;
	}

	return true;
}

static void *aac_create(obs_data_t *settings, obs_encoder_t *encoder)
{

	uint32_t bitrate = (uint32_t)obs_data_get_int(settings, "bitrate") * 1000;
	if (!bitrate) {
		blog(LOG_ERROR, "[%s] Invalid bitrate specified", obs_encoder_get_name(encoder));
		return NULL;
	}

	std::unique_ptr<ca_encoder> ca;

	try {
		ca.reset(new ca_encoder());
	} catch (...) {
		blog(LOG_ERROR, "Could not allocate encoder");
		return nullptr;
	}

	ca->encoder = encoder;

	audio_t *audio = obs_encoder_audio(encoder);

	ca->samples_per_second = audio_output_get_sample_rate(audio);

	struct encoder_settings encoder_settings = {
		.struct_size = sizeof(encoder_settings),
		.proc_version = ENCODER_PROC_VERSION,
		.bitrate = bitrate,
		.channels = (uint32_t)audio_output_get_channels(audio),
		.samplerate_in = (uint32_t)ca->samples_per_second,
		.samplerate_out = (uint32_t)obs_data_get_int(settings, "samplerate"),
		.flags = 0,
		.out_frames_per_packet = 0,
	};
	if (obs_data_get_bool(settings, "allow he-aac"))
		encoder_settings.flags |= ENCODER_FLAG_ALLOW_HE_AAC;

	if (!start_proc(ca.get()))
		return NULL;

	if (!transfer_encoder_settings(ca.get(), &encoder_settings))
		return NULL;

	ca->out_frames_per_packet = encoder_settings.out_frames_per_packet;

	return ca.release();
}

static bool write_header_data(ca_encoder *ca, const struct encoder_data_header &header, const uint8_t *data,
			      const char *msg)
{
	if (write(ca->fd_req, &header, sizeof(header)) != sizeof(header)) {
		blog(LOG_ERROR, "[%s] Failed to write header for %s", ca->name(), msg);
		return false;
	}

	if (header.size && write(ca->fd_req, data, header.size) != header.size) {
		blog(LOG_ERROR, "[%s] Failed to write data for %s", ca->name(), msg);
		return false;
	}

	return true;
}

static bool aac_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received_packet)
{
	ca_encoder *ca = static_cast<ca_encoder *>(data);

	struct encoder_data_header header = {
		.size = frame->linesize[0],
		.frames = 1,
		.pts = frame->pts,
		.flags = ENCODER_FLAG_QUERY_ENCODE,
	};

	if (!write_header_data(ca, header, frame->data[0], "frame"))
		return false;

	if (read(ca->fd_data, &header, sizeof(header)) != sizeof(header)) {
		blog(LOG_INFO, "[%s] Failed to read encoded packet header", ca->name());
		return false;
	}

	if (!header.size) {
		*received_packet = false;
		return true;
	}

	ca->encode_buffer.resize(header.size);
	if (read(ca->fd_data, ca->encode_buffer.data(), header.size) != header.size) {
		blog(LOG_INFO, "[%s] Failed to read encoded packet data", ca->name());
		return false;
	}

	*received_packet = true;

	packet->pts = header.pts;
	packet->dts = header.pts;
	packet->timebase_num = 1;
	packet->timebase_den = (uint32_t)ca->samples_per_second;
	packet->type = OBS_ENCODER_AUDIO;
	packet->keyframe = true;
	packet->size = header.size;
	packet->data = ca->encode_buffer.data();

	return true;
}

static void aac_audio_info(void *, struct audio_convert_info *info)
{
	info->format = AUDIO_FORMAT_FLOAT;
}

static size_t aac_frame_size(void *data)
{
	ca_encoder *ca = static_cast<ca_encoder *>(data);
	return ca->out_frames_per_packet;
}

static void query_extra_data(ca_encoder *ca)
{
	struct encoder_data_header header = {
		.size = 0,
		.frames = 0,
		.pts = 0,
		.flags = ENCODER_FLAG_QUERY_EXTRA_DATA,
	};

	if (!write_header_data(ca, header, nullptr, "extra-data"))
		return;

	if (read(ca->fd_data, &header, sizeof(header)) != sizeof(header)) {
		blog(LOG_INFO, "[%s] Failed to read extra-data header", ca->name());
		return;
	}

	if (!header.size)
		return;

	ca->extra_data.resize(header.size);
	if (read(ca->fd_data, ca->extra_data.data(), header.size) != header.size) {
		blog(LOG_INFO, "[%s] Failed to read extra-data", ca->name());
		return;
	}
}

static bool aac_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	ca_encoder *ca = static_cast<ca_encoder *>(data);

	if (!ca->extra_data.size())
		query_extra_data(ca);

	if (!ca->extra_data.size())
		return false;

	*extra_data = ca->extra_data.data();
	*size = ca->extra_data.size();
	return true;
}

static const std::vector<uint32_t> &get_bitrates()
{
	static const std::vector<uint32_t> bitrates;
	static bool once = false;

	if (once)
		return bitrates;

	// TODO: Call the process and get the bitrates.

	return bitrates;
}

static bool find_best_match(uint32_t bitrate, uint32_t &best_match)
{
	uint32_t actual_bitrate = bitrate * 1000;
	bool found_match = false;

	auto handle_bitrate = [&](uint32_t candidate) {
		if (abs(static_cast<intmax_t>(actual_bitrate - candidate)) <
		    abs(static_cast<intmax_t>(actual_bitrate - best_match))) {
			found_match = true;
			best_match = candidate;
		}
	};

	for (uint32_t candidate : get_bitrates())
		handle_bitrate(candidate);

	best_match /= 1000;

	return found_match;
}

static uint32_t find_matching_bitrate(uint32_t bitrate)
{
	static uint32_t match = bitrate;

	static std::once_flag once;

	call_once(once, [&]() {
		if (!find_best_match(bitrate, match)) {
			match = bitrate;
			return;
		}
	});

	return match;
}

static void aac_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "samplerate", 0); //match input
	obs_data_set_default_int(settings, "bitrate", find_matching_bitrate(128));
	obs_data_set_default_bool(settings, "allow he-aac", true);
}

static std::vector<uint32_t> get_samplerates(ca_encoder *ca)
{
	std::vector<uint32_t> samplerates;

	(void)ca; // TODO: Implement

	return samplerates;
}

static void add_samplerates(obs_property_t *prop, ca_encoder *ca)
{
	obs_property_list_add_int(prop, obs_module_text("UseInputSampleRate"), 0);

	auto samplerates = get_samplerates(ca);

	if (!samplerates.size()) {
		blog(LOG_ERROR, "[%s] Couldn't find available sample rates", ca->name());
		return;
	}

	sort(begin(samplerates), end(samplerates));

	for (uint32_t samplerate : samplerates) {
		char buffer[32] = {0};
		snprintf(buffer, sizeof(buffer) - 1, "%u", samplerate);
		obs_property_list_add_int(prop, buffer, samplerate);
	}
}

static std::vector<uint32_t> get_bitrates(ca_encoder *ca, float samplerate)
{
	std::vector<uint32_t> bitrates;

	(void)ca;
	(void)samplerate; // TODO: Implement

	return bitrates;
}

static void add_bitrates(obs_property_t *prop, ca_encoder *ca, double samplerate = 44100., uint32_t *selected = nullptr)
{
	obs_property_list_clear(prop);

	auto bitrates = get_bitrates(ca, samplerate);

	if (!bitrates.size()) {
		blog(LOG_ERROR, "Couldn't find available bitrates");
		return;
	}

	bool selected_in_range = true;
	if (selected) {
		selected_in_range = find(begin(bitrates), end(bitrates), *selected * 1000) != end(bitrates);

		if (!selected_in_range)
			bitrates.push_back(*selected * 1000);
	}

	sort(begin(bitrates), end(bitrates));

	for (uint32_t bitrate : bitrates) {
		char buffer[32] = {0};
		snprintf(buffer, sizeof(buffer) - 1, "%u", bitrate / 1000);
		size_t idx = obs_property_list_add_int(prop, buffer, bitrate / 1000);

		if (selected_in_range || bitrate / 1000 != *selected)
			continue;

		obs_property_list_item_disable(prop, idx, true);
	}
}

static bool samplerate_updated(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	auto samplerate = static_cast<uint32_t>(obs_data_get_int(settings, "samplerate"));
	if (!samplerate)
		samplerate = 44100;

	prop = obs_properties_get(props, "bitrate");
	if (prop) {
		auto bitrate = static_cast<uint32_t>(obs_data_get_int(settings, "bitrate"));

		add_bitrates(prop, nullptr, samplerate, &bitrate);

		return true;
	}

	return false;
}

extern "C" obs_properties_t *aac_properties(void *data)
{

	obs_properties_t *props = obs_properties_create();

	obs_property_t *sample_rates = obs_properties_add_list(props, "samplerate", obs_module_text("OutputSamplerate"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_set_modified_callback(sample_rates, samplerate_updated);

	obs_property_t *bit_rates = obs_properties_add_list(props, "bitrate", obs_module_text("Bitrate"),
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_properties_add_bool(props, "allow he-aac", obs_module_text("AllowHEAAC"));

	if (data) {
		ca_encoder *ca = static_cast<ca_encoder *>(data);
		add_samplerates(sample_rates, ca);
		add_bitrates(bit_rates, ca);
	}

	return props;
}

extern "C" void register_aac_info()
{
	struct obs_encoder_info aac_info = {};
	aac_info.id = ID_PREFIX "CoreAudio_AAC";
	aac_info.type = OBS_ENCODER_AUDIO;
	aac_info.codec = "aac";
	aac_info.get_name = aac_get_name;
	aac_info.destroy = aac_destroy;
	aac_info.create = aac_create;
	aac_info.encode = aac_encode;
	aac_info.get_frame_size = aac_frame_size;
	aac_info.get_audio_info = aac_audio_info;
	aac_info.get_extra_data = aac_extra_data;
	aac_info.get_defaults = aac_defaults;
	aac_info.get_properties = aac_properties;

	obs_register_encoder(&aac_info);
};
