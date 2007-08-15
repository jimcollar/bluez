/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2007  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

#include <netinet/in.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sco.h>

#include "ipc.h"
#include "sbc.h"

//#define ENABLE_DEBUG

#define BUFFER_SIZE 2048

#ifdef ENABLE_DEBUG
#define DBG(fmt, arg...)  printf("DEBUG: %s: " fmt "\n" , __FUNCTION__ , ## arg)
#else
#define DBG(fmt, arg...)
#endif

#ifndef SCO_TXBUFS
#define SCO_TXBUFS 0x03
#endif

#ifndef SCO_RXBUFS
#define SCO_RXBUFS 0x04
#endif

struct rtp_header {
	uint8_t cc:4;
	uint8_t x:1;
	uint8_t p:1;
	uint8_t v:2;

	uint8_t pt:7;
	uint8_t m:1;

	uint16_t sequence_number;
	uint32_t timestamp;
	uint32_t ssrc;
	uint32_t csrc[0];
} __attribute__ ((packed));

struct rtp_payload {
	uint8_t frame_count:4;
	uint8_t rfa0:1;
	uint8_t is_last_fragment:1;
	uint8_t is_first_fragment:1;
	uint8_t is_fragmented:1;
} __attribute__ ((packed));

struct bluetooth_a2dp {
	sbc_t sbc;			/* Codec data */
	int samples;			/* Number of encoded samples */
	time_t timestamp;		/* Codec samples timestamp */
	uint8_t buffer[BUFFER_SIZE];	/* Codec transfer buffer */
	int count;			/* Codec transfer buffer counter */

	int nsamples;			/* Cumulative number of codec samples */
	struct timeval ntimestamp;	/* Cumulative timeval */
	uint16_t seq_num;		/* */
	int frame_count;		/* */

	int bandwithcount;
	struct timeval bandwithtimestamp;
};

struct bluetooth_data {
	snd_pcm_ioplug_t io;
	snd_pcm_sframes_t hw_ptr;
	struct ipc_data_cfg cfg;	/* Bluetooth device config */
	int stream_fd;			/* Audio stream filedescriptor */
	int sock;			/* Daemon unix socket */
	uint8_t buffer[BUFFER_SIZE];	/* Encoded transfer buffer */
	int count;			/* Transfer buffer counter */
	struct bluetooth_a2dp a2dp;	/* a2dp data */
};

void memcpy_changeendian(void *dst, const void *src, int size)
{
	int i;
	const uint16_t *ptrsrc = src;
	uint16_t *ptrdst = dst;
	for (i = 0; i < size / 2; i++) {
		*ptrdst++ = htons(*ptrsrc++);
	}
}

static int bluetooth_start(snd_pcm_ioplug_t *io)
{
	DBG("bluetooth_start %p", io);

	return 0;
}

static int bluetooth_stop(snd_pcm_ioplug_t *io)
{
	DBG("bluetooth_stop %p", io);

	return 0;
}

static snd_pcm_sframes_t bluetooth_pointer(snd_pcm_ioplug_t *io)
{
	struct bluetooth_data *data = io->private_data;

#if 0
	DBG("bluetooth_pointer %p, hw_ptr=%lu", io, data->hw_ptr);
#endif

	return data->hw_ptr;
}

static void bluetooth_exit(struct bluetooth_data *data)
{
	if (data->sock >= 0)
		close(data->sock);

	if (data->stream_fd >= 0)
		close(data->stream_fd);

	if (data->cfg.codec == CFG_CODEC_SBC)
		sbc_finish(&data->a2dp.sbc);

	free(data);
}

static int bluetooth_close(snd_pcm_ioplug_t *io)
{
	struct bluetooth_data *data = io->private_data;

	DBG("%p", io);

	bluetooth_exit(data);

	return 0;
}

static int bluetooth_prepare(snd_pcm_ioplug_t *io)
{
	struct bluetooth_data *data = io->private_data;

	DBG("Preparing with io->period_size = %lu, io->buffer_size = %lu",
			io->period_size, io->buffer_size);

	if (io->stream == SND_PCM_STREAM_PLAYBACK)
		/* If not null for playback, xmms doesn't display time
		 * correctly */
		data->hw_ptr = 0;
	else
		/* ALSA library is really picky on the fact hw_ptr is not null.
		 * If it is, capture won't start */
		data->hw_ptr = io->period_size;

	return 0;
}

