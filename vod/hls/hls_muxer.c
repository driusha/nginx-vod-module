#include "frame_joiner_filter.h"
#include "hls_muxer.h"

#define HLS_TIMESCALE (90000)

// from ffmpeg mpegtsenc
#define DEFAULT_PES_HEADER_FREQ 16
#define DEFAULT_PES_PAYLOAD_SIZE ((DEFAULT_PES_HEADER_FREQ - 1) * 184 + 170)

vod_status_t 
hls_muxer_init(
	hls_muxer_state_t* state, 
	request_context_t* request_context,
	hls_muxer_conf_t* conf,
	uint32_t segment_index,
	mpeg_metadata_t* mpeg_metadata, 
	read_cache_state_t* read_cache_state, 
	write_callback_t write_callback, 
	void* write_context,
	bool_t* simulation_supported)
{
	mpegts_encoder_init_streams_state_t init_streams_state;
	mpeg_stream_metadata_t* cur_stream_metadata;
	hls_muxer_stream_state_t* cur_stream;
	const media_filter_t* next_filter;
	void* next_filter_context;
	vod_status_t rc;

	*simulation_supported = TRUE;

	state->request_context = request_context;
	state->read_cache_state = read_cache_state;
	state->cur_frame = NULL;
	state->video_duration = 0;

	// init the write queue
	write_buffer_queue_init(&state->queue, request_context);
	state->queue.write_callback = write_callback;
	state->queue.write_context = write_context;

	// init the packetizer streams and get the packet ids / stream ids
	rc = mpegts_encoder_init_streams(request_context, &state->queue, &init_streams_state, segment_index);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// allocate the streams
	state->first_stream = vod_alloc(request_context->pool, sizeof(*state->first_stream) * mpeg_metadata->streams.nelts);
	if (state->first_stream == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"hls_muxer_init: vod_alloc failed (1)");
		return VOD_ALLOC_FAILED;
	}

	state->last_stream = state->first_stream + mpeg_metadata->streams.nelts;
	
	cur_stream_metadata = mpeg_metadata->first_stream;
	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++, cur_stream_metadata++)
	{
		cur_stream->media_type = cur_stream_metadata->media_info.media_type;
		cur_stream->timescale = cur_stream_metadata->media_info.timescale;
		cur_stream->frames_file_index = cur_stream_metadata->frames_file_index;
		cur_stream->first_frame = cur_stream_metadata->frames;
		cur_stream->cur_frame = cur_stream_metadata->frames;
		cur_stream->last_frame = cur_stream->cur_frame + cur_stream_metadata->frame_count;
		cur_stream->first_frame_time_offset = cur_stream_metadata->first_frame_time_offset;
		cur_stream->clip_from_frame_offset = cur_stream_metadata->clip_from_frame_offset;
		cur_stream->next_frame_time_offset = cur_stream->first_frame_time_offset;
		cur_stream->next_frame_dts = rescale_time(cur_stream->next_frame_time_offset, cur_stream->timescale, HLS_TIMESCALE);
		cur_stream->first_frame_offset = cur_stream_metadata->frame_offsets;
		cur_stream->cur_frame_offset = cur_stream_metadata->frame_offsets;

		rc = mpegts_encoder_init(
			&cur_stream->mpegts_encoder_state, 
			&init_streams_state,
			cur_stream->media_type,
			request_context, 
			&state->queue, 
			conf->interleave_frames,
			conf->align_frames);
		if (rc != VOD_OK)
		{
			return rc;
		}

		switch (cur_stream_metadata->media_info.media_type)
		{
		case MEDIA_TYPE_VIDEO:
			if (cur_stream_metadata->media_info.duration_millis > state->video_duration)
			{
				state->video_duration = cur_stream_metadata->media_info.duration_millis;
			}

			cur_stream->buffer_state = NULL;
			cur_stream->top_filter_context = vod_alloc(request_context->pool, sizeof(mp4_to_annexb_state_t));
			if (cur_stream->top_filter_context == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
					"hls_muxer_init: vod_alloc failed (2)");
				return VOD_ALLOC_FAILED;
			}

			rc = mp4_to_annexb_init(
				cur_stream->top_filter_context,
				request_context,
				&mpegts_encoder,
				&cur_stream->mpegts_encoder_state,
				cur_stream_metadata->media_info.extra_data,
				cur_stream_metadata->media_info.extra_data_size,
				cur_stream_metadata->media_info.u.video.nal_packet_size_length);
			if (rc != VOD_OK)
			{
				return rc;
			}

			if (!mp4_to_annexb_simulation_supported(cur_stream->top_filter_context))
			{
				*simulation_supported = FALSE;
			}

			cur_stream->top_filter = &mp4_to_annexb;
			break;

		case MEDIA_TYPE_AUDIO:
			if (conf->interleave_frames)
			{
				// frame interleaving enabled, just join several audio frames according to timestamp
				cur_stream->buffer_state = NULL;

				next_filter_context = vod_alloc(request_context->pool, sizeof(frame_joiner_t));
				if (next_filter_context == NULL)
				{
					vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
						"hls_muxer_init: vod_alloc failed (3)");
					return VOD_ALLOC_FAILED;
				}

				frame_joiner_init(next_filter_context, &mpegts_encoder, &cur_stream->mpegts_encoder_state);

				next_filter = &frame_joiner;
			}
			else
			{
				// no frame interleaving, buffer the audio until it reaches a certain size / delay from video
				cur_stream->buffer_state = vod_alloc(request_context->pool, sizeof(buffer_filter_t));
				if (cur_stream->buffer_state == NULL)
				{
					vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
						"hls_muxer_init: vod_alloc failed (4)");
					return VOD_ALLOC_FAILED;
				}

				rc = buffer_filter_init(cur_stream->buffer_state, request_context, &mpegts_encoder, &cur_stream->mpegts_encoder_state, DEFAULT_PES_PAYLOAD_SIZE);
				if (rc != VOD_OK)
				{
					return rc;
				}

				next_filter = &buffer_filter;
				next_filter_context = cur_stream->buffer_state;
			}

			cur_stream->top_filter_context = vod_alloc(request_context->pool, sizeof(adts_encoder_state_t));
			if (cur_stream->top_filter_context == NULL)
			{
				vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
					"hls_muxer_init: vod_alloc failed (5)");
				return VOD_ALLOC_FAILED;
			}

			rc = adts_encoder_init(
				cur_stream->top_filter_context, 
				request_context, 
				next_filter, 
				next_filter_context, 
				cur_stream_metadata->media_info.extra_data,
				cur_stream_metadata->media_info.extra_data_size);
			if (rc != VOD_OK)
			{
				return rc;
			}

			cur_stream->top_filter = &adts_encoder;
			break;
		}
	}

	mpegts_encoder_finalize_streams(&init_streams_state);

	return VOD_OK;
}

