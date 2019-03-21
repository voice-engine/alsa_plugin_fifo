// The FIFO plugin is similar to the file plugin,
// while the FIFO plugin doesn't require a slave PCM device.
// It supports FIFO (named pipe) and normal files

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <alsa/pcm_plugin.h>

#define ARRAY_SIZE(ary) (sizeof(ary) / sizeof(ary[0]))

typedef struct _snd_pcm_fifo_t
{
	snd_pcm_ioplug_t io;
	int fd;
	int channels;
	int rate;
	snd_pcm_format_t format;
	unsigned int frame_bytes;
	volatile snd_pcm_sframes_t ptr;
} snd_pcm_fifo_t;

/*
 * Read data from fifo
 */
static void fifo_read(snd_pcm_ioplug_t *io)
{
	snd_pcm_fifo_t *fifo = io->private_data;
	int frame_bytes = fifo->frame_bytes;
	snd_pcm_uframes_t avail = io->appl_ptr - io->hw_ptr + io->buffer_size;

	while (avail > 0)
	{
		const snd_pcm_channel_area_t *areas = snd_pcm_ioplug_mmap_areas(io);
		unsigned int offset = fifo->ptr % io->buffer_size;
		unsigned int cont = io->buffer_size - offset;
		int frames = avail > cont ? cont : avail;

		char *dst = (char *)areas->addr + (areas->first + areas->step * offset) / 8;
		int result = read(fifo->fd, dst, frames * frame_bytes * io->channels);
		if (result > 0)
		{
			result /= (frame_bytes * io->channels);
			fifo->ptr += result;
			// avaoid overflow - fifo->ptr is a signed number
			if (fifo->ptr > (1 << (sizeof(snd_pcm_sframes_t) * 8 - 2)))
			{
				fifo->ptr %= io->buffer_size;
			}
		}

		// fprintf(stderr, "read: %d, %ld, %ld, %ld\n", result, fifo->ptr, io->appl_ptr, io->hw_ptr);
		if (result < frames)
		{
			break;
		}

		avail -= result;
	}
}

static void fifo_write(snd_pcm_ioplug_t *io)
{
	snd_pcm_fifo_t *fifo = io->private_data;
	int frame_bytes = fifo->frame_bytes;
	const snd_pcm_channel_area_t *areas = snd_pcm_ioplug_mmap_areas(io);
	snd_pcm_uframes_t avail = io->appl_ptr - fifo->ptr;

	while (avail > 0)
	{
		unsigned int offset = fifo->ptr % io->buffer_size;
		unsigned int cont = io->buffer_size - offset;
		int frames = avail > cont ? cont : avail;
		char *src = (char *)areas->addr + (areas->first + areas->step * offset) / 8;

		int result = write(fifo->fd, src, frames * frame_bytes * io->channels);
		if (result > 0)
		{
			result /= (frame_bytes * io->channels);
			fifo->ptr += result;
			// avaoid overflow - fifo->ptr is a signed number
			if (fifo->ptr > (1 << (sizeof(snd_pcm_sframes_t) * 8 - 2)))
			{
				fifo->ptr %= io->buffer_size;
			}
		}

		// fprintf(stderr, "write: %d, %ld, %ld, %ld\n", result, fifo->ptr, io->appl_ptr, io->hw_ptr);
		if (result < frames)
		{
			break;
		}

		avail -= result;
	}
}

/*
 * start and stop callbacks - just trigger pcm PCM
 */
static int fifo_start(snd_pcm_ioplug_t *io)
{
	snd_pcm_fifo_t *fifo = io->private_data;
	fifo->ptr = 0;
	return 0;
}

static int fifo_stop(snd_pcm_ioplug_t *io)
{
	return 0;
}

/*
 * close callback
 */
static int fifo_close(snd_pcm_ioplug_t *io)
{
	snd_pcm_fifo_t *fifo = io->private_data;
	close(fifo->fd);

	return 0;
}

static snd_pcm_sframes_t fifo_pointer(snd_pcm_ioplug_t *io)
{
	snd_pcm_fifo_t *fifo = io->private_data;

	return fifo->ptr;
}

static int fifo_read_poll_revents(snd_pcm_ioplug_t *io,
								  struct pollfd *pfds, unsigned int nfds,
								  unsigned short *revents)
{
	*revents = pfds[0].revents;
	fifo_read(io);
	return 0;
}

static int fifo_write_poll_revents(snd_pcm_ioplug_t *io,
								   struct pollfd *pfds, unsigned int nfds,
								   unsigned short *revents)
{
	*revents = pfds[0].revents;
	fifo_write(io);
	return 0;
}

/*
 * capture callback table
 */
static snd_pcm_ioplug_callback_t fifo_capture_callback = {
	.start = fifo_start,
	.stop = fifo_stop,
	.close = fifo_close,
	.pointer = fifo_pointer,
	.poll_revents = fifo_read_poll_revents};

/*
 * callback table
 */
static snd_pcm_ioplug_callback_t fifo_playback_callback = {
	.start = fifo_start,
	.stop = fifo_stop,
	.close = fifo_close,
	.pointer = fifo_pointer,
	.poll_revents = fifo_write_poll_revents};

