/*
 * Copyright (C) 2015 Ruwen Hahn <palana@stunned.de>
 * Copyright (C) 2024 Norihiro Kamae <norihiro@nagater.net>
 *
 * Original code by Ruwen Hahn [1]
 * Modified by Norihiro Kamae
 * - [1] https://github.com/obsproject/obs-studio/blob/9d67bf2662158e7e652972576a4dd436a792d628/plugins/coreaudio-encoder/encoder.cpp
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

#include <util/dstr.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <vector>
#include "encoder-proc.h"
#include "util.h"
#include "encoder-proc-version.h"

#ifndef _WIN32
#include <AudioToolbox/AudioToolbox.h>
#include <util/apple/cfstring-utils.h>
#endif

#define CA_LOG(level, format, ...) fprintf(stderr, format "\n", ##__VA_ARGS__)

#ifdef _WIN32
#include "windows-imports.h"
#endif

using namespace std;

namespace {

struct asbd_builder
{
	AudioStreamBasicDescription asbd;

	asbd_builder &sample_rate(Float64 rate)
	{
		asbd.mSampleRate = rate;
		return *this;
	}

	asbd_builder &format_id(UInt32 format)
	{
		asbd.mFormatID = format;
		return *this;
	}

	asbd_builder &format_flags(UInt32 flags)
	{
		asbd.mFormatFlags = flags;
		return *this;
	}

	asbd_builder &bytes_per_packet(UInt32 bytes)
	{
		asbd.mBytesPerPacket = bytes;
		return *this;
	}

	asbd_builder &frames_per_packet(UInt32 frames)
	{
		asbd.mFramesPerPacket = frames;
		return *this;
	}

	asbd_builder &bytes_per_frame(UInt32 bytes)
	{
		asbd.mBytesPerFrame = bytes;
		return *this;
	}

	asbd_builder &channels_per_frame(UInt32 channels)
	{
		asbd.mChannelsPerFrame = channels;
		return *this;
	}

	asbd_builder &bits_per_channel(UInt32 bits)
	{
		asbd.mBitsPerChannel = bits;
		return *this;
	}
};

struct ca_encoder
{
	UInt32 format_id = 0;

	const initializer_list<UInt32> *allowed_formats = nullptr;

	AudioConverterRef converter = nullptr;

	size_t output_buffer_size = 0;
	vector<uint8_t> output_buffer;

	size_t out_frames_per_packet = 0;

	size_t in_frame_size = 0;
	size_t in_bytes_required = 0;

	vector<uint8_t> input_buffer;
	vector<uint8_t> encode_buffer;

	uint64_t total_samples = 0;
	uint64_t samples_per_second = 0;
	uint32_t priming_samples = 0;

	vector<uint8_t> extra_data;

	size_t channels = 0;

	~ca_encoder()
	{
		if (converter)
			AudioConverterDispose(converter);
	}
};
typedef struct ca_encoder ca_encoder;

} // namespace

namespace std {

#ifndef _WIN32
template<> struct default_delete<remove_pointer<CFErrorRef>::type>
{
	void operator()(remove_pointer<CFErrorRef>::type *err) { CFRelease(err); }
};

template<> struct default_delete<remove_pointer<CFStringRef>::type>
{
	void operator()(remove_pointer<CFStringRef>::type *str) { CFRelease(str); }
};
#endif

template<> struct default_delete<remove_pointer<AudioConverterRef>::type>
{
	void operator()(AudioConverterRef converter) { AudioConverterDispose(converter); }
};

} // namespace std

static const initializer_list<UInt32> &get_allowed_formats(const struct encoder_settings *);

template<typename T> using cf_ptr = unique_ptr<typename remove_pointer<T>::type>;

#ifndef _MSC_VER
__attribute__((__format__(__printf__, 2, 3)))
#endif
static void
log_to_dstr(DStr &str, const char *fmt, ...)
{
	dstr prev_str = *static_cast<dstr *>(str);

	va_list args;
	va_start(args, fmt);
	dstr_vcatf(str, fmt, args);
	va_end(args);

	if (str->array)
		return;

	char array[4096];
	va_start(args, fmt);
	vsnprintf(array, sizeof(array), fmt, args);
	va_end(args);

	array[4095] = 0;

	if (!prev_str.array && !prev_str.len)
		CA_LOG(LOG_ERROR,
		       "Could not allocate buffer for logging:"
		       "\n'%s'",
		       array);
	else
		CA_LOG(LOG_ERROR,
		       "Could not allocate buffer for logging:"
		       "\n'%s'\nPrevious log entries:\n%s",
		       array, prev_str.array);

	bfree(prev_str.array);
}

static const char *flush_log(DStr &log)
{
	if (!log->array || !log->len)
		return "";

	if (log->array[log->len - 1] == '\n') {
		log->array[log->len - 1] = 0; //Get rid of last newline
		log->len -= 1;
	}

	return log->array;
}

#define CA_CO_DLOG_(level, format) CA_LOG(level, format "%s%s", log->array ? ":\n" : "", flush_log(log))
#define CA_CO_DLOG(level, format, ...) \
	CA_LOG(level, format "%s%s", ##__VA_ARGS__, log->array ? ":\n" : "", flush_log(log))

static const char *code_to_str(OSStatus code)
{
	switch (code) {
#define HANDLE_CODE(c) \
	case c:        \
		return #c
		HANDLE_CODE(kAudio_UnimplementedError);
		HANDLE_CODE(kAudio_FileNotFoundError);
		HANDLE_CODE(kAudio_FilePermissionError);
		HANDLE_CODE(kAudio_TooManyFilesOpenError);
		HANDLE_CODE(kAudio_BadFilePathError);
		HANDLE_CODE(kAudio_ParamError);
		HANDLE_CODE(kAudio_MemFullError);

		HANDLE_CODE(kAudioConverterErr_FormatNotSupported);
		HANDLE_CODE(kAudioConverterErr_OperationNotSupported);
		HANDLE_CODE(kAudioConverterErr_PropertyNotSupported);
		HANDLE_CODE(kAudioConverterErr_InvalidInputSize);
		HANDLE_CODE(kAudioConverterErr_InvalidOutputSize);
		HANDLE_CODE(kAudioConverterErr_UnspecifiedError);
		HANDLE_CODE(kAudioConverterErr_BadPropertySizeError);
		HANDLE_CODE(kAudioConverterErr_RequiresPacketDescriptionsError);
		HANDLE_CODE(kAudioConverterErr_InputSampleRateOutOfRange);
		HANDLE_CODE(kAudioConverterErr_OutputSampleRateOutOfRange);
#undef HANDLE_CODE

	default:
		break;
	}

	return NULL;
}

static DStr osstatus_to_dstr(OSStatus code)
{
	DStr result;

#ifndef _WIN32
	cf_ptr<CFErrorRef> err{CFErrorCreate(kCFAllocatorDefault, kCFErrorDomainOSStatus, code, NULL)};
	cf_ptr<CFStringRef> str{CFErrorCopyDescription(err.get())};

	if (cfstr_copy_dstr(str.get(), kCFStringEncodingUTF8, result))
		return result;
#endif

	const char *code_str = code_to_str(code);
	dstr_printf(result, "%s%s%d%s", code_str ? code_str : "", code_str ? " (" : "", static_cast<int>(code),
		    code_str ? ")" : "");
	return result;
}

static void log_osstatus(int log_level, ca_encoder *ca, const char *context, OSStatus code)
{
	DStr str = osstatus_to_dstr(code);
	CA_LOG(log_level, "Error in %s: %s", context, str->array);
}

static const char *format_id_to_str(UInt32 format_id)
{
#define FORMAT_TO_STR(x) \
	case x:          \
		return #x
	switch (format_id) {
		FORMAT_TO_STR(kAudioFormatLinearPCM);
		FORMAT_TO_STR(kAudioFormatAC3);
		FORMAT_TO_STR(kAudioFormat60958AC3);
		FORMAT_TO_STR(kAudioFormatAppleIMA4);
		FORMAT_TO_STR(kAudioFormatMPEG4AAC);
		FORMAT_TO_STR(kAudioFormatMPEG4CELP);
		FORMAT_TO_STR(kAudioFormatMPEG4HVXC);
		FORMAT_TO_STR(kAudioFormatMPEG4TwinVQ);
		FORMAT_TO_STR(kAudioFormatMACE3);
		FORMAT_TO_STR(kAudioFormatMACE6);
		FORMAT_TO_STR(kAudioFormatULaw);
		FORMAT_TO_STR(kAudioFormatALaw);
		FORMAT_TO_STR(kAudioFormatQDesign);
		FORMAT_TO_STR(kAudioFormatQDesign2);
		FORMAT_TO_STR(kAudioFormatQUALCOMM);
		FORMAT_TO_STR(kAudioFormatMPEGLayer1);
		FORMAT_TO_STR(kAudioFormatMPEGLayer2);
		FORMAT_TO_STR(kAudioFormatMPEGLayer3);
		FORMAT_TO_STR(kAudioFormatTimeCode);
		FORMAT_TO_STR(kAudioFormatMIDIStream);
		FORMAT_TO_STR(kAudioFormatParameterValueStream);
		FORMAT_TO_STR(kAudioFormatAppleLossless);
		FORMAT_TO_STR(kAudioFormatMPEG4AAC_HE);
		FORMAT_TO_STR(kAudioFormatMPEG4AAC_LD);
		FORMAT_TO_STR(kAudioFormatMPEG4AAC_ELD);
		FORMAT_TO_STR(kAudioFormatMPEG4AAC_ELD_SBR);
		FORMAT_TO_STR(kAudioFormatMPEG4AAC_HE_V2);
		FORMAT_TO_STR(kAudioFormatMPEG4AAC_Spatial);
		FORMAT_TO_STR(kAudioFormatAMR);
		FORMAT_TO_STR(kAudioFormatAudible);
		FORMAT_TO_STR(kAudioFormatiLBC);
		FORMAT_TO_STR(kAudioFormatDVIIntelIMA);
		FORMAT_TO_STR(kAudioFormatMicrosoftGSM);
		FORMAT_TO_STR(kAudioFormatAES3);
	}
#undef FORMAT_TO_STR

	return "Unknown format";
}

static void aac_destroy(ca_encoder *ca)
{
	delete ca;
}

template<typename Func>
static bool query_converter_property_raw(DStr &log, AudioFormatPropertyID property, const char *get_property_info,
					 const char *get_property, AudioConverterRef converter, Func &&func)
{
	UInt32 size = 0;
	OSStatus code = AudioConverterGetPropertyInfo(converter, property, &size, nullptr);
	if (code) {
		log_to_dstr(log, "%s: %s\n", get_property_info, osstatus_to_dstr(code)->array);
		return false;
	}

	if (!size) {
		log_to_dstr(log, "%s returned 0 size\n", get_property_info);
		return false;
	}

	vector<uint8_t> buffer;

	try {
		buffer.resize(size);
	} catch (...) {
		log_to_dstr(log, "Failed to allocate %u bytes for %s\n", static_cast<uint32_t>(size), get_property);
		return false;
	}

	code = AudioConverterGetProperty(converter, property, &size, buffer.data());
	if (code) {
		log_to_dstr(log, "%s: %s\n", get_property, osstatus_to_dstr(code)->array);
		return false;
	}

	func(size, static_cast<void *>(buffer.data()));

	return true;
}

#define EXPAND_CONVERTER_NAMES(x) x, "AudioConverterGetPropertyInfo(" #x ")", "AudioConverterGetProperty(" #x ")"

template<typename Func> static bool enumerate_bitrates(DStr &log, AudioConverterRef converter, Func &&func)
{
	auto helper = [&](UInt32 size, void *data) {
		auto range = static_cast<AudioValueRange *>(data);
		size_t num_ranges = size / sizeof(AudioValueRange);
		for (size_t i = 0; i < num_ranges; i++)
			func(static_cast<UInt32>(range[i].mMinimum), static_cast<UInt32>(range[i].mMaximum));
	};

	return query_converter_property_raw(log, EXPAND_CONVERTER_NAMES(kAudioConverterApplicableEncodeBitRates),
					    converter, helper);
}

static bool bitrate_valid(DStr &log, ca_encoder *ca, AudioConverterRef converter, UInt32 bitrate)
{
	bool valid = false;

	auto helper = [&](UInt32 min_, UInt32 max_) {
		if (min_ <= bitrate && bitrate <= max_)
			valid = true;
	};

	enumerate_bitrates(log, converter, helper);

	return valid;
}

static bool create_encoder(DStr &log, ca_encoder *ca, AudioStreamBasicDescription *in, AudioStreamBasicDescription *out,
			   UInt32 format_id, UInt32 bitrate, UInt32 samplerate, UInt32 rate_control)
{
#define STATUS_CHECK(c)                                                             \
	code = c;                                                                   \
	if (code) {                                                                 \
		log_to_dstr(log, #c " returned %s", osstatus_to_dstr(code)->array); \
		return false;                                                       \
	}

	Float64 srate = samplerate ? (Float64)samplerate : (Float64)ca->samples_per_second;

	auto out_ =
		asbd_builder().sample_rate(srate).channels_per_frame((UInt32)ca->channels).format_id(format_id).asbd;

	UInt32 size = sizeof(*out);
	OSStatus code;
	STATUS_CHECK(AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, NULL, &size, &out_));

	*out = out_;

	STATUS_CHECK(AudioConverterNew(in, out, &ca->converter))

	STATUS_CHECK(AudioConverterSetProperty(ca->converter, kAudioCodecPropertyBitRateControlMode,
					       sizeof(rate_control), &rate_control));

	if (!bitrate_valid(log, ca, ca->converter, bitrate)) {
		log_to_dstr(log,
			    "Encoder does not support bitrate %u "
			    "for format %s (0x%x)\n",
			    (uint32_t)bitrate, format_id_to_str(format_id), (uint32_t)format_id);
		return false;
	}

	ca->format_id = format_id;

	return true;
#undef STATUS_CHECK
}

static const initializer_list<UInt32> aac_formats = {
	kAudioFormatMPEG4AAC_HE_V2,
	kAudioFormatMPEG4AAC_HE,
	kAudioFormatMPEG4AAC,
};

static const initializer_list<UInt32> aac_lc_formats = {
	kAudioFormatMPEG4AAC,
};

static ca_encoder *aac_create(const struct encoder_settings *settings)
{
#define STATUS_CHECK(c)                                      \
	code = c;                                            \
	if (code) {                                          \
		log_osstatus(LOG_ERROR, ca.get(), #c, code); \
		return nullptr;                              \
	}

	UInt32 bitrate = settings->bitrate;
	if (!bitrate) {
		CA_LOG(LOG_ERROR, "Invalid bitrate specified");
		return NULL;
	}

	unique_ptr<ca_encoder> ca;

	try {
		ca.reset(new ca_encoder());
	} catch (...) {
		CA_LOG(LOG_ERROR, "Could not allocate encoder");
		return nullptr;
	}

	ca->channels = settings->channels;
	ca->samples_per_second = settings->samplerate_in;

	const uint32_t bytes_per_frame = sizeof(float) * settings->channels;
	const uint32_t bits_per_channel = sizeof(float) * 8;

	auto in = asbd_builder()
			  .sample_rate((Float64)ca->samples_per_second)
			  .channels_per_frame((UInt32)ca->channels)
			  .bytes_per_frame(bytes_per_frame)
			  .frames_per_packet(1)
			  .bytes_per_packet(bytes_per_frame)
			  .bits_per_channel(bits_per_channel)
			  .format_id(kAudioFormatLinearPCM)
			  .format_flags(kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked |
					kAudioFormatFlagIsFloat | 0)
			  .asbd;

	AudioStreamBasicDescription out;

	UInt32 rate_control = kAudioCodecBitRateControlMode_Constant;

	ca->allowed_formats = &get_allowed_formats(settings);

	DStr log;

	bool encoder_created = false;
	for (UInt32 format_id : *ca->allowed_formats) {
		log_to_dstr(log, "Trying format %s (0x%x)\n", format_id_to_str(format_id), (uint32_t)format_id);

		if (!create_encoder(log, ca.get(), &in, &out, format_id, bitrate, settings->samplerate_out,
				    rate_control))
			continue;

		encoder_created = true;
		break;
	}

	if (!encoder_created) {
		CA_CO_DLOG(LOG_ERROR,
			   "Could not create encoder for "
			   "selected format%s",
			   ca->allowed_formats->size() == 1 ? "" : "s");
		return nullptr;
	}

	if (log->len)
		CA_CO_DLOG_(LOG_DEBUG, "Encoder created");

	OSStatus code;
	UInt32 converter_quality = kAudioConverterQuality_Max;
	STATUS_CHECK(AudioConverterSetProperty(ca->converter, kAudioConverterCodecQuality, sizeof(converter_quality),
					       &converter_quality));

	STATUS_CHECK(AudioConverterSetProperty(ca->converter, kAudioConverterEncodeBitRate, sizeof(bitrate), &bitrate));

	UInt32 size = sizeof(in);
	STATUS_CHECK(
		AudioConverterGetProperty(ca->converter, kAudioConverterCurrentInputStreamDescription, &size, &in));

	size = sizeof(out);
	STATUS_CHECK(
		AudioConverterGetProperty(ca->converter, kAudioConverterCurrentOutputStreamDescription, &size, &out));

	AudioConverterPrimeInfo primeInfo;
	size = sizeof(primeInfo);
	STATUS_CHECK(AudioConverterGetProperty(ca->converter, kAudioConverterPrimeInfo, &size, &primeInfo));

	/*
	 * Fix channel map differences between CoreAudio AAC, FFmpeg, Wav
	 * New channel mappings below assume 2.1, 4.0, 4.1, 5.1, 7.1 resp.
	 */

	if (ca->channels == 3) {
		SInt32 channelMap3[3] = {2, 0, 1};
		AudioConverterSetProperty(ca->converter, kAudioConverterChannelMap, sizeof(channelMap3), channelMap3);
	}
	else if (ca->channels == 4) {
		/*
		 * For four channels coreaudio encoder has default channel "quad"
		 * instead of 4.0. So explicitly set channel layout to
		 * kAudioChannelLayoutTag_MPEG_4_0_B = (116L << 16) | 4.
		 */
		AudioChannelLayout inAcl = {0};
		inAcl.mChannelLayoutTag = (116L << 16) | 4;
		AudioConverterSetProperty(ca->converter, kAudioConverterInputChannelLayout, sizeof(inAcl), &inAcl);
		AudioConverterSetProperty(ca->converter, kAudioConverterOutputChannelLayout, sizeof(inAcl), &inAcl);
		SInt32 channelMap4[4] = {2, 0, 1, 3};
		AudioConverterSetProperty(ca->converter, kAudioConverterChannelMap, sizeof(channelMap4), channelMap4);
	}
	else if (ca->channels == 5) {
		SInt32 channelMap5[5] = {2, 0, 1, 3, 4};
		AudioConverterSetProperty(ca->converter, kAudioConverterChannelMap, sizeof(channelMap5), channelMap5);
	}
	else if (ca->channels == 6) {
		SInt32 channelMap6[6] = {2, 0, 1, 4, 5, 3};
		AudioConverterSetProperty(ca->converter, kAudioConverterChannelMap, sizeof(channelMap6), channelMap6);
	}
	else if (ca->channels == 8) {
		SInt32 channelMap8[8] = {2, 0, 1, 6, 7, 4, 5, 3};
		AudioConverterSetProperty(ca->converter, kAudioConverterChannelMap, sizeof(channelMap8), channelMap8);
	}

	ca->in_frame_size = in.mBytesPerFrame;
	size_t in_packets = out.mFramesPerPacket / in.mFramesPerPacket;
	ca->in_bytes_required = in_packets * ca->in_frame_size;

	ca->out_frames_per_packet = out.mFramesPerPacket;
	ca->priming_samples = primeInfo.leadingFrames;

	ca->output_buffer_size = out.mBytesPerPacket;

	if (out.mBytesPerPacket == 0) {
		UInt32 max_packet_size = 0;
		size = sizeof(max_packet_size);

		code = AudioConverterGetProperty(ca->converter, kAudioConverterPropertyMaximumOutputPacketSize, &size,
						 &max_packet_size);
		if (code) {
			log_osstatus(LOG_WARNING, ca.get(), "AudioConverterGetProperty(PacketSz)", code);
			ca->output_buffer_size = 32768;
		}
		else {
			ca->output_buffer_size = max_packet_size;
		}
	}

	try {
		ca->output_buffer.resize(ca->output_buffer_size);
	} catch (...) {
		CA_LOG(LOG_ERROR, "Failed to allocate output buffer");
		return nullptr;
	}

	const char *format_name = out.mFormatID == kAudioFormatMPEG4AAC_HE_V2 ? "HE-AAC v2"
				  : out.mFormatID == kAudioFormatMPEG4AAC_HE  ? "HE-AAC"
									      : "AAC";
	CA_LOG(LOG_INFO,
	       "settings:\n"
	       "\tmode:          %s\n"
	       "\tbitrate:       %u bps\n"
	       "\tsample rate:   %llu\n"
	       "\tcbr:           %s\n"
	       "\toutput buffer: %lu",
	       format_name, (unsigned int)bitrate, ca->samples_per_second,
	       rate_control == kAudioCodecBitRateControlMode_Constant ? "on" : "off",
	       (unsigned long)ca->output_buffer_size);

	return ca.release();