static hls_muxer_stream_state_t* 
hls_muxer_choose_stream(hls_muxer_state_t* state)
{
	hls_muxer_stream_state_t* cur_stream;
	hls_muxer_stream_state_t* result = NULL;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if (cur_stream->cur_frame >= cur_stream->last_frame)
		{
			continue;
		}

		if (result == NULL || cur_stream->next_frame_dts < result->next_frame_dts)
		{
			result = cur_stream;
		}
	}

	return result;
}

static vod_status_t 
hls_muxer_start_frame(hls_muxer_state_t* state)
{
	hls_muxer_stream_state_t* cur_stream;
	hls_muxer_stream_state_t* selected_stream;
	output_frame_t output_frame;
	uint64_t cur_frame_time_offset;
	uint64_t cur_frame_dts;
	uint64_t buffer_dts;
	vod_status_t rc;

	selected_stream = hls_muxer_choose_stream(state);
	if (selected_stream == NULL)
	{
		return VOD_OK;		// done
	}

	// init the frame
	state->cur_frame = selected_stream->cur_frame;
	selected_stream->cur_frame++;
	state->cur_file_index = selected_stream->frames_file_index;
	state->cur_frame_offset = *selected_stream->cur_frame_offset;
	selected_stream->cur_frame_offset++;
	cur_frame_time_offset = selected_stream->next_frame_time_offset;
	selected_stream->next_frame_time_offset += state->cur_frame->duration;
	cur_frame_dts = selected_stream->next_frame_dts;
	selected_stream->next_frame_dts = rescale_time(selected_stream->next_frame_time_offset, selected_stream->timescale, HLS_TIMESCALE);

	state->last_stream_frame = selected_stream->cur_frame >= selected_stream->last_frame;

	// flush any buffered frames if their delay becomes too big
	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if (selected_stream == cur_stream || cur_stream->buffer_state == NULL)
		{
			continue;
		}

		if (buffer_filter_get_dts(cur_stream->buffer_state, &buffer_dts) &&
			cur_frame_dts > buffer_dts + HLS_DELAY / 2)
		{
			rc = buffer_filter_force_flush(cur_stream->buffer_state, FALSE);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
	}

	// set the current top_filter
	state->cur_writer = selected_stream->top_filter;
	state->cur_writer_context = selected_stream->top_filter_context;

	// initialize the mpeg ts frame info
	output_frame.pts = rescale_time(cur_frame_time_offset + state->cur_frame->pts_delay, selected_stream->timescale, HLS_TIMESCALE);
	output_frame.dts = cur_frame_dts;
	output_frame.key = state->cur_frame->key_frame;
	output_frame.size = state->cur_frame->size;
	output_frame.header_size = 0;

	state->cache_slot_id = selected_stream->mpegts_encoder_state.stream_info.pid;

	// start the frame
	rc = state->cur_writer->start_frame(state->cur_writer_context, &output_frame);
	if (rc != VOD_OK)
	{
		return rc;
	}

	state->cur_frame_pos = 0;

	return VOD_OK;
}

