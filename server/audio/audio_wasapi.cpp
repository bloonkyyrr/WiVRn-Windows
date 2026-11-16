/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "audio_wasapi.h"

#if defined(_WIN32)

#include "driver/wivrn_session.h"
#include "os/os_time.h"
#include "platform.h"
#include "util/u_logging.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

namespace wivrn
{
namespace
{

std::string hresult_string(HRESULT hr)
{
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "0x%08lx", (unsigned long)hr);
	return buffer;
}

void check(HRESULT hr, const char * what)
{
	if (FAILED(hr))
		throw std::runtime_error(std::string(what) + " failed (" + hresult_string(hr) + ")");
}

template <typename T>
void release(T *& pointer)
{
	if (pointer)
	{
		pointer->Release();
		pointer = nullptr;
	}
}

/*
 * Captures the default render endpoint in loopback mode and forwards it to the
 * headset speaker. Windows has no in-process way to publish a virtual capture
 * device, so microphone forwarding is not advertised without a virtual audio
 * driver: the headset must not send microphone data when it is not requested.
 */
class wasapi_device final : public audio_device
{
	to_headset::audio_stream_description desc;
	wivrn_session & session;

	IMMDeviceEnumerator * enumerator = nullptr;
	IMMDevice * endpoint = nullptr;
	IAudioClient * client = nullptr;
	IAudioCaptureClient * capture = nullptr;

	WAVEFORMATEX format{};
	std::atomic<bool> paused{false};
	std::atomic<bool> running{true};
	std::promise<void> ready;
	std::future<void> ready_future = ready.get_future();
	std::jthread thread;

	void cleanup()
	{
		release(capture);
		release(client);
		release(endpoint);
		release(enumerator);
	}

	void setup()
	{
		check(CoCreateInstance(__uuidof(MMDeviceEnumerator),
		                       nullptr,
		                       CLSCTX_ALL,
		                       __uuidof(IMMDeviceEnumerator),
		                       reinterpret_cast<void **>(&enumerator)),
		      "CoCreateInstance(MMDeviceEnumerator)");

		check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint),
		      "GetDefaultAudioEndpoint");

		check(endpoint->Activate(__uuidof(IAudioClient),
		                         CLSCTX_ALL,
		                         nullptr,
		                         reinterpret_cast<void **>(&client)),
		      "Activate(IAudioClient)");

		format.wFormatTag = WAVE_FORMAT_PCM;
		format.nChannels = desc.speaker->num_channels;
		format.nSamplesPerSec = desc.speaker->sample_rate;
		format.wBitsPerSample = 16;
		format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
		format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
		format.cbSize = 0;

		// Loopback capture of the default render endpoint, converted by WASAPI
		// to the format the headset expects.
		HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
		                                AUDCLNT_STREAMFLAGS_LOOPBACK |
		                                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
		                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
		                                2000000,
		                                0,
		                                &format,
		                                nullptr);

		if (FAILED(hr))
		{
			// AUTOCONVERTPCM is not accepted by every endpoint; retry when the
			// endpoint already uses the requested format.
			hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
			                        AUDCLNT_STREAMFLAGS_LOOPBACK,
			                        2000000,
			                        0,
			                        &format,
			                        nullptr);
		}

		check(hr, "IAudioClient::Initialize");

		check(client->GetService(__uuidof(IAudioCaptureClient),
		                         reinterpret_cast<void **>(&capture)),
		      "GetService(IAudioCaptureClient)");
	}

	void run(std::stop_token stop_token)
	{
		HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (FAILED(hr))
		{
			ready.set_exception(std::make_exception_ptr(
			        std::runtime_error("CoInitializeEx failed (" + hresult_string(hr) + ")")));
			return;
		}

		try
		{
			setup();
		}
		catch (...)
		{
			ready.set_exception(std::current_exception());
			cleanup();
			CoUninitialize();
			return;
		}

		ready.set_value();

		bool started = false;
		while (running and not stop_token.stop_requested())
		{
			if (paused)
			{
				if (started)
				{
					client->Stop();
					started = false;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}

			if (not started)
			{
				hr = client->Start();
				if (FAILED(hr))
				{
					U_LOG_W("WASAPI: could not start capture (%s)", hresult_string(hr).c_str());
					break;
				}
				started = true;
			}

			UINT32 packet_frames = 0;
			hr = capture->GetNextPacketSize(&packet_frames);
			if (FAILED(hr))
			{
				U_LOG_W("WASAPI: GetNextPacketSize failed (%s)", hresult_string(hr).c_str());
				break;
			}

			if (packet_frames == 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				continue;
			}

			BYTE * data = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;
			hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
			if (FAILED(hr))
			{
				U_LOG_W("WASAPI: GetBuffer failed (%s)", hresult_string(hr).c_str());
				break;
			}

			if (frames and data and not(flags & AUDCLNT_BUFFERFLAGS_SILENT))
			{
				try
				{
					session.send_control(audio_data{
					        .timestamp = session.get_offset().to_headset(os_monotonic_get_ns()),
					        .payload = std::span<uint8_t>(data, (size_t)frames * format.nBlockAlign),
					});
				}
				catch (const std::exception & e)
				{
					U_LOG_D("WASAPI: failed to send audio data: %s", e.what());
				}
			}

			capture->ReleaseBuffer(frames);
		}

		if (started)
			client->Stop();

		cleanup();
		CoUninitialize();
	}

public:
	wasapi_device(const std::string & source_name,
	              const std::string & source_description,
	              const std::string & sink_name,
	              const std::string & sink_description,
	              const from_headset::headset_info_packet & info,
	              wivrn_session & session) :
	        session(session)
	{
		(void)source_name;
		(void)source_description;
		(void)sink_name;
		(void)sink_description;

		if (not info.speaker or info.speaker->num_channels == 0 or info.speaker->sample_rate == 0)
			throw std::runtime_error("headset did not report a usable speaker format");

		desc.speaker = {
		        .num_channels = info.speaker->num_channels,
		        .sample_rate = info.speaker->sample_rate,
		};

		thread = std::jthread([this](std::stop_token stop_token) {
			run(stop_token);
		});

		// Propagates setup failures from the audio thread to the factory.
		ready_future.get();
	}

	~wasapi_device() override
	{
		running = false;
		if (thread.joinable())
			thread.join();
	}

	void process_mic_data(wivrn::audio_data &&) override {}

	void pause() override
	{
		paused = true;
	}

	void resume() override
	{
		paused = false;

		try
		{
			session.send_control(to_headset::audio_stream_description{desc});
			session.send_control(to_headset::feature_control{to_headset::feature_control::microphone, false});
		}
		catch (const std::exception & e)
		{
			U_LOG_W("WASAPI: failed to update audio state: %s", e.what());
		}
	}
};

} // namespace

std::unique_ptr<audio_device> create_wasapi_handle(
        const std::string & source_name,
        const std::string & source_description,
        const std::string & sink_name,
        const std::string & sink_description,
        const wivrn::from_headset::headset_info_packet & info,
        wivrn::wivrn_session & session)
{
	try
	{
		return std::make_unique<wasapi_device>(
		        source_name, source_description, sink_name, sink_description, info, session);
	}
	catch (const std::exception & e)
	{
		U_LOG_I("WASAPI backend creation failed: %s", e.what());
		return nullptr;
	}
}

} // namespace wivrn

#endif