#undef STATUS_CHECK
}

static OSStatus complex_input_data_proc(AudioConverterRef inAudioConverter, UInt32 *ioNumberDataPackets,
					AudioBufferList *ioData,
					AudioStreamPacketDescription **outDataPacketDescription, void *inUserData)
{
	UNUSED_PARAMETER(inAudioConverter);
	UNUSED_PARAMETER(outDataPacketDescription);

	ca_encoder *ca = static_cast<ca_encoder *>(inUserData);

	if (ca->input_buffer.size() < ca->in_bytes_required) {
		*ioNumberDataPackets = 0;
		ioData->mBuffers[0].mData = NULL;
		return 1;
	}

	auto start = begin(ca->input_buffer);
	auto stop = begin(ca->input_buffer) + ca->in_bytes_required;
	ca->encode_buffer.assign(start, stop);
	ca->input_buffer.erase(start, stop);

	*ioNumberDataPackets = (UInt32)(ca->in_bytes_required / ca->in_frame_size);
	ioData->mNumberBuffers = 1;

	ioData->mBuffers[0].mData = ca->encode_buffer.data();
	ioData->mBuffers[0].mNumberChannels = (UInt32)ca->channels;
	ioData->mBuffers[0].mDataByteSize = (UInt32)ca->in_bytes_required;

	return 0;
}

