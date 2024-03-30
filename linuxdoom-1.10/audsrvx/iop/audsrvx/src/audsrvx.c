/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright 2005, ps2dev - http://www.ps2dev.org
# Licenced under GNU Library General Public License version 2
*/

/**
 * @file
 * audsrv IOP server
 */

#include <stdio.h>
#include <thbase.h>
#include <thsemap.h>
#include <loadcore.h>
#include <sysmem.h>
#include <intrman.h>
#include <sifcmd.h>
#include <libsd.h>
#include <sysclib.h>

#include "audsrvx.h"
#include "audsrvx_internal.h"
#include "common.h"
#include "rpc_server.h"
#include "rpc_client.h"
#include "upsamplers.h"
#include "hw.h"
#include "spu.h"

#define MODNAME "audsrv"
#define VERSION "0.93"
IRX_ID(MODNAME, 1, 4);

/* globals */
/** core1 (sfx) volume */
static int core1_volume = MAX_VOLUME;
/** frequency set by user */
static int core1_freq = 0;
/** bits per sample, set by user */
static int core1_bits = 0;
/** number of audio channels */
static int core1_channels = 0;
/** shift count from bytes to samples */
static int core1_sample_shift = 0;

/* status */
/** initialization status */
static int initialized = 0;
/** playing (not mute) status */
static int playing = 0;


/* ring buffer properties */
/** ring buffer itself */
static char ringbuf[20480];
/** size of ring buffer in bytes */
static int ringbuf_size = sizeof(ringbuf);
/** reading head pointer */
static int readpos;
/** writing head pointer */
static int writepos;

/** playing thread id */
static int play_tid = 0;
/** semaphore for wait_audio() */
static int queue_sema = 0;
/** SPU2 transfer complete semaphore */
static int transfer_sema = 0;
/** threshold to initiate a callback */
static int fillbuf_threshold = 0;

/** boolean to notify when format has changed */
static int format_changed = 0;

/** double buffer for streaming */
static u8 core1_buf[0x1000] __attribute__((aligned (16)));

static short rendered_left [ 512 ];
static short rendered_right[ 512 ];

static char *buffer1;
static char *buffer2;
static u32 buffer_in_use = 0;
static u32 buffer1_max_size = 0;
static u32 buffer2_max_size = 0;
static u32 buffer1_data_size = 0;
static u32 buffer2_data_size = 0;
static u32 buffer1_has_data = 0;
static u32 buffer2_has_data = 0;
static u32 buffer_reset = 0;


enum 
{
	BUFFER1 = 1,
	BUFFER2 = 2,
};

/** exports table */
extern struct irx_export_table _exp_audsrv;

/* forward declarations */
static void play_thread(void *arg);

/** Transfer complete callback.
 * @param arg     not used
 * @returns true, always
 *
 * Generated by SPU2, when a block was transmitted and now putting
 * to upload a second block to the other buffer.
 */
static int transfer_complete(void *arg)
{
	(void)arg;

	iSignalSema(transfer_sema);
	return 1;
}

/** Apply volume changes, or keep mute if not playing */
static void update_volume()
{
	int vol;

	/* external input */
	sceSdSetParam(SD_CORE_1 | SD_PARAM_AVOLL, 0x7fff);
	sceSdSetParam(SD_CORE_1 | SD_PARAM_AVOLR, 0x7fff);

	/* core0 input */
	sceSdSetParam(SD_CORE_0 | SD_PARAM_BVOLL, 0);
	sceSdSetParam(SD_CORE_0 | SD_PARAM_BVOLR, 0);

	/* core1 input */
	vol = playing ? core1_volume : 0;
	sceSdSetParam(SD_CORE_1 | SD_PARAM_BVOLL, vol);
	sceSdSetParam(SD_CORE_1 | SD_PARAM_BVOLR, vol);

	/* set master volume for core 0 */
	sceSdSetParam(SD_CORE_0 | SD_PARAM_MVOLL, 0);
	sceSdSetParam(SD_CORE_0 | SD_PARAM_MVOLR, 0);

	/* set master volume for core 1 */
	sceSdSetParam(SD_CORE_1 | SD_PARAM_MVOLL, MAX_VOLUME);
	sceSdSetParam(SD_CORE_1 | SD_PARAM_MVOLR, MAX_VOLUME);
}


/** Set audio double buffer
 * @returns 0, always
 *
 * 
 */
int audsrv_set_buffers(char *ptr1, char *ptr2, 
					   u32 size1, u32 size2)
{
	buffer1 = ptr1;
	buffer2 = ptr2;
	buffer1_max_size = size1;
	buffer2_max_size = size2;
	printf("set buffers and size %d %d\n", buffer1_max_size, buffer2_max_size);
	return AUDSRV_ERR_NOERROR;
}