static int fifo_hw_constraint(snd_pcm_fifo_t *fifo)
{
	unsigned int accesses[] = {
		SND_PCM_ACCESS_RW_INTERLEAVED,
		SND_PCM_ACCESS_MMAP_INTERLEAVED};
	unsigned int formats[] = {fifo->format};
	int err;

	if ((err = snd_pcm_ioplug_set_param_list(&fifo->io, SND_PCM_IOPLUG_HW_ACCESS,
											 ARRAY_SIZE(accesses), accesses)) < 0 ||
		(err = snd_pcm_ioplug_set_param_list(&fifo->io, SND_PCM_IOPLUG_HW_FORMAT,
											 ARRAY_SIZE(formats), formats)) < 0 ||
		(err = snd_pcm_ioplug_set_param_minmax(&fifo->io, SND_PCM_IOPLUG_HW_CHANNELS,
											   fifo->channels, fifo->channels)) < 0 ||
		(err = snd_pcm_ioplug_set_param_minmax(&fifo->io, SND_PCM_IOPLUG_HW_RATE,
											   fifo->rate, fifo->rate)) < 0)
	{
		SNDERR("ioplug cannot set params!");
		return err;
	}
	err = snd_pcm_ioplug_set_param_minmax(&fifo->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES,
										  256, 4 * 1024 * 1024);
	if (err < 0)
	{
		SNDERR("ioplug cannot set hw buffer bytes");
		return err;
	}

	err = snd_pcm_ioplug_set_param_minmax(&fifo->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
										  128, 2 * 1024 * 1024);
	if (err < 0)
	{
		SNDERR("ioplug cannot set hw period bytes");
		return err;
	}

	err = snd_pcm_ioplug_set_param_minmax(&fifo->io, SND_PCM_IOPLUG_HW_PERIODS, 3, 1024);
	if (err < 0)
	{
		SNDERR("ioplug cannot set hw periods");
		return err;
	}
	return 0;
}

/*
 * Main entry point
 */
SND_PCM_PLUGIN_DEFINE_FUNC(fifo)
{
	snd_config_iterator_t i, next;
	const char *file = NULL;
	const char *infile = NULL;
	snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;
	int rate = 16000;
	int channels = 1;
	snd_pcm_fifo_t *fifo;
	int fd;
	int err;

	if (stream != SND_PCM_STREAM_CAPTURE)
	{
		SNDERR("Warning!\nWhen using fifo plugin for playback, "
				"it may lose the last block of playback. "
				"\nPlease use file plugin instead");
	}

	snd_config_for_each(i, next, conf)
	{
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (strcmp(id, "file") == 0)
		{
			if (snd_config_get_string(n, &file) < 0)
			{
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "infile") == 0)
		{
			if (snd_config_get_string(n, &infile) < 0)
			{
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "rate") == 0)
		{
			long val;
			err = snd_config_get_integer(n, &val);
			if (err < 0)
			{
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			rate = val;
			continue;
		}
		if (strcmp(id, "format") == 0)
		{
			const char *str;
			err = snd_config_get_string(n, &str);
			if (err < 0)
			{
				SNDERR("invalid type for %s", id);
				return -EINVAL;
			}
			format = snd_pcm_format_value(str);
			continue;
		}
		if (strcmp(id, "channels") == 0)
		{
			long val;
			err = snd_config_get_integer(n, &val);
			if (err < 0)
			{
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			channels = val;
			continue;
		}
	}

	if (stream == SND_PCM_STREAM_PLAYBACK)
	{
		if (!file)
		{
			SNDERR("for playback, file is not set");
			return -EINVAL;
		}
		fd = open(file, O_RDWR | O_NONBLOCK);
	}
	else
	{
		if (!infile)
		{
			SNDERR("for capturing. infile is not set");
			return -EINVAL;
		}
		fd = open(infile, O_RDWR | O_NONBLOCK);
	}

	if (fd < 0)
	{
		SNDERR("can not open file");
		return -EINVAL;
	}

	fifo = calloc(1, sizeof(*fifo));
	if (!fifo)
	{
		SNDERR("cannot allocate");
		return -ENOMEM;
	}

	fifo->fd = fd;
	fifo->channels = channels;
	fifo->rate = rate;
	fifo->format = format;
	fifo->frame_bytes = snd_pcm_format_width(format) / 8;

	fifo->io.version = SND_PCM_IOPLUG_VERSION;
	fifo->io.name = "ALSA <-> FIFO (Named Pipe) Plugin";
	fifo->io.mmap_rw = 1;
	fifo->io.poll_fd = fifo->fd;
	fifo->io.poll_events = stream == SND_PCM_STREAM_PLAYBACK ? POLLOUT : POLLIN;
	fifo->io.callback = stream == SND_PCM_STREAM_PLAYBACK ? &fifo_playback_callback : &fifo_capture_callback;
	fifo->io.private_data = fifo;

	err = snd_pcm_ioplug_create(&fifo->io, name, stream, mode);
	if (err < 0)
		goto fail;

	if ((err = fifo_hw_constraint(fifo)) < 0)
	{
		snd_pcm_ioplug_delete(&fifo->io);
		goto fail;
	}
	*pcmp = fifo->io.pcm;

	return 0;
fail:
	free(fifo);
	return err;
}

SND_PCM_PLUGIN_SYMBOL(fifo);