static int bluetooth_hsp_hw_params(snd_pcm_ioplug_t *io,
					snd_pcm_hw_params_t *params)
{
	struct bluetooth_data *data = io->private_data;
	uint32_t period_count = io->buffer_size / io->period_size;
	int opt_name, err;

	DBG("fd = %d, period_count = %d", data->stream_fd, period_count);

	opt_name = (io->stream == SND_PCM_STREAM_PLAYBACK) ?
			SCO_TXBUFS : SCO_RXBUFS;

	if (setsockopt(data->stream_fd, SOL_SCO, opt_name, &period_count,
			sizeof(period_count)) == 0)
		return 0;

	opt_name = (io->stream == SND_PCM_STREAM_PLAYBACK) ?
			SO_SNDBUF : SO_RCVBUF;

	if (setsockopt(data->stream_fd, SOL_SCO, opt_name, &period_count,
			sizeof(period_count)) == 0)
		return 0;

	err = errno;
	SNDERR("%s (%d)", strerror(err), err);

	return -err;
}

static int bluetooth_a2dp_hw_params(snd_pcm_ioplug_t *io,
					snd_pcm_hw_params_t *params)
{
	struct bluetooth_data *data = io->private_data;
	uint32_t period_count = io->buffer_size / io->period_size;
	int opt_name, err;
	struct timeval t = { 0, period_count };

	DBG("fd = %d, period_count = %d", data->stream_fd, period_count);

	opt_name = (io->stream == SND_PCM_STREAM_PLAYBACK) ?
			SO_SNDTIMEO : SO_RCVTIMEO;

	if (setsockopt(data->stream_fd, SOL_SOCKET, opt_name, &t,
			sizeof(t)) == 0)
		return 0;

	err = errno;
	SNDERR("%s (%d)", strerror(err), err);

	return -err;
}

static snd_pcm_sframes_t bluetooth_hsp_read(snd_pcm_ioplug_t *io,
						const snd_pcm_channel_area_t *areas,
						snd_pcm_uframes_t offset,
						snd_pcm_uframes_t size)
{
	struct bluetooth_data *data = io->private_data;
	struct ipc_data_cfg cfg = data->cfg;
	snd_pcm_uframes_t frames_to_write, ret;
	unsigned char *buff;
	int nrecv, frame_size = 0;

	DBG("areas->step=%u, areas->first=%u, offset=%lu, size=%lu,"
		"io->nonblock=%u", areas->step, areas->first, offset, size,
		io->nonblock);

	if (data->count > 0)
		goto proceed;

	frame_size = areas->step / 8;

	nrecv = recv(data->stream_fd, data->buffer, cfg.pkt_len,
			MSG_WAITALL | (io->nonblock ? MSG_DONTWAIT : 0));

	if (nrecv < 0) {
		ret = (errno == EPIPE) ? -EIO : -errno;
		goto done;
	}

	if (nrecv != cfg.pkt_len) {
		ret = -EIO;
		SNDERR(strerror(-ret));
		goto done;
	}

	/* Increment hardware transmition pointer */
	data->hw_ptr = (data->hw_ptr + cfg.pkt_len / cfg.sample_size) %
			io->buffer_size;

proceed:
	buff = (unsigned char *) areas->addr +
			(areas->first + areas->step * offset) / 8;

	if ((data->count + size * frame_size) <= cfg.pkt_len)
		frames_to_write = size;
	else
		frames_to_write = (cfg.pkt_len - data->count) / frame_size;

	memcpy(buff, data->buffer + data->count, frame_size * frames_to_write);
	data->count += (frame_size * frames_to_write);
	data->count %= cfg.pkt_len;

	/* Return written frames count */
	ret = frames_to_write;

done:
	DBG("returning %lu", ret);
	return ret;
}