static bool aac_encode(ca_encoder *ca, const struct encoder_data_header *frame, uint8_t *frame_data,
		       struct encoder_data_header *packet, uint8_t **packet_data)
{
	ca->input_buffer.insert(end(ca->input_buffer), frame_data, frame_data + frame->size);

	if (ca->input_buffer.size() < ca->in_bytes_required)
		return true;

	UInt32 packets = 1;

	AudioBufferList buffer_list = {0};
	buffer_list.mNumberBuffers = 1;
	buffer_list.mBuffers[0].mNumberChannels = (UInt32)ca->channels;
	buffer_list.mBuffers[0].mDataByteSize = (UInt32)ca->output_buffer_size;
	buffer_list.mBuffers[0].mData = ca->output_buffer.data();

	AudioStreamPacketDescription out_desc = {0};

	OSStatus code = AudioConverterFillComplexBuffer(ca->converter, complex_input_data_proc, ca, &packets,
							&buffer_list, &out_desc);
	if (code && code != 1) {
		log_osstatus(LOG_ERROR, ca, "AudioConverterFillComplexBuffer", code);
		return false;
	}

	if (packets <= 0)
		return true;

	packet->pts = ca->total_samples - ca->priming_samples;
	packet->size = out_desc.mDataByteSize;
	*packet_data = (uint8_t *)buffer_list.mBuffers[0].mData + out_desc.mStartOffset;