static vod_status_t
hls_muxer_send(hls_muxer_state_t* state)
{
	hls_muxer_stream_state_t* cur_stream;
	off_t min_offset = state->queue.cur_offset;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if (cur_stream->mpegts_encoder_state.send_queue_offset < min_offset)
		{
			min_offset = cur_stream->mpegts_encoder_state.send_queue_offset;
		}
	}

	return write_buffer_queue_send(&state->queue, min_offset);
}

vod_status_t 
hls_muxer_process(hls_muxer_state_t* state)
{
	u_char* read_buffer;
	uint32_t read_size;
	uint32_t write_size;
	uint64_t offset;
	vod_status_t rc;
	bool_t first_time = (state->cur_frame == NULL);
	bool_t wrote_data = FALSE;

	for (;;)
	{
		// start a new frame if we don't have a frame
		if (state->cur_frame == NULL)
		{
			rc = hls_muxer_start_frame(state);
			if (rc != VOD_OK)
			{
				return rc;
			}
			
			if (state->cur_frame == NULL)
			{
				break;		// done
			}
		}
		
		// read some data from the frame
		offset = state->cur_frame_offset + state->cur_frame_pos;
		if (!read_cache_get_from_cache(state->read_cache_state, state->cur_frame->size - state->cur_frame_pos, state->cache_slot_id, state->cur_file_index, offset, &read_buffer, &read_size))
		{
			if (!wrote_data && !first_time)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"hls_muxer_process: no data was handled, probably a truncated file");
				return VOD_BAD_DATA;
			}

			rc = hls_muxer_send(state);
			if (rc != VOD_OK)
			{
				return rc;
			}

			return VOD_AGAIN;
		}

		wrote_data = TRUE;
		
		// write the frame
		write_size = vod_min(state->cur_frame->size - state->cur_frame_pos, read_size);
		rc = state->cur_writer->write(state->cur_writer_context, read_buffer, write_size);
		if (rc != VOD_OK)
		{
			return rc;
		}
		state->cur_frame_pos += write_size;
		
		// flush the frame if we finished writing it
		if (state->cur_frame_pos >= state->cur_frame->size)
		{
			rc = state->cur_writer->flush_frame(state->cur_writer_context, state->last_stream_frame);
			if (rc != VOD_OK)
			{
				return rc;
			}
			
			state->cur_frame = NULL;
		}
	}

	// flush the buffer queue
	rc = write_buffer_queue_flush(&state->queue);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}