static snd_pcm_sframes_t bluetooth_hsp_write(snd_pcm_ioplug_t *io,
						const snd_pcm_channel_area_t *areas,
						snd_pcm_uframes_t offset,
						snd_pcm_uframes_t size)
{
	struct bluetooth_data *data = io->private_data;
	struct ipc_data_cfg cfg = data->cfg;
	snd_pcm_sframes_t ret = 0;
	snd_pcm_uframes_t frames_to_read;
	uint8_t *buff;
	int rsend, frame_size;

	DBG("areas->step=%u, areas->first=%u, offset=%lu, size=%lu,"
			"io->nonblock=%u", areas->step, areas->first,
			offset, size, io->nonblock);

	frame_size = areas->step / 8;
	if ((data->count + size * frame_size) <= cfg.pkt_len)
		frames_to_read = size;
	else
		frames_to_read = (cfg.pkt_len - data->count) / frame_size;

	DBG("count = %d, frames_to_read = %lu", data->count, frames_to_read);

	/* Ready for more data */
	buff = (uint8_t *) areas->addr +
			(areas->first + areas->step * offset) / 8;
	memcpy(data->buffer + data->count, buff, frame_size * frames_to_read);

	/* Remember we have some frames in the pipe now */
	data->count += frames_to_read * frame_size;
	if (data->count != cfg.pkt_len) {
		ret = frames_to_read;
		goto done;
	}

	rsend = send(data->stream_fd, data->buffer, cfg.pkt_len,
			io->nonblock ? MSG_DONTWAIT : 0);
	if (rsend > 0) {
		/* Reset count pointer */
		data->count = 0;

		/* Increment hardware transmition pointer */
		data->hw_ptr = (data->hw_ptr + cfg.pkt_len / frame_size)
				% io->buffer_size;

		ret = frames_to_read;
	} else if (rsend < 0)
		ret = (errno == EPIPE) ? -EIO : -errno;
	else
		ret = -EIO;

done:
	DBG("returning %lu", ret);
	return ret;
}

static snd_pcm_sframes_t bluetooth_a2dp_read(snd_pcm_ioplug_t *io,
						const snd_pcm_channel_area_t *areas,
						snd_pcm_uframes_t offset,
						snd_pcm_uframes_t size)
{
	snd_pcm_uframes_t ret = 0;
	return ret;
}

static int avdtp_write(struct bluetooth_data *data, unsigned int nonblock)
{
	int count = 0;
	int written;
	struct rtp_header *header;
	struct rtp_payload *payload;
	struct bluetooth_a2dp *a2dp = &data->a2dp;
#ifdef ENABLE_DEBUG
	static struct timeval send_date = { 0, 0 };
	static struct timeval prev_date = { 0, 0 };
	struct timeval send_delay = { 0, 0 };
	struct timeval sendz_delay = { 0, 0 };
#endif

	header = (void *) a2dp->buffer;
	payload = (void *) (a2dp->buffer + sizeof(*header));

	memset(a2dp->buffer, 0, sizeof(*header) + sizeof(*payload));

	payload->frame_count = a2dp->frame_count;
	header->v = 2;
	header->pt = 1;
	header->sequence_number = htons(a2dp->seq_num);
	header->timestamp = htonl(a2dp->nsamples);
	header->ssrc = htonl(1);

	while (count++ < 10) {
#ifdef ENABLE_DEBUG
		gettimeofday(&send_date, NULL);
#endif
		written = send(data->stream_fd, a2dp->buffer, a2dp->count,
				nonblock ? MSG_DONTWAIT : 0);

#ifdef ENABLE_DEBUG
		if ((written >= 0 || errno == EAGAIN) && prev_date.tv_sec != 0) {
			long delay, real, theo, delta;

			delay = (long) (send_delay.tv_sec * 1000 +
						send_delay.tv_usec / 1000),
			real = (long) (sendz_delay.tv_sec * 1000 +
						sendz_delay.tv_usec / 1000);
			theo = (long) (((float) a2dp->nsamples) /
						data->cfg.rate * 1000.0);
			delta = (long) (sendz_delay.tv_sec * 1000 +
						sendz_delay.tv_usec / 1000) -
					(long) (((float) a2dp->nsamples) /
							data->cfg.rate * 1000.0);

			timersub(&send_date, &prev_date, &send_delay);
			timersub(&send_date, &a2dp->ntimestamp, &sendz_delay);

			printf("send %d (cumul=%d) samples (delay=%ld ms,"
					" real=%ld ms, theo=%ld ms,"
					" delta=%ld ms).\n", a2dp->samples,
					a2dp->nsamples, delay, real, theo,
					delta);
		}
#endif
		if (written == a2dp->count)
			break;

		a2dp->count -= written;

		DBG("send (retry).");
		usleep(150000);
	}

#ifdef ENABLE_DEBUG
	prev_date = send_date;
#endif

	/* Send our data */
	if (written != a2dp->count)
		printf("Wrote %d not %d bytes\n", written, a2dp->count);
#ifdef ENABLE_DEBUG
	else {
		/* Measure bandwith usage */
		struct timeval now = { 0, 0 };
		struct timeval interval = { 0, 0 };

		if(a2dp->bandwithtimestamp.tv_sec == 0)
			gettimeofday(&a2dp->bandwithtimestamp, NULL);

		/* See if we must wait again */
		gettimeofday(&now, NULL);
		timersub(&now, &a2dp->bandwithtimestamp, &interval);
		if(interval.tv_sec > 0)
			printf("Bandwith: %d (%d kbps)\n", a2dp->bandwithcount,
				a2dp->bandwithcount / 128);
		a2dp->bandwithtimestamp = now;
		a2dp->bandwithcount = 0;
	}

	a2dp->bandwithcount += written;

#endif
	/* Reset buffer of data to send */
	a2dp->count = sizeof(struct rtp_header) + sizeof(struct rtp_payload);
	a2dp->frame_count = 0;
	a2dp->samples = 0;
	a2dp->seq_num++;

	return written;
}

