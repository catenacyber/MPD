/*
 * Copyright 2020-2021 The Music Player Daemon Project
 * http://www.musicpd.org
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

#include "WasapiOutputPlugin.hxx"
#include "ForMixer.hxx"
#include "AudioClient.hxx"
#include "Device.hxx"
#include "PropertyStore.hxx"
#include "output/OutputAPI.hxx"
#include "lib/icu/Win32.hxx"
#include "mixer/MixerList.hxx"
#include "output/Error.hxx"
#include "pcm/Export.hxx"
#include "thread/Cond.hxx"
#include "thread/Mutex.hxx"
#include "thread/Name.hxx"
#include "thread/Thread.hxx"
#include "util/AllocatedString.hxx"
#include "util/ConstBuffer.hxx"
#include "util/Domain.hxx"
#include "util/RuntimeError.hxx"
#include "util/ScopeExit.hxx"
#include "util/StringBuffer.hxx"
#include "win32/Com.hxx"
#include "win32/ComPtr.hxx"
#include "win32/ComWorker.hxx"
#include "win32/HResult.hxx"
#include "win32/WinEvent.hxx"
#include "Log.hxx"
#include "config.h"

#include <boost/lockfree/spsc_queue.hpp>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <optional>
#include <variant>

#include <audioclient.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>

namespace {
static constexpr Domain wasapi_output_domain("wasapi_output");

constexpr uint32_t
GetChannelMask(const uint8_t channels) noexcept
{
	switch (channels) {
	case 1:
		return KSAUDIO_SPEAKER_MONO;
	case 2:
		return KSAUDIO_SPEAKER_STEREO;
	case 3:
		return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER;
	case 4:
		return KSAUDIO_SPEAKER_QUAD;
	case 5:
		return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
		       SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
	case 6:
		return KSAUDIO_SPEAKER_5POINT1;
	case 7:
		return KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER;
	case 8:
		return KSAUDIO_SPEAKER_7POINT1_SURROUND;
	default:
		gcc_unreachable();
	}
}

template <typename Functor>
inline bool
SafeSilenceTry(Functor &&functor) noexcept
{
	try {
		functor();
		return true;
	} catch (...) {
		return false;
	}
}

std::vector<WAVEFORMATEXTENSIBLE>
GetFormats(const AudioFormat &audio_format) noexcept
{
#ifdef ENABLE_DSD
	if (audio_format.format == SampleFormat::DSD) {
		AudioFormat dop_format = audio_format;
		PcmExport::Params params;
		params.dsd_mode = PcmExport::DsdMode::DOP;
		dop_format.sample_rate =
			params.CalcOutputSampleRate(audio_format.sample_rate);
		dop_format.format = SampleFormat::S24_P32;
		return GetFormats(dop_format);
	}
#endif
	std::vector<WAVEFORMATEXTENSIBLE> Result;
	if (audio_format.format == SampleFormat::S24_P32) {
		Result.resize(2);
		Result[0].Format.wBitsPerSample = 24;
		Result[0].Samples.wValidBitsPerSample = 24;
		Result[1].Format.wBitsPerSample = 32;
		Result[1].Samples.wValidBitsPerSample = 24;
	} else {
		Result.resize(1);
		Result[0].Format.wBitsPerSample = audio_format.GetSampleSize() * 8;
		Result[0].Samples.wValidBitsPerSample = audio_format.GetSampleSize() * 8;
	}
	const DWORD mask = GetChannelMask(audio_format.channels);
	const GUID guid = audio_format.format == SampleFormat::FLOAT
				  ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
				  : KSDATAFORMAT_SUBTYPE_PCM;
	for (auto &device_format : Result) {
		device_format.dwChannelMask = mask;
		device_format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		device_format.Format.nChannels = audio_format.channels;
		device_format.Format.nSamplesPerSec = audio_format.sample_rate;
		device_format.Format.cbSize =
			sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
		device_format.SubFormat = guid;
		device_format.Format.nBlockAlign = device_format.Format.nChannels *
						   device_format.Format.wBitsPerSample /
						   8;
		device_format.Format.nAvgBytesPerSec =
			audio_format.sample_rate * device_format.Format.nBlockAlign;
	}
	return Result;
}

#ifdef ENABLE_DSD
void
SetDSDFallback(AudioFormat &audio_format) noexcept
{
	audio_format.format = SampleFormat::FLOAT;
	audio_format.sample_rate = 384000;
}
#endif

} // namespace

class WasapiOutputThread : public Thread {
	friend class WasapiOutput;
	WinEvent event;
	WinEvent data_poped;
	IAudioClient *client;
	ComPtr<IAudioRenderClient> render_client;
	const UINT32 frame_size;
	const UINT32 buffer_size_in_frames;
	bool is_exclusive;

	enum class Status : uint32_t { FINISH, PLAY, PAUSE };
	alignas(BOOST_LOCKFREE_CACHELINE_BYTES) std::atomic<Status> status =
		Status::PAUSE;
	alignas(BOOST_LOCKFREE_CACHELINE_BYTES) struct {
		std::atomic_bool occur = false;
		std::exception_ptr ptr = nullptr;
		WinEvent thrown;
	} error;
	boost::lockfree::spsc_queue<BYTE> spsc_buffer;

public:
	WasapiOutputThread(IAudioClient *_client,
			   ComPtr<IAudioRenderClient> &&_render_client,
			   const UINT32 _frame_size, const UINT32 _buffer_size_in_frames,
			   bool _is_exclusive)
		:Thread(BIND_THIS_METHOD(Work)), client(_client),
		 render_client(std::move(_render_client)), frame_size(_frame_size),
		 buffer_size_in_frames(_buffer_size_in_frames), is_exclusive(_is_exclusive),
		 spsc_buffer(_buffer_size_in_frames * 4 * _frame_size)
	{
	}

	void Finish() noexcept { return SetStatus(Status::FINISH); }
	void Play() noexcept { return SetStatus(Status::PLAY); }
	void Pause() noexcept { return SetStatus(Status::PAUSE); }
	void WaitDataPoped() noexcept { data_poped.Wait(); }
	void CheckException() {
		if (error.occur.load()) {
			auto err = std::exchange(error.ptr, nullptr);
			error.thrown.Set();
			std::rethrow_exception(err);
		}
	}

private:
	void SetStatus(Status s) noexcept {
		status.store(s);
		event.Set();
	}
	void Work() noexcept;
};

class WasapiOutput final : public AudioOutput {
	std::atomic_flag not_interrupted = true;
	bool is_started = false;
	bool is_exclusive;
	bool enumerate_devices;
#ifdef ENABLE_DSD
	bool dop_setting;
#endif
	std::string device_config;
	std::shared_ptr<COMWorker> com_worker;
	ComPtr<IMMDeviceEnumerator> enumerator;
	ComPtr<IMMDevice> device;
	ComPtr<IAudioClient> client;
	WAVEFORMATEXTENSIBLE device_format;
	std::optional<WasapiOutputThread> thread;
	std::size_t watermark;
	std::optional<PcmExport> pcm_export;

public:
	static AudioOutput *Create(EventLoop &, const ConfigBlock &block);
	WasapiOutput(const ConfigBlock &block);

	auto GetComWorker() noexcept {
		// TODO: protect access to the shard_ptr
		return com_worker;
	}

	void Enable() override {
		com_worker = std::make_shared<COMWorker>();

		try {
			com_worker->Async([&]() { OpenDevice(); }).get();
		} catch (...) {
			com_worker.reset();
			throw;
		}
	}
	void Disable() noexcept override {
		com_worker->Async([&]() { DoDisable(); }).get();
		com_worker.reset();
	}
	void Open(AudioFormat &audio_format) override {
		com_worker->Async([&]() { DoOpen(audio_format); }).get();
	}
	void Close() noexcept override;
	std::chrono::steady_clock::duration Delay() const noexcept override;
	size_t Play(const void *chunk, size_t size) override;
	void Drain() override;
	bool Pause() override;
	void Interrupt() noexcept override;

	constexpr bool Exclusive() const { return is_exclusive; }
	constexpr size_t FrameSize() const { return device_format.Format.nBlockAlign; }
	constexpr size_t SampleRate() const {
		return device_format.Format.nSamplesPerSec;
	}

private:
	friend bool wasapi_is_exclusive(WasapiOutput &output) noexcept;
	friend IMMDevice *wasapi_output_get_device(WasapiOutput &output) noexcept;
	friend IAudioClient *wasapi_output_get_client(WasapiOutput &output) noexcept;

	void DoDisable() noexcept;
	void DoOpen(AudioFormat &audio_format);

	void OpenDevice();
	bool TryFormatExclusive(const AudioFormat &audio_format);
	void FindExclusiveFormatSupported(AudioFormat &audio_format);
	void FindSharedFormatSupported(AudioFormat &audio_format);
	void EnumerateDevices();
	ComPtr<IMMDevice> GetDevice(unsigned int index);
	ComPtr<IMMDevice> SearchDevice(std::string_view name);
};

WasapiOutput &
wasapi_output_downcast(AudioOutput &output) noexcept
{
	return static_cast<WasapiOutput &>(output);
}

bool
wasapi_is_exclusive(WasapiOutput &output) noexcept
{
	return output.is_exclusive;
}

std::shared_ptr<COMWorker>
wasapi_output_get_com_worker(WasapiOutput &output) noexcept
{
	return output.GetComWorker();
}

IMMDevice *
wasapi_output_get_device(WasapiOutput &output) noexcept
{
	return output.device.get();
}

IAudioClient *
wasapi_output_get_client(WasapiOutput &output) noexcept
{
	return output.client.get();
}

void
WasapiOutputThread::Work() noexcept
{
	SetThreadName("Wasapi Output Worker");
	FormatDebug(wasapi_output_domain, "Working thread started");
	COM com;
	while (true) {
		try {
			event.Wait();

			Status current_state = status.load();
			if (current_state == Status::FINISH) {
				FormatDebug(wasapi_output_domain,
					    "Working thread stopped");
				return;
			}

			UINT32 write_in_frames = buffer_size_in_frames;
			if (!is_exclusive) {
				UINT32 data_in_frames =
					GetCurrentPaddingFrames(*client);

				if (data_in_frames >= buffer_size_in_frames) {
					continue;
				}
				write_in_frames -= data_in_frames;
			}

			BYTE *data;
			DWORD mode = 0;

			if (HRESULT result =
				    render_client->GetBuffer(write_in_frames, &data);
			    FAILED(result)) {
				throw MakeHResultError(result, "Failed to get buffer");
			}

			AtScopeExit(&) {
				render_client->ReleaseBuffer(write_in_frames, mode);
			};

			if (current_state == Status::PLAY) {
				const UINT32 write_size = write_in_frames * frame_size;
				UINT32 new_data_size = 0;
				new_data_size = spsc_buffer.pop(data, write_size);
				std::fill_n(data + new_data_size,
					    write_size - new_data_size, 0);
				data_poped.Set();
			} else {
				mode = AUDCLNT_BUFFERFLAGS_SILENT;
				FormatDebug(wasapi_output_domain,
					    "Working thread paused");
			}
		} catch (...) {
			error.ptr = std::current_exception();
			error.occur.store(true);
			error.thrown.Wait();
		}
	}
}

AudioOutput *
WasapiOutput::Create(EventLoop &, const ConfigBlock &block)
{
	return new WasapiOutput(block);
}

WasapiOutput::WasapiOutput(const ConfigBlock &block)
	:AudioOutput(FLAG_ENABLE_DISABLE | FLAG_PAUSE),
	 is_exclusive(block.GetBlockValue("exclusive", false)),
	 enumerate_devices(block.GetBlockValue("enumerate", false)),
#ifdef ENABLE_DSD
	 dop_setting(block.GetBlockValue("dop", false)),
#endif
	 device_config(block.GetBlockValue("device", ""))
{
}

/// run inside COMWorkerThread
void
WasapiOutput::DoDisable() noexcept
{
	if (thread) {
		try {
			thread->Finish();
			thread->Join();
		} catch (std::exception &err) {
			FormatError(wasapi_output_domain, "exception while disabling: %s",
				    err.what());
		}
		thread.reset();
		client.reset();
	}
	device.reset();
	enumerator.reset();
}

/// run inside COMWorkerThread
void
WasapiOutput::DoOpen(AudioFormat &audio_format)
{
	client.reset();

	if (GetState(*device) != DEVICE_STATE_ACTIVE) {
		device.reset();
		OpenDevice();
	}

	client = Activate<IAudioClient>(*device);

	if (audio_format.channels > 8) {
		audio_format.channels = 8;
	}

#ifdef ENABLE_DSD
	if (!dop_setting && audio_format.format == SampleFormat::DSD) {
		SetDSDFallback(audio_format);
	}
#endif
	if (Exclusive()) {
		FindExclusiveFormatSupported(audio_format);
	} else {
		FindSharedFormatSupported(audio_format);
	}
	bool require_export = audio_format.format == SampleFormat::S24_P32;
#ifdef ENABLE_DSD
	require_export |= audio_format.format == SampleFormat::DSD;
#endif
	if (require_export) {
		PcmExport::Params params;
		params.dsd_mode = PcmExport::DsdMode::NONE;
#ifdef ENABLE_DSD
		if (audio_format.format == SampleFormat::DSD) {
			params.dsd_mode = PcmExport::DsdMode::DOP;
		}
#endif
		params.shift8 = false;
		params.pack24 = false;
		if (device_format.Format.wBitsPerSample == 32 &&
		    device_format.Samples.wValidBitsPerSample == 24) {
			params.shift8 = true;
		}
		if (device_format.Format.wBitsPerSample == 24) {
			params.pack24 = true;
		}
		FormatDebug(wasapi_output_domain, "Packing data: shift8=%d pack24=%d",
			    int(params.shift8), int(params.pack24));
		pcm_export.emplace();
		pcm_export->Open(audio_format.format, audio_format.channels, params);
	}

	using s = std::chrono::seconds;
	using ms = std::chrono::milliseconds;
	using ns = std::chrono::nanoseconds;
	using hundred_ns = std::chrono::duration<uint64_t, std::ratio<1, 10000000>>;

	// The unit in REFERENCE_TIME is hundred nanoseconds
	REFERENCE_TIME default_device_period, min_device_period;

	if (HRESULT result =
		    client->GetDevicePeriod(&default_device_period, &min_device_period);
	    FAILED(result)) {
		throw MakeHResultError(result, "Unable to get device period");
	}
	FormatDebug(wasapi_output_domain,
		    "Default device period: %I64u ns, Minimum device period: "
		    "%I64u ns",
		    ns(hundred_ns(default_device_period)).count(),
		    ns(hundred_ns(min_device_period)).count());

	REFERENCE_TIME buffer_duration;
	if (Exclusive()) {
		buffer_duration = default_device_period;
	} else {
		const REFERENCE_TIME align = hundred_ns(ms(50)).count();
		buffer_duration = (align / default_device_period) * default_device_period;
	}
	FormatDebug(wasapi_output_domain, "Buffer duration: %I64u ns",
		    size_t(ns(hundred_ns(buffer_duration)).count()));

	if (Exclusive()) {
		if (HRESULT result = client->Initialize(
			    AUDCLNT_SHAREMODE_EXCLUSIVE,
			    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, buffer_duration,
			    buffer_duration,
			    reinterpret_cast<WAVEFORMATEX *>(&device_format), nullptr);
		    FAILED(result)) {
			if (result == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
				// https://docs.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize
				UINT32 buffer_size_in_frames =
					GetBufferSizeInFrames(*client);
				buffer_duration =
					std::ceil(double(buffer_size_in_frames *
							 hundred_ns(s(1)).count()) /
						  SampleRate());
				FormatDebug(
					wasapi_output_domain,
					"Aligned buffer duration: %I64u ns",
					size_t(ns(hundred_ns(buffer_duration)).count()));
				client.reset();
				client = Activate<IAudioClient>(*device);
				result = client->Initialize(
					AUDCLNT_SHAREMODE_EXCLUSIVE,
					AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
					buffer_duration, buffer_duration,
					reinterpret_cast<WAVEFORMATEX *>(&device_format),
					nullptr);
			}

			if (FAILED(result)) {
				throw MakeHResultError(result, "Unable to initialize audio client");
			}
		}
	} else {
		if (HRESULT result = client->Initialize(
			    AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
			    buffer_duration, 0,
			    reinterpret_cast<WAVEFORMATEX *>(&device_format), nullptr);
		    FAILED(result)) {
			throw MakeHResultError(result,
					       "Unable to initialize audio client");
		}
	}

	auto render_client = GetService<IAudioRenderClient>(*client);

	const UINT32 buffer_size_in_frames = GetBufferSizeInFrames(*client);

	watermark = buffer_size_in_frames * 3 * FrameSize();
	thread.emplace(client.get(), std::move(render_client), FrameSize(),
		       buffer_size_in_frames, is_exclusive);

	SetEventHandle(*client, thread->event.handle());

	thread->Start();
}

void
WasapiOutput::Close() noexcept
{
	assert(thread);

	try {
		com_worker->Async([&]() {
			Stop(*client);
		}).get();
		thread->CheckException();
	} catch (std::exception &err) {
		FormatError(wasapi_output_domain, "exception while stoping: %s",
			    err.what());
	}
	is_started = false;
	thread->Finish();
	thread->Join();
	com_worker->Async([&]() {
		thread.reset();
		client.reset();
	}).get();
	pcm_export.reset();
}

std::chrono::steady_clock::duration
WasapiOutput::Delay() const noexcept
{
	if (!is_started) {
		// idle while paused
		return std::chrono::seconds(1);
	}

	assert(thread);

	const size_t data_size = thread->spsc_buffer.read_available();
	const size_t delay_size = std::max(data_size, watermark) - watermark;

	using s = std::chrono::seconds;
	using duration = std::chrono::steady_clock::duration;
	auto result = duration(s(delay_size)) / device_format.Format.nAvgBytesPerSec;
	return result;
}

size_t
WasapiOutput::Play(const void *chunk, size_t size)
{
	assert(thread);

	not_interrupted.test_and_set();

	ConstBuffer<void> input(chunk, size);
	if (pcm_export) {
		input = pcm_export->Export(input);
	}
	if (input.empty())
		return size;

	do {
		const size_t consumed_size = thread->spsc_buffer.push(
			static_cast<const BYTE *>(input.data), input.size);
		if (consumed_size == 0) {
			assert(is_started);
			thread->WaitDataPoped();
			if (!not_interrupted.test_and_set()) {
				throw AudioOutputInterrupted{};
			}
			continue;
		}

		if (!is_started) {
			is_started = true;
			thread->Play();
			com_worker->Async([&]() {
				Start(*client);
			}).wait();
		}

		thread->CheckException();

		if (pcm_export) {
			return pcm_export->CalcInputSize(consumed_size);
		}
		return consumed_size;
	} while (true);
}

bool
WasapiOutput::Pause()
{
	if (is_started) {
		thread->Pause();
		is_started = false;
	}
	thread->CheckException();
	return true;
}

void
WasapiOutput::Interrupt() noexcept
{
	if (thread) {
		not_interrupted.clear();
		thread->data_poped.Set();
	}
}

void
WasapiOutput::Drain()
{
	assert(thread);

	thread->spsc_buffer.consume_all([](auto &&) {});
	thread->CheckException();
}

/// run inside COMWorkerThread
void
WasapiOutput::OpenDevice()
{
	enumerator.CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
				    CLSCTX_INPROC_SERVER);

	if (enumerate_devices) {
		try {
			EnumerateDevices();
		} catch (...) {
			LogError(std::current_exception());
		}
	}

	if (!device_config.empty()) {
		unsigned int id;
		if (!SafeSilenceTry([this, &id]() { id = std::stoul(device_config); })) {
			device = SearchDevice(device_config);
			if (!device)
				throw FormatRuntimeError("Device '%s' not found",
							 device_config.c_str());
		} else
			device = GetDevice(id);
	} else {
		device = GetDefaultAudioEndpoint(*enumerator);
	}
}

/// run inside COMWorkerThread
bool
WasapiOutput::TryFormatExclusive(const AudioFormat &audio_format)
{
	for (auto test_format : GetFormats(audio_format)) {
		HRESULT result = client->IsFormatSupported(
			AUDCLNT_SHAREMODE_EXCLUSIVE,
			reinterpret_cast<WAVEFORMATEX *>(&test_format), nullptr);
		const auto format_string = ToString(audio_format);
		const auto result_string = std::string(HRESULTToString(result));
		FormatDebug(wasapi_output_domain, "Trying %s %lu %u-%u (exclusive) -> %s",
			    format_string.c_str(), test_format.Format.nSamplesPerSec,
			    test_format.Format.wBitsPerSample,
			    test_format.Samples.wValidBitsPerSample,
			    result_string.c_str());
		if (SUCCEEDED(result)) {
			device_format = test_format;
			return true;
		}

		if (result == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)
			throw std::runtime_error("Exclusive mode not allowed");
	}
	return false;
}

/// run inside COMWorkerThread
void
WasapiOutput::FindExclusiveFormatSupported(AudioFormat &audio_format)
{
	for (uint8_t channels : {0, 2, 6, 8, 7, 1, 4, 5, 3}) {
		if (audio_format.channels == channels) {
			continue;
		}
		if (channels == 0) {
			channels = audio_format.channels;
		}
		auto old_channels = std::exchange(audio_format.channels, channels);
#ifdef ENABLE_DSD
		bool was_dsd = false;
		if (audio_format.format == SampleFormat::DSD) {
			if (dop_setting && TryFormatExclusive(audio_format)) {
				return;
			}
			was_dsd = true;
			SetDSDFallback(audio_format);
		}
#endif
		for (uint32_t rate : {0, 384000, 352800, 192000, 176400, 96000, 88200,
				      48000, 44100, 32000, 22050, 16000, 11025, 8000}) {
			if (audio_format.sample_rate <= rate) {
				continue;
			}
			if (rate == 0) {
				rate = audio_format.sample_rate;
			}
			auto old_rate = std::exchange(audio_format.sample_rate, rate);
			for (SampleFormat format : {
				     SampleFormat::UNDEFINED,
				     SampleFormat::S32,
				     SampleFormat::S24_P32,
				     SampleFormat::S16,
				     SampleFormat::S8,
			     }) {
				if (audio_format.format == format) {
					continue;
				}
				if (format == SampleFormat::UNDEFINED) {
					format = audio_format.format;
				}
				auto old_format =
					std::exchange(audio_format.format, format);
				if (TryFormatExclusive(audio_format)) {
					return;
				}
				audio_format.format = old_format;
			}
			audio_format.sample_rate = old_rate;
		}
#ifdef ENABLE_DSD
		if (was_dsd) {
			audio_format.format = SampleFormat::DSD;
		}
#endif
		audio_format.channels = old_channels;
	}
}

/// run inside COMWorkerThread
void
WasapiOutput::FindSharedFormatSupported(AudioFormat &audio_format)
{
	HRESULT result;

	// In shared mode, different sample rate is always unsupported.
	auto mixer_format = GetMixFormat(*client);

	audio_format.sample_rate = mixer_format->nSamplesPerSec;
	device_format = GetFormats(audio_format).front();

	ComHeapPtr<WAVEFORMATEXTENSIBLE> closest_format;
	result = client->IsFormatSupported(
		AUDCLNT_SHAREMODE_SHARED,
		reinterpret_cast<WAVEFORMATEX *>(&device_format),
		closest_format.AddressCast<WAVEFORMATEX>());
	{
		const auto format_string = ToString(audio_format);
		const auto result_string = std::string(HRESULTToString(result));
		FormatDebug(wasapi_output_domain, "Trying %s %lu %u-%u (shared) -> %s",
			    format_string.c_str(), device_format.Format.nSamplesPerSec,
			    device_format.Format.wBitsPerSample,
			    device_format.Samples.wValidBitsPerSample,
			    result_string.c_str());
	}

	if (FAILED(result) && result != AUDCLNT_E_UNSUPPORTED_FORMAT) {
		throw MakeHResultError(result, "IsFormatSupported failed");
	}

	switch (result) {
	case S_OK:
		break;
	case AUDCLNT_E_UNSUPPORTED_FORMAT:
	default:
		// Trying channels fallback.
		audio_format.channels = mixer_format->nChannels;

		device_format = GetFormats(audio_format).front();

		result = client->IsFormatSupported(
			AUDCLNT_SHAREMODE_SHARED,
			reinterpret_cast<WAVEFORMATEX *>(&device_format),
			closest_format.AddressCast<WAVEFORMATEX>());
		{
			const auto format_string = ToString(audio_format);
			const auto result_string = std::string(HRESULTToString(result));
			FormatDebug(wasapi_output_domain,
				    "Trying %s %lu %u-%u (shared) -> %s",
				    format_string.c_str(),
				    device_format.Format.nSamplesPerSec,
				    device_format.Format.wBitsPerSample,
				    device_format.Samples.wValidBitsPerSample,
				    result_string.c_str());
		}
		if (FAILED(result)) {
			throw MakeHResultError(result, "Format is not supported");
		}
		break;
	case S_FALSE:
		if (closest_format->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
			device_format = *closest_format;
		} else {
			device_format.Samples.wValidBitsPerSample =
				device_format.Format.wBitsPerSample;
			device_format.Format = closest_format->Format;
			switch (std::exchange(device_format.Format.wFormatTag,
					      WAVE_FORMAT_EXTENSIBLE)) {
			case WAVE_FORMAT_PCM:
				device_format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
				break;
			case WAVE_FORMAT_IEEE_FLOAT:
				device_format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
				break;
			default:
				gcc_unreachable();
			}
		}
		break;
	}

	// Copy closest match back to audio_format.
	audio_format.channels = device_format.Format.nChannels;
	audio_format.sample_rate = device_format.Format.nSamplesPerSec;
	if (device_format.SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
		switch (device_format.Format.wBitsPerSample) {
		case 8:
			audio_format.format = SampleFormat::S8;
			break;
		case 16:
			audio_format.format = SampleFormat::S16;
			break;
		case 32:
			audio_format.format =
				device_format.Samples.wValidBitsPerSample == 32
					? SampleFormat::S32
					: SampleFormat::S24_P32;
			break;
		}
	} else if (device_format.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
		audio_format.format = SampleFormat::FLOAT;
	}
}

/// run inside COMWorkerThread
void
WasapiOutput::EnumerateDevices()
{
	const auto device_collection = EnumAudioEndpoints(*enumerator);

	const UINT count = GetCount(*device_collection);
	for (UINT i = 0; i < count; ++i) {
		const auto enumerated_device = Item(*device_collection, i);

		const auto property_store =
			OpenPropertyStore(*enumerated_device);

		auto name = GetString(*property_store,
				      PKEY_Device_FriendlyName);
		if (name == nullptr)
			continue;

		FormatNotice(wasapi_output_domain,
			     "Device \"%u\" \"%s\"", i, name.c_str());
	}
}

/// run inside COMWorkerThread
ComPtr<IMMDevice>
WasapiOutput::GetDevice(unsigned int index)
{
	const auto device_collection = EnumAudioEndpoints(*enumerator);
	return Item(*device_collection, index);
}

/// run inside COMWorkerThread
ComPtr<IMMDevice>
WasapiOutput::SearchDevice(std::string_view name)
{
	const auto device_collection = EnumAudioEndpoints(*enumerator);

	const UINT count = GetCount(*device_collection);
	for (UINT i = 0; i < count; ++i) {
		auto d = Item(*device_collection, i);

		const auto property_store = OpenPropertyStore(*d);
		auto n = GetString(*property_store, PKEY_Device_FriendlyName);
		if (n != nullptr && name.compare(n) == 0)
			return d;
	}

	return nullptr;
}

static bool
wasapi_output_test_default_device()
{
	return true;
}

const struct AudioOutputPlugin wasapi_output_plugin = {
	"wasapi",
	wasapi_output_test_default_device,
	WasapiOutput::Create,
	&wasapi_mixer_plugin,
};