	ca->total_samples += ca->in_bytes_required / ca->in_frame_size;

	return true;
}

/* The following code was extracted from encca_aac.c in HandBrake's libhb */
#define MP4ESDescrTag 0x03
#define MP4DecConfigDescrTag 0x04
#define MP4DecSpecificDescrTag 0x05

// based off of mov_mp4_read_descr_len from mov.c in ffmpeg's libavformat
static int read_descr_len(uint8_t **buffer)
{
	int len = 0;
	int count = 4;
	while (count--) {
		int c = *(*buffer)++;
		len = (len << 7) | (c & 0x7f);
		if (!(c & 0x80))
			break;
	}
	return len;
}

// based off of mov_mp4_read_descr from mov.c in ffmpeg's libavformat
static int read_descr(uint8_t **buffer, int *tag)
{
	*tag = *(*buffer)++;
	return read_descr_len(buffer);
}

// based off of mov_read_esds from mov.c in ffmpeg's libavformat
static void read_esds_desc_ext(uint8_t *desc_ext, vector<uint8_t> &buffer, bool version_flags)
{
	uint8_t *esds = desc_ext;
	int tag, len;

	if (version_flags)
		esds += 4; // version + flags

	read_descr(&esds, &tag);
	esds += 2; // ID
	if (tag == MP4ESDescrTag)
		esds++; // priority

	read_descr(&esds, &tag);
	if (tag == MP4DecConfigDescrTag) {
		esds++;    // object type id
		esds++;    // stream type
		esds += 3; // buffer size db
		esds += 4; // max bitrate
		esds += 4; // average bitrate

		len = read_descr(&esds, &tag);
		if (tag == MP4DecSpecificDescrTag)
			try {
				buffer.assign(esds, esds + len);
			} catch (...) {
				//leave buffer empty
			}
	}
}
/* extracted code ends here */