static void 
hls_muxer_simulation_flush_delayed_streams(hls_muxer_state_t* state, hls_muxer_stream_state_t* selected_stream, uint64_t frame_dts)
{
	hls_muxer_stream_state_t* cur_stream;
	uint64_t buffer_dts;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if (selected_stream == cur_stream || cur_stream->buffer_state == NULL)
		{
			continue;
		}

		if (buffer_filter_get_dts(cur_stream->buffer_state, &buffer_dts) &&
			frame_dts > buffer_dts + HLS_DELAY / 2)
		{
			vod_log_debug2(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"hls_muxer_simulation_flush_delayed_streams: flushing buffered frames buffer dts %L frame dts %L",
				buffer_dts,
				frame_dts);
			buffer_filter_simulated_force_flush(cur_stream->buffer_state, FALSE);
		}
	}
}

static void 
hls_muxer_simulation_write_frame(hls_muxer_stream_state_t* selected_stream, input_frame_t* cur_frame, uint64_t cur_frame_dts, bool_t last_frame)
{
	output_frame_t output_frame;

	// initialize the mpeg ts frame info
	// Note: no need to initialize the pts or original size
	output_frame.dts = cur_frame_dts;
	output_frame.key = cur_frame->key_frame;
	output_frame.header_size = 0;

	selected_stream->top_filter->simulated_start_frame(selected_stream->top_filter_context, &output_frame);
	selected_stream->top_filter->simulated_write(selected_stream->top_filter_context, cur_frame->size);
	selected_stream->top_filter->simulated_flush_frame(selected_stream->top_filter_context, last_frame);
}

static hls_muxer_stream_state_t*
hls_muxer_iframes_choose_stream(hls_muxer_state_t* state)
{
	hls_muxer_stream_state_t* cur_stream;
	hls_muxer_stream_state_t* result = NULL;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		if (cur_stream->cur_frame >= cur_stream->last_frame || 
			cur_stream->next_frame_time_offset >= cur_stream->segment_limit)
		{
			continue;
		}

		if (result == NULL || cur_stream->next_frame_dts < result->next_frame_dts)
		{
			result = cur_stream;
		}
	}

	return result;
}

static void 
hls_muxer_simulation_set_segment_limit(
	hls_muxer_state_t* state,
	uint64_t segment_end,
	uint32_t timescale)
{
	hls_muxer_stream_state_t* cur_stream;

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		cur_stream->segment_limit = (segment_end * cur_stream->timescale) / timescale - cur_stream->clip_from_frame_offset;
		cur_stream->is_first_segment_frame = TRUE;
	}
}

