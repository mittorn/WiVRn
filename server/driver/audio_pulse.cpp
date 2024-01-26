#include "audio_pulse.h"

#include "../wivrn_ipc.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "utils/sync_queue.h"
#include "utils/wrap_lambda.h"
#include "wivrn_session.h"
#if 0
#include <pulse/context.h>
#include <pulse/ext-device-manager.h>
#include <pulse/introspect.h>
#include <pulse/proplist.h>
#include <pulse/thread-mainloop.h>
#endif
#include <atomic>
#include <fcntl.h>
#include <filesystem>
#include <future>
#include <iostream>
#include <poll.h>
#include <sys/poll.h>

static const char * source_name = "WiVRn-mic";
static const char * sink_name = "WiVRn";
static const char * source_pipe = "wivrn-source";
static const char * sink_pipe = "wivrn-sink";

struct module_entry
{
	uint32_t module;
	uint32_t device;
	std::filesystem::path socket;
};

std::filesystem::path get_socket_path()
{
	const char * path = std::getenv("XDG_RUNTIME_DIR");
	if (path)
		return path;
	path = "/tmp/wivrn";
	std::filesystem::create_directories(path);
	U_LOG_W("XDG_RUNTIME_DIR is not set, using %s instead", path);
	return path;
}

module_entry ensure_sink(const char * name, const std::string & description, int channels, int sample_rate)
{
	std::filesystem::path fifo = get_socket_path() / sink_pipe;
	module_entry source = {0};
	source.socket = fifo;
	return source;

}

module_entry ensure_source( const char * name, const std::string & description, int channels, int sample_rate)
{
	std::filesystem::path fifo = get_socket_path() / source_pipe;
	module_entry source = {0};
	source.socket = fifo;
	return source;
}

struct pulse_device : public audio_device
{
	xrt::drivers::wivrn::to_headset::audio_stream_description desc;

	std::thread mic_thread;
	std::thread speaker_thread;
	std::atomic<bool> quit;

	std::optional<module_entry> speaker;
	std::optional<module_entry> microphone;

	utils::sync_queue<audio_data> mic_buffer;

	xrt::drivers::wivrn::fd_base speaker_pipe;
	xrt::drivers::wivrn::fd_base mic_pipe;

	xrt::drivers::wivrn::wivrn_session & session;

	~pulse_device()
	{
		quit = true;
		mic_buffer.close();
		if (mic_thread.joinable())
			mic_thread.join();
		if (speaker_thread.joinable())
			speaker_thread.join();
		if (speaker or microphone)
		{
			try
			{
			}
			catch (const std::exception & e)
			{
				std::cout << "failed to depublish pulseaudio modules: "
				          << e.what() << std::endl;
			}
		}
	}

	xrt::drivers::wivrn::to_headset::audio_stream_description description() const override
	{
		return desc;
	};

	void run_speaker()
	{
		assert(desc.speaker);
		pthread_setname_np(pthread_self(), "speaker_thread");

		U_LOG_I("started speaker thread, sample rate %dHz, %d channels", desc.speaker->sample_rate, desc.speaker->num_channels);

		const size_t sample_size = desc.speaker->num_channels * sizeof(int16_t);
		// use buffers of up to 2ms
		// read buffers must be smaller than buffer size on client or we will discard chunks often
		const size_t buffer_size = (desc.speaker->sample_rate * sample_size * 2) / 1000;
		xrt::drivers::wivrn::audio_data packet;
		std::vector<uint8_t> buffer(buffer_size, 0);
		size_t remainder = 0;

		// Flush existing data, but keep alignment
		{
			char sewer[1024];
			while (true)
			{
				int size = read(speaker_pipe.get_fd(), sewer, sizeof(sewer));
				if (size <= 0)
					break;
				remainder = (remainder + size) % sample_size;
			}
		}

		try
		{
			while (not quit)
			{
				pollfd pfd{};
				pfd.fd = speaker_pipe.get_fd();
				pfd.events = POLLIN;

				int r = poll(&pfd, 1, 100);
				if (r < 0)
					throw std::system_error(errno, std::system_category());
				if (pfd.revents & (POLLHUP | POLLERR))
					throw std::runtime_error("Error on speaker pipe");
				if (pfd.revents & POLLIN)
				{
					int size = read(pfd.fd, buffer.data() + remainder, buffer_size - remainder);
					if (size < 0)
						throw std::system_error(errno, std::system_category());
					size += remainder;              // full size of available data
					remainder = size % sample_size; // data to keep for next iteration
					size -= remainder;              // size of data to send
					packet.payload = std::span<uint8_t>(buffer.begin(), size);
					packet.timestamp = session.get_offset().to_headset(os_monotonic_get_ns()).count();
					session.send_control(packet);
					U_LOG_I("sendo audio: %zu bytes", packet.payload.size_bytes());
					// put the remaining data at the beginning of the buffer
					memmove(buffer.data(), buffer.data() + size, remainder);
				}
			}
		}
		catch (const std::exception & e)
		{
			U_LOG_E("Error in audio thread: %s", e.what());
		}
	}