static snd_pcm_sframes_t bluetooth_a2dp_write(snd_pcm_ioplug_t *io,
						const snd_pcm_channel_area_t *areas,
						snd_pcm_uframes_t offset,
						snd_pcm_uframes_t size)
{
	struct bluetooth_data *data = io->private_data;
	struct bluetooth_a2dp *a2dp = &data->a2dp;
	snd_pcm_sframes_t ret = 0;
	snd_pcm_uframes_t frames_to_read;
	int frame_size, encoded;
	uint8_t *buff;
	static int codesize = 0;

	DBG("areas->step=%u, areas->first=%u, offset=%lu, size=%lu,"
			"io->nonblock=%u", areas->step, areas->first,
			offset, size, io->nonblock);

	if (codesize == 0) {
		/* How much data can be encoded by sbc at a time? */
		codesize = a2dp->sbc.subbands * a2dp->sbc.blocks *
				a2dp->sbc.channels * 2;
		/* Reserv header space in outgoing buffer */
		a2dp->count = sizeof(struct rtp_header) +
				sizeof(struct rtp_payload);
		gettimeofday(&a2dp->ntimestamp, NULL);
	}

	frame_size = areas->step / 8;
	if ((data->count + size * frame_size) <= codesize)
		frames_to_read = size;
	else
		frames_to_read = (codesize - data->count) / frame_size;

	DBG("count = %d, frames_to_read = %lu", data->count, frames_to_read);
	DBG("a2dp.count = %d cfg.pkt_len = %d", a2dp->count,
			data->cfg.pkt_len);

	/* FIXME: If state is not streaming then return */

	/* Ready for more data */
	buff = (uint8_t *) areas->addr +
		(areas->first + areas->step * offset) / 8;
	memcpy_changeendian(data->buffer + data->count, buff,
				frame_size * frames_to_read);

	/* Remember we have some frames in the pipe now */
	data->count += frames_to_read * frame_size;
	if (data->count != codesize) {
		ret = frames_to_read;
		goto done;
	}

	/* Enough data to encode (sbc wants 1k blocks) */
	encoded = sbc_encode(&(a2dp->sbc), data->buffer, codesize);
	if (encoded <= 0) {
		DBG("Encoding error %d", encoded);
		goto done;
	}

	data->count -= encoded;

	DBG("encoded = %d  a2dp.sbc.len= %d", encoded, a2dp->sbc.len);

	if (a2dp->count + a2dp->sbc.len >= data->cfg.pkt_len)
		avdtp_write(data, io->nonblock);

	memcpy(a2dp->buffer + a2dp->count, a2dp->sbc.data, a2dp->sbc.len);
	a2dp->count += a2dp->sbc.len;
	a2dp->frame_count++;
	a2dp->samples += encoded / frame_size;
	a2dp->nsamples += encoded / frame_size;
	/* Increment hardware transmition pointer */
	data->hw_ptr = (data->hw_ptr + encoded / frame_size)
			% io->buffer_size;

	ret = frames_to_read;

done:
	DBG("returning %lu", ret);
	return ret;
}