int audsrv_transfer_notify(int buffer, int size)
{
	
	if (buffer == BUFFER1)
	{
		buffer1_has_data = 1;
		buffer1_data_size = size;
	} 
	else if (buffer == BUFFER2)
	{
		buffer2_has_data = 1;
		buffer2_data_size = size;
	}
	else 
	{
		return AUDSRV_ERR_ARGS;
	}
		
	if (playing == 0)
	{
		/* audio is always playing, just change the volume */
		playing = 1;
		update_volume();
	}
	//printf("buffers are full %d %d\n", buffer1_has_data, buffer2_has_data);
	return AUDSRV_ERR_NOERROR;
}

int audsrv_buffers_status()
{
	int ret = 0;
	
	if (buffer1_has_data)
		ret |= 0x01;
	if (buffer2_has_data)
		ret |= 0x02;

	//printf("check buffers %d\n", ret);
	return ret;
}

int audsrv_reset_buffers()
{
	buffer1_has_data = buffer2_has_data = 0;
	buffer1_data_size = buffer2_data_size = 0;
	buffer_reset = 1;
	playing = 0;
	return AUDSRV_ERR_NOERROR;
}

/** Stops all audio playing
 * @returns 0, always
 *
 * Mutes output and stops accepting audio blocks; also, clears callbacks.
 */
int audsrv_stop_audio()
{
	/* audio is still playing, just mute */
	playing = 0;
	update_volume();
	fillbuf_threshold = 0;

	return AUDSRV_ERR_NOERROR;
}

/** Checks if the format noted by frequency and depth is supported
 *
 * @param freq     frequency in hz
 * @param bits     bits per sample (8, 16)
 * @param channels channels
 * @returns positive on success, zero value on failure
 */
int audsrv_format_ok(int freq, int bits, int channels)
{
	if (find_upsampler(freq, bits, channels) != NULL)
	{
		return 1;
	}

	/* unsupported format */
	return 0;
}

/** Configures audio stream
 * @param freq     frequency in hz
 * @param bits     bits per sample (8, 16)
 * @param channels number of channels
 * @returns 0 on success, negative if configuration is not supported
 *
 * This sets up audsrv to accept stream in this format and convert
 * it to SPU2's native format if required. Note: it is possible to
 * change the format at any point. You might want to stop audio prior
 * to that, to prevent mismatched audio output.
 */
int audsrv_set_format(int freq, int bits, int channels)
{
	int feed_size;

	if (audsrv_format_ok(freq, bits, channels) == 0)
	{
		return -AUDSRV_ERR_FORMAT_NOT_SUPPORTED;
	}

	/* update shift-right count */
	core1_sample_shift = 0;
	if (bits == 16)
	{
		core1_sample_shift++;
	}

	if (channels == 2)
	{
		core1_sample_shift++;
	}

	core1_freq = freq;
	core1_bits = bits;
	core1_channels = channels;

	/* set ring buffer size to 10 iterations worth of data (~50 ms) */
	feed_size = ((512 * core1_freq) / 48000) << core1_sample_shift;
	ringbuf_size = feed_size * 10;

	writepos = 0;
	readpos = (feed_size * 5) & ~3;

	printf("audsrv: freq %d bits %d channels %d ringbuf_sz %d feed_size %d shift %d\n", freq, bits, channels, ringbuf_size, feed_size, core1_sample_shift);

	format_changed = 1;
	return AUDSRV_ERR_NOERROR;
}

/** Initializes audsrv library
 * @returns 0 on success
 */
int audsrv_init()
{
	if (initialized)
	{
		return 0;
	}

	/* initialize libsd */
	if (sceSdInit(SD_INIT_COLD) < 0)
	{
		printf("audsrv: failed to initialize libsd\n");
		return -1;
	}

	readpos = 0;
	writepos = 0;

	/* initialize transfer_complete's semaphore */
	transfer_sema = CreateMutex(0);
	if (transfer_sema < 0)
	{
		return AUDSRV_ERR_OUT_OF_MEMORY;
	}

	queue_sema = CreateMutex(0);
	if (queue_sema < 0)
	{
		DeleteSema(transfer_sema);
		return AUDSRV_ERR_OUT_OF_MEMORY;
	}

	/* audio is always playing in the background. trick is to
	 * set the data input volume to zero
	 */
	audsrv_stop_audio();

	/* initialize transfer-complete callback */
	sceSdSetTransCallback(AUDSRV_BLOCK_DMA_CH, (void *)transfer_complete);
	sceSdBlockTrans(AUDSRV_BLOCK_DMA_CH, SD_TRANS_LOOP, core1_buf, sizeof(core1_buf), 0);

	/* default to SPU's native */
	audsrv_set_format(48000, 16, 2);

	play_tid = create_thread(play_thread, 39, 0);
	printf("audsrv: playing thread 0x%x started\n", play_tid);

	printf("audsrv: kickstarted\n");

	initialized = 1;
	return AUDSRV_ERR_NOERROR;
}

