/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

// Include first because of incompatibility between Eigen and X includes
#include "wivrn_session.h"

#include "video_encoder.h"
#include "util/u_logging.h"

#include <string>

#include "wivrn_config.h"

#ifdef WIVRN_USE_NVENC
#include "video_encoder_nvenc.h"
#endif
#ifdef WIVRN_USE_VAAPI
#include "ffmpeg/VideoEncoderVA.h"
#endif
#ifdef WIVRN_USE_X264
#include "video_encoder_x264.h"
#endif


#include <signal.h>
#include <ucontext.h>
#include <time.h>
#if defined( __GLIBC__ )
#if ( __GLIBC__ <= 2 ) && ( __GLIBC_MINOR__ <= 30 )
// Library support was added in glibc 2.30.
// Earlier glibc versions did not provide a wrapper for this system call,
// necessitating the use of syscall(2).
#include <sys/syscall.h>

static pid_t gettid( void )
{
	return syscall( SYS_gettid );
}
#endif // ( __GLIBC__ <= 2 ) && ( __GLIBC_MINOR__ <= 30 )

// Glibc misses this macro in POSIX headers (bits/types/sigevent_t.h)
// but it does present in Linux headers (asm-generic/siginfo.h) and musl
#if !defined( sigev_notify_thread_id )
#define sigev_notify_thread_id _sigev_un._tid
#endif // !defined( sigev_notify_thread_id )
#endif // defined(__GLIBC__)
static void Linux_TimerHandler( int sig, siginfo_t *si, void *uc )
{
	timer_t  *tidp = (timer_t*)si->si_value.sival_ptr;
	int overrun = timer_getoverrun( *tidp );
	printf( "Frame too long (overrun %d)!\n", overrun );
}

#define DEBUG_TIMER_SIGNAL SIGRTMIN

static void Linux_SetTimer( float tm )
{
	static timer_t timerid;

	if( !timerid && tm )
	{
		struct sigevent    sev = { 0 };
		struct sigaction   sa = { 0 };

		sa.sa_flags = SA_SIGINFO;
		sa.sa_sigaction = Linux_TimerHandler;
		sigaction( DEBUG_TIMER_SIGNAL, &sa, NULL );
		// this path availiable in POSIX, but may signal wrong thread...
		// sev.sigev_notify = SIGEV_SIGNAL;
		sev.sigev_notify = SIGEV_THREAD_ID;
		sev.sigev_notify_thread_id = gettid();
		sev.sigev_signo = DEBUG_TIMER_SIGNAL;
		sev.sigev_value.sival_ptr = &timerid;
		timer_create( CLOCK_REALTIME, &sev, &timerid );
	}

	if( timerid )
	{
		struct itimerspec  its = {0};
		its.it_value.tv_sec = tm;
		its.it_value.tv_nsec = 1000000000ULL * fmod( tm, 1.0f );
		its.it_interval.tv_sec = 0;
		its.it_interval.tv_nsec = 0;
		timer_settime( timerid, 0, &its, NULL );
	}
}
namespace xrt::drivers::wivrn
{

std::unique_ptr<VideoEncoder> VideoEncoder::Create(
        vk_bundle * vk,
        vk_cmd_pool & pool,
        encoder_settings & settings,
        uint8_t stream_idx,
        int input_width,
        int input_height,
        float fps)
{
	using namespace std::string_literals;
	std::unique_ptr<VideoEncoder> res;
#ifdef WIVRN_USE_X264
	if (settings.encoder_name == encoder_x264)
	{
		res = std::make_unique<VideoEncoderX264>(vk, pool, settings, input_width, input_height, fps);
	}
#endif
#ifdef WIVRN_USE_NVENC
	if (settings.encoder_name == encoder_nvenc)
	{
		res = std::make_unique<VideoEncoderNvenc>(vk, settings, fps);
	}
#endif
#ifdef WIVRN_USE_VAAPI
	if (settings.encoder_name == encoder_vaapi)
	{
		res = std::make_unique<VideoEncoderVA>(vk, settings, fps);
	}
#endif
	if (res)
	{
		res->stream_idx = stream_idx;
	}
	else
	{
		U_LOG_E("No video encoder %s", settings.encoder_name.c_str());
	}
	return res;
}

void VideoEncoder::SyncNeeded()
{
	sync_needed = true;
}

void VideoEncoder::Encode(wivrn_session & cnx,
                          const to_headset::video_stream_data_shard::view_info_t & view_info,
                          uint64_t frame_index,
                          int index)
{
//	Linux_SetTimer( 15.0 / 1000 );
	this->cnx = &cnx;
	auto target_timestamp = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(view_info.display_time));
	bool idr = sync_needed.exchange(false);
	static uint64_t last_idr;
	static uint64_t last_display_time;
	static uint64_t timepoint1, timepoint2;
	if(frame_index % 10 == 0)
	{
		timepoint2 = timepoint1;
		timepoint1 = view_info.display_time;
	}
	if( (timepoint1 - timepoint2) > 112000000 && (frame_index - last_idr) < 5000)
		idr = false;
	if(frame_index - last_idr < 90)
		idr = false;
	if( (view_info.display_time - last_display_time) > 12000000)
		idr = false;
	if(idr)
		last_idr = frame_index;
	if(last_display_time == 0)
		idr = true;
	last_display_time = view_info.display_time;
	const char* extra = idr ? ",idr" : ",p";
	cnx.dump_time("encode_begin", frame_index, os_monotonic_get_ns(), stream_idx, extra);

	// Prepare the video shard template
	shard.stream_item_idx = stream_idx;
	shard.frame_idx = frame_index;
	shard.shard_idx = 0;
	shard.view_info = view_info;
//	printf("encode\n");

	Encode(index, idr, target_timestamp);
	cnx.dump_time("encode_end", frame_index, os_monotonic_get_ns(), stream_idx, extra);
//	Linux_SetTimer( 0 );
}

void VideoEncoder::SendData(std::span<uint8_t> data, bool end_of_frame)
{
	std::lock_guard lock(mutex);
//	printf("send data\n");
#if 0
	std::ofstream debug("/tmp/video_dump-" + std::to_string(stream_idx), std::ios::app);
	debug.write((char*)data.data(), data.size());
#endif
	if (shard.shard_idx == 0)
		cnx->dump_time("send_begin", shard.frame_idx, os_monotonic_get_ns(), stream_idx);

	shard.flags = to_headset::video_stream_data_shard::start_of_slice;
	auto begin = data.begin();
	auto end = data.end();
	while (begin != end)
	{
		const size_t view_info_size = sizeof(to_headset::video_stream_data_shard::view_info_t);
		const size_t max_payload_size = to_headset::video_stream_data_shard::max_payload_size - (shard.view_info ? view_info_size : 0);
		auto next = std::min(end, begin + max_payload_size);
		if (next == end)
		{
			shard.flags |= to_headset::video_stream_data_shard::end_of_slice;
			if (end_of_frame)
				shard.flags |= to_headset::video_stream_data_shard::end_of_frame;
		}
		shard.payload = {begin, next};
		cnx->send_stream(shard);
		++shard.shard_idx;
		shard.flags = 0;
		shard.view_info.reset();
		begin = next;
	}
	if (end_of_frame)
		cnx->dump_time("send_end", shard.frame_idx, os_monotonic_get_ns(), stream_idx);
}

} // namespace xrt::drivers::wivrn