static void query_extra_data(ca_encoder *ca)
{
	UInt32 size = 0;

	OSStatus code;
	code = AudioConverterGetPropertyInfo(ca->converter, kAudioConverterCompressionMagicCookie, &size, NULL);
	if (code) {
		log_osstatus(LOG_ERROR, ca, "AudioConverterGetPropertyInfo(magic_cookie)", code);
		return;
	}

	if (!size) {
		CA_LOG(LOG_WARNING, "Got 0 data size info for magic_cookie");
		return;
	}

	vector<uint8_t> extra_data;

	try {
		extra_data.resize(size);
	} catch (...) {
		CA_LOG(LOG_WARNING, "Could not allocate extra data buffer");
		return;
	}

	code = AudioConverterGetProperty(ca->converter, kAudioConverterCompressionMagicCookie, &size,
					 extra_data.data());
	if (code) {
		log_osstatus(LOG_ERROR, ca, "AudioConverterGetProperty(magic_cookie)", code);
		return;
	}

	if (!size) {
		CA_LOG(LOG_WARNING, "Got 0 data size for magic_cookie");
		return;
	}

	read_esds_desc_ext(extra_data.data(), ca->extra_data, false);
}

static asbd_builder fill_common_asbd_fields(asbd_builder builder, bool in = false, UInt32 channels = 2)
{
	UInt32 bytes_per_frame = sizeof(float) * channels;
	UInt32 bits_per_channel = bytes_per_frame / channels * 8;

	builder.channels_per_frame(channels);

	if (in) {
		builder.bytes_per_frame(bytes_per_frame)
			.frames_per_packet(1)
			.bytes_per_packet(1 * bytes_per_frame)
			.bits_per_channel(bits_per_channel);
	}

	return builder;
}