static snd_pcm_ioplug_callback_t bluetooth_hsp_playback = {
	.start		= bluetooth_start,
	.stop		= bluetooth_stop,
	.pointer	= bluetooth_pointer,
	.close		= bluetooth_close,
	.hw_params	= bluetooth_hsp_hw_params,
	.prepare	= bluetooth_prepare,
	.transfer	= bluetooth_hsp_write,
};

static snd_pcm_ioplug_callback_t bluetooth_hsp_capture = {
	.start		= bluetooth_start,
	.stop		= bluetooth_stop,
	.pointer	= bluetooth_pointer,
	.close		= bluetooth_close,
	.hw_params	= bluetooth_hsp_hw_params,
	.prepare	= bluetooth_prepare,
	.transfer	= bluetooth_hsp_read,
};

static snd_pcm_ioplug_callback_t bluetooth_a2dp_playback = {
	.start		= bluetooth_start,
	.stop		= bluetooth_stop,
	.pointer	= bluetooth_pointer,
	.close		= bluetooth_close,
	.hw_params	= bluetooth_a2dp_hw_params,
	.prepare	= bluetooth_prepare,
	.transfer	= bluetooth_a2dp_write,
};

static snd_pcm_ioplug_callback_t bluetooth_a2dp_capture = {
	.start		= bluetooth_start,
	.stop		= bluetooth_stop,
	.pointer	= bluetooth_pointer,
	.close		= bluetooth_close,
	.hw_params	= bluetooth_a2dp_hw_params,
	.prepare	= bluetooth_prepare,
	.transfer	= bluetooth_a2dp_read,
};

#define ARRAY_NELEMS(a) (sizeof((a)) / sizeof((a)[0]))

static int bluetooth_hw_constraint(snd_pcm_ioplug_t *io)
{
	struct bluetooth_data *data = io->private_data;
	struct ipc_data_cfg cfg = data->cfg;
	snd_pcm_access_t access_list[] = {
		SND_PCM_ACCESS_RW_INTERLEAVED,
		/* Mmap access is really useless fo this driver, but we
		 * support it because some pieces of software out there
		 * insist on using it */
		SND_PCM_ACCESS_MMAP_INTERLEAVED
	};
	unsigned int format_list[] = {
		SND_PCM_FORMAT_S16_LE
	};
	int err;

	/* access type */
	err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS,
					ARRAY_NELEMS(access_list), access_list);
	if (err < 0)
		return err;

	/* supported formats */
	err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT,
					ARRAY_NELEMS(format_list), format_list);
	if (err < 0)
		return err;

	/* supported channels */
	err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS,
					cfg.channels, cfg.channels);
	if (err < 0)
		return err;

	/* supported rate */
	err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE,
					cfg.rate, cfg.rate);
	if (err < 0)
		return err;

	/* supported block size */
	err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
					cfg.pkt_len, cfg.pkt_len);
	if (err < 0)
		return err;

	err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS,
					2, 200);
	if (err < 0)
		return err;

	return 0;
}