	void run_mic()
	{
		assert(desc.microphone);
		pthread_setname_np(pthread_self(), "mic_thread");
//		printf("mic %p\n", desc.microphone);

		const size_t sample_size = desc.microphone->num_channels * sizeof(int16_t);
		try
		{
			while (1)//not quit)
			{
//				printf("micr %d\n", sample_size);
#if 1
				pollfd pfd{};
				pfd.fd = mic_pipe.get_fd();
				pfd.events = POLLOUT;

				int r = poll(&pfd, 1, 100);
				if (r < 0)
					throw std::system_error(errno, std::system_category());
				if (pfd.revents & (POLLHUP | POLLERR))
					throw std::runtime_error("Error on mic pipe");
				if (pfd.revents & POLLOUT)
				{
#endif
					auto packet = mic_buffer.pop();
					auto & buffer = packet.payload;

					int written = write(mic_pipe.get_fd(), buffer.data(), buffer.size());
					//printf("mic %d\n", written);
				//	if (written < 0)
				//		throw std::system_error(errno, std::system_category());

					// Discard anything that didn't fit the buffer
				}
			}
		}
		catch (const std::exception & e)
		{
			U_LOG_E("Error in audio thread: %s", e.what());
		}
	}

	void process_mic_data(xrt::drivers::wivrn::audio_data && mic_data) override
	{
		mic_buffer.push(std::move(mic_data));
	}

	pulse_device(
	        const std::string & source_name,
	        const std::string & source_description,
	        const std::string & sink_name,
	        const std::string & sink_description,
	        const xrt::drivers::wivrn::from_headset::headset_info_packet & info,
	        xrt::drivers::wivrn::wivrn_session & session) :
	        session(session)
	{
		//pa_connection cnx("WiVRn");
//		printf("mic %p\n", info.microphone);
		if (info.microphone)
		{
			microphone = ensure_source(source_name.c_str(), source_description, info.microphone->num_channels, info.microphone->sample_rate);
			desc.microphone = {
			        .num_channels = info.microphone->num_channels,
			        .sample_rate = info.microphone->sample_rate};

			mic_pipe = open(microphone->socket.c_str(), O_WRONLY | O_NONBLOCK);
			if (not mic_pipe)
				throw std::system_error(errno, std::system_category(), "failed to open mic pipe " + microphone->socket.string());
			mic_thread = std::thread([this]() { run_mic(); });
		}

		if (info.speaker)
		{
			speaker = ensure_sink(sink_name.c_str(), sink_description, info.speaker->num_channels, info.speaker->sample_rate);
			desc.speaker = {
			        .num_channels = info.speaker->num_channels,
			        .sample_rate = info.speaker->sample_rate};

			speaker_pipe = open(speaker->socket.c_str(), O_RDONLY | O_NONBLOCK);
			if (not speaker_pipe)
				throw std::system_error(errno, std::system_category(), "failed to open speaker pipe " + speaker->socket.string());

			speaker_thread = std::thread([this]() { run_speaker(); });
		}
	}
};

std::shared_ptr<audio_device> create_pulse_handle(
        const std::string & source_name,
        const std::string & source_description,
        const std::string & sink_name,
        const std::string & sink_description,
        const xrt::drivers::wivrn::from_headset::headset_info_packet & info,
        wivrn_session & session)
{
	return std::make_shared<pulse_device>(
	        source_name, source_description, sink_name, sink_description, info, session);
}

void unload_module(uintptr_t id)
{
	//pa_connection cnx("WiVRn");
	//unload_module(cnx, id);
}
