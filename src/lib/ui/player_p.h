#ifndef __AVBOX_PLAYER_PRIVATE__
#define __AVBOX_PLAYER_PRIVATE__

#include "../avbox.h"


/* flush flags */
#define AVBOX_PLAYER_FLUSH_INVALID		(0x0)
#define AVBOX_PLAYER_FLUSH_AUDIO		(0x1)
#define AVBOX_PLAYER_FLUSH_SUBPX		(0x2)
#define AVBOX_PLAYER_FLUSH_VIDEO		(0x4)
#define AVBOX_PLAYER_FLUSH_ALL			(AVBOX_PLAYER_FLUSH_VIDEO|\
							AVBOX_PLAYER_FLUSH_AUDIO|AVBOX_PLAYER_FLUSH_SUBPX)

#define AVBOX_PLAYER_PACKET_TYPE_SET_CLOCK	(0x1)
#define AVBOX_PLAYER_PACKET_TYPE_VIDEO		(0x2)


LISTABLE_STRUCT(avbox_av_frame,
	AVFrame *avframe;
);


LISTABLE_STRUCT(avbox_av_packet,
	AVPacket *avpacket;
);


struct avbox_player_packet
{
	int type;
	union {
		struct avbox_av_frame *video_frame;
		int64_t clock_value;
	};
};


/**
 * Time function pointer.
 */
typedef int64_t (*avbox_player_time_fn)(
	struct avbox_player * const inst);


struct avbox_player
{
	struct avbox_object *object;
	struct avbox_window *window;
	struct avbox_window *video_window;
	struct avbox_queue *video_packets_q;
	struct avbox_queue *audio_packets_q;
	struct avbox_queue *video_frames_q;
	struct avbox_audiostream *audio_stream;
	struct avbox_stopwatch *video_time;
	struct avbox_checkpoint video_decoder_checkpoint;
	struct avbox_checkpoint video_output_checkpoint;
	struct avbox_checkpoint audio_decoder_checkpoint;
	struct avbox_checkpoint stream_parser_checkpoint;
	struct avbox_delegate *video_output_worker;
	struct avbox_delegate *video_decoder_worker;
	struct avbox_delegate *audio_decoder_worker;
	struct avbox_delegate *stream_input_worker;
	struct avbox_thread *video_output_thread;
	struct avbox_thread *video_decoder_thread;
	struct avbox_thread *audio_decoder_thread;
	struct avbox_thread *stream_input_thread;
	struct avbox_thread *control_thread;
	struct avbox_rational aspect_ratio;
	struct avbox_player_state_info state_info;
	struct avbox_player_stream stream;

	const char *media_file;
	const char *next_file;
	enum avbox_player_status status;
	int underrun_timer_id;
	int stream_exit_timer_id;
	int still_frame;
	int still_frame_timer_id;
	struct avbox_syncarg *still_frame_waiter;
	int audio_stream_id;
	int audio_stream_index;
	int video_stream_index;
	int play_state;
	int stream_quit;
	int stream_percent;
	int stream_exiting;
	int video_decoder_flushed;
	int audio_decoder_flushed;
	int flushing;
	int underrun;
	int stopping;
	int paused;
	int pools_primed;

	avbox_player_time_fn getmastertime;
	AVFormatContext *fmt_ctx;
	struct avbox_av_frame *last_video_frame;
	pthread_mutex_t state_lock;
	LIST subscribers;

	LIST frame_pool;
	LIST av_packet_pool;
	LIST packet_pool;
	LIST ctlmsg_pool;
	pthread_mutex_t frame_pool_lock;
	pthread_mutex_t packet_pool_lock;
	pthread_mutex_t av_packet_pool_lock;
	pthread_mutex_t ctlmsg_pool_lock;

#ifdef DEBUG_MEMORY_POOLS
	unsigned int frame_pool_allocs;
	unsigned int packet_pool_allocs;
	unsigned int av_packet_pool_allocs;
	unsigned int ctlmsg_pool_allocs;
#endif

	/* playlist stuff */
	/* TODO: this belongs in the application code */
	LIST playlist;
	struct avbox_playlist_item *playlist_item;
};


int
avbox_player_get_video_decode_cache_size(struct avbox_player * const inst);


INTERNAL void
release_av_packet(struct avbox_player * const inst, struct avbox_av_packet * const packet);


/**
 * Allocate an avframe from the pool.
 */
INTERNAL struct avbox_av_frame*
acquire_av_frame(struct avbox_player * const inst);


/**
 * Return an AVFrame to the pool
 */
INTERNAL void
release_av_frame(struct avbox_player * const inst, struct avbox_av_frame * const frame);


INTERNAL struct avbox_player_packet*
acquire_packet(struct avbox_player * const inst);


INTERNAL void
release_packet(struct avbox_player * const inst, struct avbox_player_packet * const packet);


#endif