static int bluetooth_recvmsg_fd(struct bluetooth_data *data)
{
	char cmsg_b[CMSG_SPACE(sizeof(int))];
	struct ipc_packet pkt;
	int err, ret;
	struct iovec iov = {
		.iov_base = &pkt,
		.iov_len  = sizeof(pkt)
        };
	struct msghdr msgh = {
		.msg_name       = 0,
		.msg_namelen    = 0,
		.msg_iov        = &iov,
		.msg_iovlen     = 1,
		.msg_control    = &cmsg_b,
		.msg_controllen = CMSG_LEN(sizeof(int)),
		.msg_flags      = 0
	};

	ret = recvmsg(data->sock, &msgh, 0);
	if (ret < 0) {
		err = errno;
		SNDERR("Unable to receive fd: %s (%d)", strerror(err), err);
		return -err;
	}

	if (pkt.type == PKT_TYPE_CFG_RSP) {
		struct cmsghdr *cmsg;
		/* Receive auxiliary data in msgh */
		for (cmsg = CMSG_FIRSTHDR(&msgh); cmsg != NULL;
				cmsg = CMSG_NXTHDR(&msgh,cmsg)) {
			if (cmsg->cmsg_level == SOL_SOCKET
				&& cmsg->cmsg_type == SCM_RIGHTS)
				data->stream_fd = (*(int *) CMSG_DATA(cmsg));
				DBG("stream_fd = %d", data->stream_fd);
				return 0;
		}
	}
	else
		SNDERR("Unexpected packet type received: type = %d", pkt.type);

	return -EINVAL;
}

static int bluetooth_a2dp_init(struct bluetooth_data *data,
				struct ipc_codec_sbc *sbc)
{
	struct bluetooth_a2dp *a2dp = &data->a2dp;
	struct ipc_data_cfg *cfg = &data->cfg;

	if (cfg == NULL) {
		SNDERR("Error getting codec parameters");
		return -1;
	}

	if (cfg->codec != CFG_CODEC_SBC)
		return -1;

	/* FIXME: init using flags? */
	sbc_init(&a2dp->sbc, 0);
	a2dp->sbc.rate = cfg->rate;
	a2dp->sbc.channels = cfg->channels;
	if (cfg->channel_mode == CFG_CHANNEL_MODE_MONO ||
			cfg->channel_mode == CFG_CHANNEL_MODE_JOINT_STEREO)
		a2dp->sbc.joint = 1;
	a2dp->sbc.allocation = sbc->allocation;
	a2dp->sbc.subbands = sbc->subbands;
	a2dp->sbc.blocks = sbc->blocks;
	a2dp->sbc.bitpool = sbc->bitpool;

	return 0;
}

static int bluetooth_cfg(struct bluetooth_data *data, snd_config_t *conf)
{
	int ret, total;
	char buf[IPC_MTU];
	struct ipc_packet *pkt = (void *) buf;
	struct ipc_data_cfg *cfg = (void *) pkt->data;
	struct ipc_codec_sbc *sbc = (void *) cfg->data;

	DBG("Sending PKT_TYPE_CFG_REQ...");

	memset(buf, 0, sizeof(buf));
	pkt->type = PKT_TYPE_CFG_REQ;
	pkt->role = PKT_ROLE_NONE;
	pkt->error = PKT_ERROR_NONE;

	ret = send(data->sock, pkt, sizeof(struct ipc_packet), 0);
	if (ret < 0)
		return -errno;
	else if (ret == 0)
		return -EIO;

	DBG("OK - %d bytes sent. Waiting for response...", ret);

	memset(buf, 0, sizeof(buf));

	ret = recv(data->sock, buf, sizeof(*pkt) + sizeof(*cfg), 0);
	if (ret < 0)
		return -errno;
	else if (ret == 0)
		return -EIO;

	total = ret;

	if (pkt->type != PKT_TYPE_CFG_RSP) {
		SNDERR("Unexpected packet type received: type = %d",
				pkt->type);
		return -EINVAL;
	}

	if (pkt->error != PKT_ERROR_NONE) {
		SNDERR("Error while configuring device: error = %d",
				pkt->error);
		return pkt->error;
	}

	if (cfg->codec != CFG_CODEC_SBC)
		goto done;

	ret = recv(data->sock, sbc, sizeof(*sbc), 0);
	if (ret < 0)
		return -errno;
	else if (ret == 0)
		return -EIO;

	total += ret;

done:
	DBG("OK - %d bytes received", total);

	if (pkt->length != (total - sizeof(struct ipc_packet))) {
		SNDERR("Error while configuring device: packet size doesn't "
				"match");
		return -EINVAL;
	}

	memcpy(&data->cfg, cfg, sizeof(*cfg));

	DBG("Device configuration:");

	DBG("\n\tfd=%d\n\tfd_opt=%u\n\tchannels=%u\n\tpkt_len=%u\n"
		"\tsample_size=%u\n\trate=%u", data->stream_fd,
		data->cfg.fd_opt, data->cfg.channels, data->cfg.pkt_len,
		data->cfg.sample_size, data->cfg.rate);

	if (data->cfg.codec == CFG_CODEC_SBC) {
		struct bluetooth_a2dp *a2dp = &data->a2dp;
		ret = bluetooth_a2dp_init(data, sbc);
		if (ret < 0)
			return ret;
		printf("\tallocation=%u\n\tsubbands=%u\n\tblocks=%u\n\t"
				"bitpool=%u\n", a2dp->sbc.allocation,
				a2dp->sbc.subbands, a2dp->sbc.blocks,
				a2dp->sbc.bitpool);
	}

	if (data->stream_fd == -1) {
		SNDERR("Error while configuring device: could not acquire "
				"audio socket");
		return -EINVAL;
	}

	ret = bluetooth_recvmsg_fd(data);
	if (ret < 0)
		return ret;

	/* It is possible there is some outstanding
	data in the pipe - we have to empty it */
	while (recv(data->stream_fd, data->buffer, data->cfg.pkt_len,
				MSG_DONTWAIT) > 0);

	memset(data->buffer, 0, data->cfg.pkt_len);

	return 0;
}