/** Returns the number of bytes that can be queued
 * @returns byte count

 * Returns the number of bytes that are available in the ring buffer. This
 * is the total bytes that can be queued, without collision of the reading
 * head with the writing head.
 */
int audsrv_available()
{
	if (writepos <= readpos)
	{
		return readpos - writepos;
	}
	else
	{
		return (ringbuf_size - (writepos - readpos));
	}
}

/** Returns the number of bytes already in queue
 * @returns byte count

 * Returns the number of bytes that are already in the ring buffer.
 */
int audsrv_queued()
{
	if (writepos < readpos)
	{
		return (ringbuf_size - (readpos - writepos));
	}
	else
	{
		return writepos - readpos;
	}
}

/** Blocks until there is enough space to enqueue chunk
 * @param buflen   size of chunk requested to be enqueued (in bytes)
 * @returns error status code
 *
 * Blocks until there are enough space to store the upcoming chunk
 * in audsrv's internal ring buffer.
 */
int audsrv_wait_audio(int buflen)
{
	if (ringbuf_size < buflen)
	{
		/* this will never happen */
		return AUDSRV_ERR_ARGS;
	}

	while (1)
	{
		if (audsrv_available() >= buflen)
		{
			/* enough space! */
			return AUDSRV_ERR_NOERROR;
		}

		WaitSema(queue_sema);
	}
}

/** Uploads audio buffer to SPU
 * @param buf     audio chunk
 * @param buflen  size of chunk in bytes
 * @returns positive number of bytes sent to processor or negative error status
 *
 * Plays an audio buffer; It will not interrupt a playing
 * buffer, rather queue it up and play it as soon as possible without
 * interfering with fluent streaming. The buffer and buflen are given
 * in host format (i.e, 11025hz 8bit stereo.)
 */
int audsrv_play_audio(const char *buf, int buflen)
{
	int sent = 0;

	if (initialized == 0)
	{
		return -AUDSRV_ERR_NOT_INITIALIZED;
	}

	if (playing == 0)
	{
		/* audio is always playing, just change the volume */
		playing = 1;
		update_volume();
	}

	//printf("play audio %d bytes, readpos %d, writepos %d avail %d\n", buflen, readpos, writepos, audsrv_available());

	/* limit to what's available, no crossing possible */
	buflen = MIN(buflen, audsrv_available());

	while (buflen > 0)
	{
		int copy = buflen;
		if (writepos >= readpos)
		{
			copy = MIN(ringbuf_size - writepos, buflen);
		}

		memcpy(ringbuf + writepos, buf, copy);
		buf = buf + copy;
		buflen = buflen - copy;
		sent = sent + copy;

		writepos = writepos + copy;
		if (writepos >= ringbuf_size)
		{
			/* rewind */
			writepos = 0;
		}
	}

	return sent;
}

/** Sets output volume
 * @param vol      volume in SPU2 units [0 .. 0x3fff]
 * @returns 0 on success, negative otherwise
 */
int audsrv_set_volume(int vol)
{
	if (vol < 0 || vol > MAX_VOLUME)
	{
		/* bad joke */
		return AUDSRV_ERR_ARGS;
	}

	core1_volume = vol;
	update_volume();
	return AUDSRV_ERR_NOERROR;
}

int audsrv_set_threshold(int amount)
{
	if (amount > (ringbuf_size / 2))
	{
		/* amount is greater than what we'd recommend */
		return AUDSRV_ERR_ARGS;
	}

	printf("audsrv: callback threshold: %d\n", amount);
	fillbuf_threshold = amount;
	return 0;
}

/** Main playing thread
 * @param arg   not used
 *
 * This is the main playing thread. It feeds the SPU with upsampled, demux'd
 * audio data, from what has been queued beforehand. The stream is
 * constructed as a ring buffer. This thread only ends with TerminateThread,
 * and is usually asleep, waiting for SPU to complete playing the current
 * wave. SPU plays 2048 bytes blocks, which yields that this thread wakes
 * and sleeps 93.75 times a second
 */