vod_status_t
hls_muxer_simulate_get_iframes(
	hls_muxer_state_t* state, 
	segmenter_conf_t* segmenter_conf, 
	mpeg_metadata_t* mpeg_metadata,
	hls_get_iframe_positions_callback_t callback,
	void* context)
{
	hls_muxer_stream_state_t* selected_stream;
	segment_duration_item_t* cur_item;
	segment_duration_item_t* last_item;
	segment_durations_t segment_durations;
	input_frame_t* cur_frame;
	uint32_t cur_frame_time;
	uint32_t frame_start = 0;
	uint32_t frame_size = 0;
	uint32_t frame_start_time = 0;
	uint32_t first_frame_time = 0;
	uint32_t end_time;
	uint32_t frame_segment_index = 0;
	uint32_t segment_index = 0;
	uint64_t cur_frame_dts;
	uint64_t cur_frame_time_offset;
	uint32_t repeat_count;
	uint64_t segment_end;
	bool_t last_frame;
	vod_status_t rc;
#if (VOD_DEBUG)
	off_t cur_frame_start;
#endif

	// get segment durations
	if (segmenter_conf->align_to_key_frames)
	{
		rc = segmenter_get_segment_durations_accurate(
			state->request_context, 
			segmenter_conf, 
			&mpeg_metadata->first_stream, 
			1, 
			&segment_durations);
	}
	else
	{
		rc = segmenter_get_segment_durations_estimate(
			state->request_context,
			segmenter_conf,
			mpeg_metadata->longest_stream,
			MEDIA_TYPE_COUNT,
			&segment_durations);
	}

	if (rc != VOD_OK)
	{
		return rc;
	}
	
	cur_item = segment_durations.items;
	last_item = segment_durations.items + segment_durations.item_count;
	if (cur_item >= last_item)
	{
		return VOD_OK;
	}


	// initialize the repeat count, segment end, and the per stream limit
	repeat_count = cur_item->repeat_count - 1;
	segment_end = cur_item->duration;
	hls_muxer_simulation_set_segment_limit(state, segment_end, segment_durations.timescale);

	mpegts_encoder_simulated_start_segment(&state->queue);

	for (;;)
	{
		// get a frame
		for (;;)
		{
			// choose a stream for the current frame
			selected_stream = hls_muxer_iframes_choose_stream(state);
			if (selected_stream != NULL)
			{
				break;
			}

			// update the limit for the next segment
			if (repeat_count <= 0)
			{
				cur_item++;
				if (cur_item >= last_item)
				{
					goto done;
				}

				repeat_count = cur_item->repeat_count;
			}

			repeat_count--;
			segment_end += cur_item->duration;
			hls_muxer_simulation_set_segment_limit(state, segment_end, segment_durations.timescale);

			// start the next segment
			mpegts_encoder_simulated_start_segment(&state->queue);
			segment_index++;
		}

		// update the stream state
		cur_frame = selected_stream->cur_frame;
		selected_stream->cur_frame++;
		cur_frame_time_offset = selected_stream->next_frame_time_offset;
		selected_stream->next_frame_time_offset += cur_frame->duration;
		cur_frame_dts = selected_stream->next_frame_dts;
		selected_stream->next_frame_dts = rescale_time(selected_stream->next_frame_time_offset, selected_stream->timescale, HLS_TIMESCALE);
		
		// flush any buffered frames if their delay becomes too big
		hls_muxer_simulation_flush_delayed_streams(state, selected_stream, cur_frame_dts);

		// check whether this is the last frame of the selected stream in this segment
		last_frame = (selected_stream->cur_frame >= selected_stream->last_frame ||
			selected_stream->next_frame_time_offset >= selected_stream->segment_limit);

		// write the frame
#if (VOD_DEBUG)
		cur_frame_start = state->queue.cur_offset;
#endif // VOD_DEBUG

		hls_muxer_simulation_write_frame(
			selected_stream,
			cur_frame,
			cur_frame_dts,
			last_frame);

#if (VOD_DEBUG)
		if (cur_frame_start != state->queue.cur_offset)
		{
			vod_log_debug4(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"hls_muxer_simulate_get_iframes: wrote frame segment %uD packets %uD-%uD dts %L",
				segment_index + 1,
				(uint32_t)(cur_frame_start / MPEGTS_PACKET_SIZE + 1),
				(uint32_t)(state->queue.cur_offset / MPEGTS_PACKET_SIZE + 1),
				cur_frame_dts);
		}
#endif // VOD_DEBUG

		// only care about video key frames
		if (selected_stream->media_type != MEDIA_TYPE_VIDEO)
		{
			continue;
		}

		if (!selected_stream->is_first_segment_frame && cur_frame[-1].key_frame)
		{
			// get the frame time
			cur_frame_time = rescale_time(cur_frame_time_offset - cur_frame[-1].duration + cur_frame[-1].pts_delay, selected_stream->timescale, 1000);		// in millis
			if (frame_size != 0)
			{
				callback(context, frame_segment_index, cur_frame_time - frame_start_time, frame_start, frame_size);
			}
			else
			{
				first_frame_time = cur_frame_time;
			}

			// save the info of the current keyframe
			frame_start = selected_stream->mpegts_encoder_state.last_frame_start_pos;
			frame_size = selected_stream->mpegts_encoder_state.last_frame_end_pos -
				selected_stream->mpegts_encoder_state.last_frame_start_pos;
			frame_start_time = cur_frame_time;
			frame_segment_index = segment_index;
		}

		if (last_frame && cur_frame[0].key_frame)
		{
			// get the frame time
			cur_frame_time = rescale_time(cur_frame_time_offset + cur_frame[0].pts_delay, selected_stream->timescale, 1000);		// in millis
			if (frame_size != 0)
			{
				callback(context, frame_segment_index, cur_frame_time - frame_start_time, frame_start, frame_size);
			}
			else
			{
				first_frame_time = cur_frame_time;
			}

			// save the info of the current keyframe
			frame_start = selected_stream->mpegts_encoder_state.cur_frame_start_pos;
			frame_size = selected_stream->mpegts_encoder_state.cur_frame_end_pos -
				selected_stream->mpegts_encoder_state.cur_frame_start_pos;
			frame_start_time = cur_frame_time;
			frame_segment_index = segment_index;
		}

		selected_stream->is_first_segment_frame = FALSE;
	}

done:

	// call the callback for the last frame
	end_time = first_frame_time + state->video_duration;
	if (frame_size != 0 && end_time > frame_start_time)
	{
		callback(context, frame_segment_index, end_time - frame_start_time, frame_start, frame_size);
	}

	return VOD_OK;
}

