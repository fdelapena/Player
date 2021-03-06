/*
 * This file is part of EasyRPG Player.
 *
 * EasyRPG Player is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * EasyRPG Player is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with EasyRPG Player. If not, see <http://www.gnu.org/licenses/>.
 */

// Headers
#include <cassert>
#include <cstring>
#include "audio_decoder.h"
#include "audio_midi.h"
#include "audio_resampler.h"
#include "output.h"
#include "system.h"
#include "utils.h"

#include "decoder_mpg123.h"
#include "decoder_oggvorbis.h"
#include "decoder_opus.h"
#include "decoder_libsndfile.h"
#include "decoder_wav.h"
#include "decoder_xmp.h"

void AudioDecoder::Pause() {
	paused = true;
}

void AudioDecoder::Resume() {
	paused = false;
}

int AudioDecoder::Decode(uint8_t* buffer, int length) {
	return Decode(buffer, length, 0);
}

int AudioDecoder::Decode(uint8_t* buffer, int length, int recursion_depth) {
	if (paused) {
		memset(buffer, '\0', length);
		return length;
	}

	int res = FillBuffer(buffer, length);

	if (res < 0) {
		memset(buffer, '\0', length);
	} else if (res < length) {
		memset(&buffer[res], '\0', length - res);
	}

	if (IsFinished() && looping && recursion_depth < 10) {
		++loop_count;
		Rewind();
		if (length - res > 0) {
			int res2 = Decode(&buffer[res], length - res, ++recursion_depth);
			if (res2 <= 0) {
				return res;
			}
			return res + res2;
		}
	}

	if (recursion_depth == 10 && loop_count < 50) {
		// Only report this a few times in the hope that this is only a temporary problem and to prevent log spamming
		Output::Debug("Audio Decoder: Recursion depth exceeded. Probably stream error.");
	}

	return res;
}

std::vector<uint8_t> AudioDecoder::DecodeAll() {
	const int buffer_size = 8192;

	std::vector<uint8_t> buffer;
	buffer.resize(buffer_size);

	while (!IsFinished()) {
		int read = Decode(buffer.data() + buffer.size() - buffer_size, buffer_size);
		if (read < buffer_size) {
			buffer.resize(buffer.size() - (buffer_size - read));
			break;
		}

		buffer.resize(buffer.size() + buffer_size);
	}

	return buffer;
}

class WMAUnsupportedFormatDecoder : public AudioDecoder {
public:
	WMAUnsupportedFormatDecoder() {
		error_message = std::string("WMA audio files are not supported. Reinstall the\n") +
			"game and don't convert them when asked by Windows!\n";
	}
	bool Open(Filesystem_Stream::InputStream) override { return false; }
	bool IsFinished() const override { return true; }
	void GetFormat(int&, Format&, int&) const override {}
	bool Seek(std::streamoff, std::ios_base::seekdir) override { return false; }
private:
	int FillBuffer(uint8_t*, int) override { return -1; };
};
const char wma_magic[] = { (char)0x30, (char)0x26, (char)0xB2, (char)0x75 };