static AudioStreamBasicDescription get_default_in_asbd()
{
	return fill_common_asbd_fields(asbd_builder(), true)
		.sample_rate(44100)
		.format_id(kAudioFormatLinearPCM)
		.format_flags(kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked | kAudioFormatFlagIsFloat | 0)
		.asbd;
}

static asbd_builder get_default_out_asbd_builder(UInt32 channels)
{
	return fill_common_asbd_fields(asbd_builder(), false, channels).sample_rate(44100);
}

static cf_ptr<AudioConverterRef> get_converter(DStr &log, AudioStreamBasicDescription out,
					       AudioStreamBasicDescription in = get_default_in_asbd())
{
	UInt32 size = sizeof(out);
	OSStatus code;

#define STATUS_CHECK(x)                                                          \
	code = x;                                                                \
	if (code) {                                                              \
		log_to_dstr(log, "%s: %s\n", #x, osstatus_to_dstr(code)->array); \
		return nullptr;                                                  \
	}

	STATUS_CHECK(AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, NULL, &size, &out));

	AudioConverterRef converter;
	STATUS_CHECK(AudioConverterNew(&in, &out, &converter));

	return cf_ptr<AudioConverterRef>{converter};
#undef STATUS_CHECK
}

// TODO: Do we need this function?
static bool find_best_match(DStr &log, ca_encoder *ca, UInt32 bitrate, UInt32 &best_match)
{
	UInt32 actual_bitrate = bitrate;
	bool found_match = false;

	auto handle_bitrate = [&](UInt32 candidate) {
		if (abs(static_cast<intmax_t>(actual_bitrate - candidate)) <
		    abs(static_cast<intmax_t>(actual_bitrate - best_match))) {
			log_to_dstr(log, "Found new best match %u\n", static_cast<uint32_t>(candidate));

			found_match = true;
			best_match = candidate;
		}
	};

	auto helper = [&](UInt32 min_, UInt32 max_) {
		handle_bitrate(min_);

		if (min_ == max_)
			return;

		log_to_dstr(log, "Got actual bit rate range: %u<->%u\n", static_cast<uint32_t>(min_),
			    static_cast<uint32_t>(max_));

		handle_bitrate(max_);
	};

	for (UInt32 format_id : aac_formats) {
		log_to_dstr(log, "Trying %s (0x%x)\n", format_id_to_str(format_id), format_id);

		auto out = get_default_out_asbd_builder(2).format_id(format_id).asbd;

		auto converter = get_converter(log, out);

		if (converter)
			enumerate_bitrates(log, converter.get(), helper);
		else
			log_to_dstr(log, "Could not get converter\n");
	}

	return found_match;
}