uint32_t 
hls_muxer_simulate_get_segment_size(hls_muxer_state_t* state)
{
	hls_muxer_stream_state_t* selected_stream;
	input_frame_t* cur_frame;
	uint64_t cur_frame_dts;
#if (VOD_DEBUG)
	off_t cur_frame_start;
#endif

	mpegts_encoder_simulated_start_segment(&state->queue);

	for (;;)
	{
		// get a frame
		selected_stream = hls_muxer_choose_stream(state);
		if (selected_stream == NULL)
		{
			break;		// done
		}

		cur_frame = selected_stream->cur_frame;
		selected_stream->cur_frame++;
		selected_stream->next_frame_time_offset += cur_frame->duration;
		cur_frame_dts = selected_stream->next_frame_dts;
		selected_stream->next_frame_dts = rescale_time(selected_stream->next_frame_time_offset, selected_stream->timescale, HLS_TIMESCALE);

		// flush any buffered frames if their delay becomes too big
		hls_muxer_simulation_flush_delayed_streams(state, selected_stream, cur_frame_dts);

#if (VOD_DEBUG)
		cur_frame_start = state->queue.cur_offset;
#endif
		
		// write the frame
		hls_muxer_simulation_write_frame(selected_stream, cur_frame, cur_frame_dts, selected_stream->cur_frame >= selected_stream->last_frame);

#if (VOD_DEBUG)
		if (cur_frame_start != state->queue.cur_offset)
		{
			vod_log_debug4(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
				"hls_muxer_simulate_get_segment_size: wrote frame in packets %uD-%uD, dts %L, pid %ud",
				(uint32_t)(cur_frame_start / MPEGTS_PACKET_SIZE + 1),
				(uint32_t)(state->queue.cur_offset / MPEGTS_PACKET_SIZE + 1),
				cur_frame_dts, 
				selected_stream->mpegts_encoder_state.stream_info.pid);
		}
#endif
	}

	return state->queue.cur_offset;
}

void 
hls_muxer_simulation_reset(hls_muxer_state_t* state)
{
	hls_muxer_stream_state_t* cur_stream;

	mpegts_encoder_simulated_start_segment(&state->queue);

	for (cur_stream = state->first_stream; cur_stream < state->last_stream; cur_stream++)
	{
		cur_stream->cur_frame = cur_stream->first_frame;
		cur_stream->cur_frame_offset = cur_stream->first_frame_offset;
		cur_stream->next_frame_time_offset = cur_stream->first_frame_time_offset;
		cur_stream->next_frame_dts = rescale_time(cur_stream->next_frame_time_offset, cur_stream->timescale, HLS_TIMESCALE);
	}

	state->cur_frame = NULL;
}