static int bluetooth_init(struct bluetooth_data *data, snd_config_t *conf)
{
	int sk, err;
	struct sockaddr_un addr = {
		AF_UNIX, IPC_SOCKET_NAME
	};

	if (!data)
		return -EINVAL;

	memset(data, 0, sizeof(struct bluetooth_data));

	data->sock = -1;

	sk = socket(PF_LOCAL, SOCK_STREAM, 0);
	if (sk < 0) {
		err = errno;
		SNDERR("Cannot open socket: %s (%d)", strerror(err), err);
		return -err;
	}

	DBG("Connecting to address: %s", addr.sun_path + 1);
	if (connect(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		err = errno;
		SNDERR("Connection fail", strerror(err), err);
		close(sk);
		return -err;
	}

	data->sock = sk;

	return bluetooth_cfg(data, conf);
}

SND_PCM_PLUGIN_DEFINE_FUNC(bluetooth)
{
	struct bluetooth_data *data;
	int err;

	DBG("Bluetooth PCM plugin (%s)",
		stream == SND_PCM_STREAM_PLAYBACK ? "Playback" : "Capture");

	data = malloc(sizeof(struct bluetooth_data));
	if (!data) {
		err = -ENOMEM;
		goto error;
	}

	err = bluetooth_init(data, conf);
	if (err < 0)
		goto error;

	data->io.version = SND_PCM_IOPLUG_VERSION;
	data->io.name = "Bluetooth Audio Device";
	data->io.mmap_rw = 0; /* No direct mmap communication */
	data->io.poll_fd = data->stream_fd;
	data->io.poll_events = stream == SND_PCM_STREAM_PLAYBACK ?
					POLLOUT : POLLIN;
	data->io.private_data = data;

	if (data->cfg.codec == CFG_CODEC_SBC)
		data->io.callback = stream == SND_PCM_STREAM_PLAYBACK ?
			&bluetooth_a2dp_playback :
			&bluetooth_a2dp_capture;
	else
		data->io.callback = stream == SND_PCM_STREAM_PLAYBACK ?
			&bluetooth_hsp_playback :
			&bluetooth_hsp_capture;

	err = snd_pcm_ioplug_create(&data->io, name, stream, mode);
	if (err < 0)
		goto error;

	err = bluetooth_hw_constraint(&data->io);
	if (err < 0) {
		snd_pcm_ioplug_delete(&data->io);
		goto error;
	}

	*pcmp = data->io.pcm;

	return 0;

error:
	bluetooth_exit(data);

	return err;
}

SND_PCM_PLUGIN_SYMBOL(bluetooth);