template<typename Func>
static bool query_property_raw(DStr &log, AudioFormatPropertyID property, const char *get_property_info,
			       const char *get_property, AudioStreamBasicDescription &desc, Func &&func)
{
	UInt32 size = 0;
	OSStatus code = AudioFormatGetPropertyInfo(property, sizeof(AudioStreamBasicDescription), &desc, &size);
	if (code) {
		log_to_dstr(log, "%s: %s\n", get_property_info, osstatus_to_dstr(code)->array);
		return false;
	}

	if (!size) {
		log_to_dstr(log, "%s returned 0 size\n", get_property_info);
		return false;
	}

	vector<uint8_t> buffer;

	try {
		buffer.resize(size);
	} catch (...) {
		log_to_dstr(log, "Failed to allocate %u bytes for %s\n", static_cast<uint32_t>(size), get_property);
		return false;
	}

	code = AudioFormatGetProperty(property, sizeof(AudioStreamBasicDescription), &desc, &size, buffer.data());
	if (code) {
		log_to_dstr(log, "%s: %s\n", get_property, osstatus_to_dstr(code)->array);
		return false;
	}

	func(size, static_cast<void *>(buffer.data()));

	return true;
}

#define EXPAND_PROPERTY_NAMES(x) x, "AudioFormatGetPropertyInfo(" #x ")", "AudioFormatGetProperty(" #x ")"

template<typename Func> static bool enumerate_samplerates(DStr &log, AudioStreamBasicDescription &desc, Func &&func)
{
	auto helper = [&](UInt32 size, void *data) {
		auto range = static_cast<AudioValueRange *>(data);
		size_t num_ranges = size / sizeof(AudioValueRange);
		for (size_t i = 0; i < num_ranges; i++)
			func(range[i]);
	};

	return query_property_raw(log, EXPAND_PROPERTY_NAMES(kAudioFormatProperty_AvailableEncodeSampleRates), desc,
				  helper);
}

static vector<UInt32> get_samplerates(DStr &log, const initializer_list<UInt32> &allowed_formats)
{
	vector<UInt32> samplerates;

	auto handle_samplerate = [&](UInt32 rate) {
		if (find(begin(samplerates), end(samplerates), rate) == end(samplerates)) {
			log_to_dstr(log, "Adding sample rate %u\n", static_cast<uint32_t>(rate));
			samplerates.push_back(rate);
		}
		else {
			log_to_dstr(log, "Sample rate %u already added\n", static_cast<uint32_t>(rate));
		}
	};

	auto helper = [&](const AudioValueRange &range) {
		auto min_ = static_cast<UInt32>(range.mMinimum);
		auto max_ = static_cast<UInt32>(range.mMaximum);

		handle_samplerate(min_);

		if (min_ == max_)
			return;

		log_to_dstr(log, "Got actual sample rate range: %u<->%u\n", static_cast<uint32_t>(min_),
			    static_cast<uint32_t>(max_));

		handle_samplerate(max_);
	};

	for (UInt32 format : allowed_formats) {
		log_to_dstr(log, "Trying %s (0x%x)\n", format_id_to_str(format), static_cast<uint32_t>(format));

		auto asbd = asbd_builder().format_id(format).asbd;

		enumerate_samplerates(log, asbd, helper);
	}

	return samplerates;
}

static vector<UInt32> get_bitrates(DStr &log, const struct encoder_settings *settings)
{
	vector<UInt32> bitrates;
	int channels = settings ? settings->channels : 2;
	float samplerate = settings ? settings->samplerate_out : 44100;

	auto handle_bitrate = [&](UInt32 bitrate) {
		if (find(begin(bitrates), end(bitrates), bitrate) == end(bitrates)) {
			log_to_dstr(log, "Adding bitrate %u\n", static_cast<uint32_t>(bitrate));
			bitrates.push_back(bitrate);
		}
		else {
			log_to_dstr(log, "Bitrate %u already added\n", static_cast<uint32_t>(bitrate));
		}
	};

	auto helper = [&](UInt32 min_, UInt32 max_) {
		handle_bitrate(min_);

		if (min_ == max_)
			return;

		log_to_dstr(log, "Got actual bitrate range: %u<->%u\n", static_cast<uint32_t>(min_),
			    static_cast<uint32_t>(max_));

		handle_bitrate(max_);
	};

	for (UInt32 format_id : get_allowed_formats(settings)) {
		log_to_dstr(log, "Trying %s (0x%x) at %g Hz\n", format_id_to_str(format_id),
			    static_cast<uint32_t>(format_id), samplerate);

		auto out = get_default_out_asbd_builder(channels).format_id(format_id).sample_rate(samplerate).asbd;

		auto converter = get_converter(log, out);

		if (converter)
			enumerate_bitrates(log, converter.get(), helper);
	}

	return bitrates;
}