std::unique_ptr<AudioDecoder> AudioDecoder::Create(Filesystem_Stream::InputStream& stream, bool resample) {
	char magic[4] = { 0 };
	if (!stream.ReadIntoObj(magic)) {
		return nullptr;
	}
	stream.seekg(0, std::ios::beg);

	auto add_resampler = [resample](std::unique_ptr<AudioDecoder> dec) -> std::unique_ptr<AudioDecoder> {
#ifdef USE_AUDIO_RESAMPLER
		if (resample)
			return std::make_unique<AudioResampler>(std::move(dec));
#endif
		return dec;
	};

	// Try to use MIDI decoder, use fallback(s) if available
	if (!strncmp(magic, "MThd", 4)) {
		auto midi = MidiDecoder::Create(stream, resample);
		if (midi) {
			return midi;
		}
	}

	// Try to use internal OGG decoder
	if (!strncmp(magic, "OggS", 4)) { // OGG
#ifdef HAVE_OPUS
		stream.seekg(28, std::ios::ios_base::beg);
		if (stream.read(magic, sizeof(magic)).gcount() == 0) {
			return nullptr;
		}
		stream.seekg(0, std::ios::ios_base::beg);

		if (!strncmp(magic, "Opus", 4)) {
			return add_resampler(std::make_unique<OpusDecoder>());
		}
#endif

#if defined(HAVE_TREMOR) || defined(HAVE_OGGVORBIS)
		stream.seekg(29, std::ios::ios_base::beg);
		if (stream.read(magic, sizeof(magic)).gcount() == 0) {
			return nullptr;
		}
		stream.seekg(0, std::ios::ios_base::beg);

		if (!strncmp(magic, "vorb", 4)) {
			return add_resampler(std::make_unique<OggVorbisDecoder>());
		}
#endif
	}

#ifdef WANT_FASTWAV
	// Try to use a basic decoder for faster wav decoding if not ADPCM
	if (!strncmp(magic, "RIFF", 4)) {
		stream.seekg(20, std::ios::ios_base::beg);
		uint16_t raw_enc;
		stream.read(reinterpret_cast<char*>(&raw_enc), 2);

		Utils::SwapByteOrder(raw_enc);
		stream.seekg(0, std::ios::ios_base::beg);
		if (raw_enc == 0x01) { // Codec is normal PCM
			return add_resampler(std::make_unique<WavDecoder>());
		}
	}

#endif

	// Try to use libsndfile for common formats
	if (!strncmp(magic, "RIFF", 4) || // WAV
		!strncmp(magic, "FORM", 4) || // WAV AIFF
		!strncmp(magic, "OggS", 4) || // OGG
		!strncmp(magic, "fLaC", 4)) { // FLAC
#ifdef HAVE_LIBSNDFILE
		return add_resampler(std::make_unique<LibsndfileDecoder>());
#endif
		return nullptr;
	}

	// Inform about WMA issue
	if (!memcmp(magic, wma_magic, 4)) {
		return std::make_unique<WMAUnsupportedFormatDecoder>();
	}

	// Test for tracker modules
#ifdef HAVE_XMP
	if (XMPDecoder::IsModule(ToString(stream.GetName()))) {
		return add_resampler(std::make_unique<XMPDecoder>());
	}
#endif

	// False positive MP3s should be prevented before by checking for common headers
#ifdef HAVE_MPG123
	static bool mpg123_works = true;
	if (mpg123_works) {
		auto mp3dec = add_resampler(std::make_unique<Mpg123Decoder>());
		if (mp3dec->WasInited()) {
			if (strncmp(magic, "ID3", 3) == 0) {
				return mp3dec;
			}

			// Parsing MP3s seems to be the only reliable way to detect them
			if (Mpg123Decoder::IsMp3(stream)) {
				stream.clear();
				stream.seekg(0, std::ios_base::beg);
				return mp3dec;
			}
		} else {
			mpg123_works = false;
		}
	}
#endif

	stream.clear();
	stream.seekg(0, std::ios::ios_base::beg);
	return nullptr;
}

void AudioDecoder::SetFade(int begin, int end, int duration) {
	fade_time = 0.0;

	if (duration <= 0.0) {
		volume = end;
		return;
	}

	if (begin == end) {
		volume = end;
		return;
	}

	volume = (double)begin;
	fade_end = (double)end;
	fade_time = (double)duration;
	delta_step = (fade_end - volume) / fade_time;
}

void AudioDecoder::Update(int delta) {
	if (fade_time <= 0.0) {
		return;
	}

	fade_time -= delta;
	volume += delta * delta_step;

	volume = volume > 100.0 ? 100.0 :
		volume < 0.0 ? 0.0 :
		volume;
}

void AudioDecoder::SetVolume(int volume) {
	this->volume = (double)volume;
}

int AudioDecoder::GetVolume() const {
	return (int)volume;
}

void AudioDecoder::Rewind() {
	if (!Seek(0, std::ios_base::beg)) {
		// The libs guarantee that Rewind works
		assert(false && "Rewind");
	}
}

bool AudioDecoder::GetLooping() const {
	return looping;
}

void AudioDecoder::SetLooping(bool enable) {
	looping = enable;
}

int AudioDecoder::GetLoopCount() const {
	return loop_count;
}

bool AudioDecoder::WasInited() const {
	return true;
}

std::string AudioDecoder::GetError() const {
	return error_message;
}

std::string AudioDecoder::GetType() const {
	return music_type;
}

bool AudioDecoder::SetFormat(int, Format, int) {
	return false;
}

int AudioDecoder::GetPitch() const {
	return 0;
}

bool AudioDecoder::SetPitch(int) {
	return false;
}

std::streampos AudioDecoder::Tell() const {
	return -1;
}

int AudioDecoder::GetTicks() const {
	return 0;
}

int AudioDecoder::GetSamplesizeForFormat(AudioDecoder::Format format) {
	switch (format) {
		case Format::S8:
		case Format::U8:
			return 1;
		case Format::S16:
		case Format::U16:
			return 2;
		case Format::S32:
		case Format::U32:
		case Format::F32:
			return 4;
	}

	assert(false && "Bad format");
	return -1;
}