static void play_thread(void *arg)
{
	int intr_state;
	int step;
	struct upsample_t up;
	upsampler_t upsampler = NULL;
	char *source = (char *)buffer1;
	u32 *buffer_size = &buffer1_data_size;

	(void)arg;

	printf("starting play thread\n");
	while (1)
	{
		int block;
		u8 *bufptr;
	//	int available;

		if (buffer_reset)
		{
			buffer_reset = 0;
			buffer_in_use = 0;
			source = (char *)buffer1;
			buffer_size = &buffer1_data_size;
			readpos = 0;
		}

		if (format_changed)
		{
			upsampler = find_upsampler(core1_freq, core1_bits, core1_channels);
			format_changed = 0;
		}
		//printf("here doing whatever %d\n", playing);
		if (playing && upsampler != NULL)
		{
			//printf("%d %d %d\n", readpos, *bufferSize, buffer_in_use);
			up.src = (const unsigned char *)source + readpos;
			up.left = rendered_left;
			up.right = rendered_right;
			step = upsampler(&up);
			
			readpos = readpos + step;

			
			if (readpos >= *buffer_size)
			{
				readpos = 0;
				if (buffer_in_use)
				{
					buffer_in_use = 0;
					source = (char *)buffer1;
					buffer_size = &buffer1_data_size;
					buffer2_has_data = 0;
				} else {
					buffer_in_use = 1;
					source = (char *)buffer2;
					buffer_size = &buffer2_data_size;
					buffer1_has_data = 0;
				} 

			}
		}
		else
		{
			/* not playing */
			memset(rendered_left, '\0', sizeof(rendered_left));
			memset(rendered_right, '\0', sizeof(rendered_right));
		}

		/* wait until it's safe to transmit another block */
		WaitSema(transfer_sema);

		/* suspend all interrupts */
		CpuSuspendIntr(&intr_state);

		/* one block is playing currently, other is idle */
		block = 1 - (sceSdBlockTransStatus(AUDSRV_BLOCK_DMA_CH, 0) >> 24);

		/* copy 1024 bytes from left and right buffers, into core1_buf */
		bufptr = core1_buf + (block << 11);
		wmemcpy(bufptr +    0, rendered_left + 0, 512);
		wmemcpy(bufptr +  512, rendered_right + 0, 512);
		wmemcpy(bufptr + 1024, rendered_left + 256, 512);
		wmemcpy(bufptr + 1536, rendered_right + 256, 512);

		CpuResumeIntr(intr_state);

		//available = audsrv_available();
		//if (available >= (ringbuf_size / 10))
		//{
			/* arbitrarily selected ringbuf_size / 10, to reduce
			 * number of semaphores signalled.
			 */
			//SignalSema(queue_sema);
		//}

		//if (fillbuf_threshold > 0 && available >= fillbuf_threshold)
		//{
			/* EE client requested a callback */
		//	call_client_callback(AUDSRV_FILLBUF_CALLBACK);
		//}

		//printf("avaiable: %d, queued: %d\n", available, ringbuf_size - available);
	}
}

/** Shutdowns audsrv
 * @returns AUDSRV_ERR_NOERROR
 */
int audsrv_quit()
{
	/* silence! */
	audsrv_stop_audio();
	audsrv_stop_cd();
	audsrv_adpcm_init();

	/* stop transmission */
	sceSdSetTransCallback(AUDSRV_BLOCK_DMA_CH, NULL);
	sceSdBlockTrans(AUDSRV_BLOCK_DMA_CH, SD_TRANS_STOP, 0, 0, 0);

	/* stop playing thread */
	if (play_tid > 0)
	{
		TerminateThread(play_tid);
		DeleteThread(play_tid);
		play_tid = 0;
	}

#ifndef NO_RPC_THREAD
	/* Deinitialize RPC client, only once the playback thread is stopped. */
	deinitialize_rpc_client();
#endif

	if (transfer_sema > 0)
	{
		DeleteSema(transfer_sema);
		transfer_sema = 0;
	}

	if (queue_sema > 0)
	{
		DeleteSema(queue_sema);
		queue_sema = 0;
	}

	return 0;
}

/** IRX _start function
 * @returns zero on success
 *
 * Initializes interrupts and the RPC thread. Oh, and greets the user.
 */
int _start(int argc, char *argv[])
{
	int err;

	(void)argc;
	(void)argv;

	FlushDcache();
	CpuEnableIntr(0);

	printf("audsrv: greetings from version " VERSION " !\n");

	err = RegisterLibraryEntries(&_exp_audsrv);
	if (err != 0)
	{
		printf("audsrv: couldn't register library entries. Error %d\n", err);
		return MODULE_NO_RESIDENT_END;
	}

	audsrv_adpcm_init();

	/* create RPC listener thread */
	initialize_rpc_thread();

	return MODULE_RESIDENT_END;
}