static const initializer_list<UInt32> &get_allowed_formats(const struct encoder_settings *settings)
{
	if (!settings)
		return aac_formats;

	if ((settings->flags & ENCODER_FLAG_ALLOW_HE_AAC) != 0 && settings->channels != 3)
		return aac_formats;
	else
		return aac_lc_formats;
}

static void list_properties(struct encoder_settings *settings)
{
	DStr log;
	auto samplerates = get_samplerates(log, get_allowed_formats(settings));
	printf("\"samplerates\": [");
	for (size_t i = 0; i < samplerates.size(); i++)
		printf("%s%u", i ? ", " : "", samplerates[i]);
	printf("],\n");

	auto bitrates = get_bitrates(log, settings);
	printf("\"bitrates\": [");
	for (size_t i = 0; i < bitrates.size(); i++)
		printf("%s%u", i ? ", " : "", bitrates[i]);
	printf("]\n");
}

static bool write_header_data(const encoder_data_header &header, const uint8_t *data)
{
	if (fwrite(&header, sizeof(header), 1, stdout) != 1) {
		CA_LOG(LOG_ERROR, "Failed to write packet header to stdout");
		return false;
	}

	if (header.size && fwrite(data, header.size, 1, stdout) != 1) {
		CA_LOG(LOG_ERROR, "Failed to write packet header to stdout");
		return false;
	}

	fflush(stdout);

	return true;
}

static inline int main_internal(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		char *ai = argv[i];
		if (ai[0] == '-') {
			char c;
			while (c = *++ai) {
				switch (c) {
				case 'l':
					list_properties(nullptr);
					return 0;
				default:
					fprintf(stderr, "Error: Unknown option '%c'\n", c);
					return 1;
				}
			}
		}
		else {
			fprintf(stderr, "Error: Unknown argument '%s'\n", ai);
			return 1;
		}
	}

#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif

	struct encoder_settings settings;

	if (fread(&settings, sizeof(settings), 1, stdin) != 1) {
		CA_LOG(LOG_ERROR, "Failed to read settings from stdin");
		return 1;
	}

	if (settings.struct_size != sizeof(settings)) {
		CA_LOG(LOG_ERROR, "struct_size mismatch, got %u, expected %zu", settings.struct_size, sizeof(settings));
		return 1;
	}

	if (settings.proc_version != ENCODER_PROC_VERSION) {
		CA_LOG(LOG_ERROR, "Protocol version mismatch, got %u, expected %u", settings.proc_version,
		       ENCODER_PROC_VERSION);
		return 1;
	}

	struct ca_encoder *ca = aac_create(&settings);
	if (!ca) {
		CA_LOG(LOG_ERROR, "Failed to create the instance");
		return 1;
	}

	settings.out_frames_per_packet = (uint32_t)ca->out_frames_per_packet;

	if (fwrite(&settings, sizeof(settings), 1, stdout) != 1) {
		CA_LOG(LOG_ERROR, "Failed to write settings to stdout");
		return 1;
	}

	fflush(stdout);

	encoder_data_header header;
	vector<uint8_t> payload;
	for (header.flags = 0; (header.flags & ENCODER_FLAG_EXIT) == 0;) {
		if (fread(&header, sizeof(header), 1, stdin) != 1)
			break;

		payload.resize(header.size);
		if (header.size && fread(payload.data(), header.size, 1, stdin) != 1) {
			CA_LOG(LOG_ERROR, "Failed to read payload from stdin");
			break;
		}

		if (header.flags & ENCODER_FLAG_QUERY_ENCODE) {
			encoder_data_header packet_header = {
				.size = 0,
				.flags = ENCODER_FLAG_QUERY_ENCODE,
			};
			uint8_t *packet_data = nullptr;
			bool encode_res = aac_encode(ca, &header, payload.data(), &packet_header, &packet_data);

			if (!write_header_data(packet_header, packet_data))
				break;
		}

		if (header.flags & ENCODER_FLAG_QUERY_EXTRA_DATA) {
			if (!ca->extra_data.size())
				query_extra_data(ca);
			encoder_data_header header = {
				.size = ca->extra_data.size(),
				.flags = ENCODER_FLAG_QUERY_EXTRA_DATA,
			};
			if (!write_header_data(header, ca->extra_data.data()))
				break;
		}
	}

	aac_destroy(ca);

	return 0;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
	if (!load_core_audio()) {
		CA_LOG(LOG_WARNING, "CoreAudio AAC encoder not installed on "
				    "the system or couldn't be loaded");
		return 1;
	}

	CA_LOG(LOG_INFO, "Adding CoreAudio AAC encoder");
#endif

	int ret = main_internal(argc, argv);

#ifdef _WIN32
	unload_core_audio();
#endif

	return ret;
}
