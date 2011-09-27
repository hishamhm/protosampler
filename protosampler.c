/*
 *  sampler.c
 *
 *  based on amidi.c from alsa-utils 1.0.13
 *  Copyright (c) Clemens Ladisch <clemens@ladisch.de>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <alsa/asoundlib.h>
#include "config.h"

static char *port_name = "default";
static int stop;
static snd_rawmidi_t *input, **inputp;
static snd_rawmidi_t *output, **outputp;

static void error(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	putc('\n', stderr);
}

static void usage(void)
{
	printf(
		"Usage: protosampler options\n"
		"\n"
		"-h, --help             this help\n"
		"-V, --version          print current version\n"
		"-l, --list-devices     list all hardware ports\n"
		"-L, --list-rawmidis    list all RawMIDI definitions\n"
		"-p, --port=name        select port by name\n"
		"-a, --active-sensing   don't ignore active sensing bytes\n");
}

static void version(void)
{
	puts("protosampler version " VERSION);
}

static void *my_malloc(size_t size)
{
	void *p = malloc(size);
	if (!p) {
		error("out of memory");
		exit(EXIT_FAILURE);
	}
	return p;
}

static int is_input(snd_ctl_t *ctl, int card, int device, int sub)
{
	snd_rawmidi_info_t *info;
	int err;

	snd_rawmidi_info_alloca(&info);
	snd_rawmidi_info_set_device(info, device);
	snd_rawmidi_info_set_subdevice(info, sub);
	snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
	
	if ((err = snd_ctl_rawmidi_info(ctl, info)) < 0 && err != -ENXIO)
		return err;
	else if (err == 0)
		return 1;

	return 0;
}

static int is_output(snd_ctl_t *ctl, int card, int device, int sub)
{
	snd_rawmidi_info_t *info;
	int err;

	snd_rawmidi_info_alloca(&info);
	snd_rawmidi_info_set_device(info, device);
	snd_rawmidi_info_set_subdevice(info, sub);
	snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_OUTPUT);
	
	if ((err = snd_ctl_rawmidi_info(ctl, info)) < 0 && err != -ENXIO)
		return err;
	else if (err == 0)
		return 1;

	return 0;
}

static void list_device(snd_ctl_t *ctl, int card, int device)
{
	snd_rawmidi_info_t *info;
	const char *name;
	const char *sub_name;
	int subs, subs_in, subs_out;
	int sub, in, out;
	int err;

	snd_rawmidi_info_alloca(&info);
	snd_rawmidi_info_set_device(info, device);

	snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
	snd_ctl_rawmidi_info(ctl, info);
	subs_in = snd_rawmidi_info_get_subdevices_count(info);
	snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_OUTPUT);
	snd_ctl_rawmidi_info(ctl, info);
	subs_out = snd_rawmidi_info_get_subdevices_count(info);
	subs = subs_in > subs_out ? subs_in : subs_out;

	sub = 0;
	in = out = 0;
	if ((err = is_output(ctl, card, device, sub)) < 0) {
		error("cannot get rawmidi information %d:%d: %s",
		      card, device, snd_strerror(err));
		return;
	} else if (err)
		out = 1;

	if (err == 0) {
		if ((err = is_input(ctl, card, device, sub)) < 0) {
			error("cannot get rawmidi information %d:%d: %s",
			      card, device, snd_strerror(err));
			return;
		}
	} else if (err) 
		in = 1;

	if (err == 0)
		return;

	name = snd_rawmidi_info_get_name(info);
	sub_name = snd_rawmidi_info_get_subdevice_name(info);
	if (sub_name[0] == '\0') {
		if (subs == 1) {
			printf("%c%c  hw:%d,%d    %s\n", 
			       in ? 'I' : ' ', out ? 'O' : ' ',
			       card, device, name);
		} else
			printf("%c%c  hw:%d,%d    %s (%d subdevices)\n",
			       in ? 'I' : ' ', out ? 'O' : ' ',
			       card, device, name, subs);
	} else {
		sub = 0;
		for (;;) {
			printf("%c%c  hw:%d,%d,%d  %s\n",
			       in ? 'I' : ' ', out ? 'O' : ' ',
			       card, device, sub, sub_name);
			if (++sub >= subs)
				break;

			in = is_input(ctl, card, device, sub);
			out = is_output(ctl, card, device, sub);
			snd_rawmidi_info_set_subdevice(info, sub);
			if (out) {
				snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_OUTPUT);
				if ((err = snd_ctl_rawmidi_info(ctl, info)) < 0) {
					error("cannot get rawmidi information %d:%d:%d: %s",
					      card, device, sub, snd_strerror(err));
					break;
				} 
			} else {
				snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
				if ((err = snd_ctl_rawmidi_info(ctl, info)) < 0) {
					error("cannot get rawmidi information %d:%d:%d: %s",
					      card, device, sub, snd_strerror(err));
					break;
				}
			}
			sub_name = snd_rawmidi_info_get_subdevice_name(info);
		}
	}
}

static void list_card_devices(int card)
{
	snd_ctl_t *ctl;
	char name[32];
	int device;
	int err;

	sprintf(name, "hw:%d", card);
	if ((err = snd_ctl_open(&ctl, name, 0)) < 0) {
		error("cannot open control for card %d: %s", card, snd_strerror(err));
		return;
	}
	device = -1;
	for (;;) {
		if ((err = snd_ctl_rawmidi_next_device(ctl, &device)) < 0) {
			error("cannot determine device number: %s", snd_strerror(err));
			break;
		}
		if (device < 0)
			break;
		list_device(ctl, card, device);
	}
	snd_ctl_close(ctl);
}

static void device_list(void)
{
	int card, err;

	card = -1;
	if ((err = snd_card_next(&card)) < 0) {
		error("cannot determine card number: %s", snd_strerror(err));
		return;
	}
	if (card < 0) {
		error("no sound card found");
		return;
	}
	puts("Dir Device    Name");
	do {
		list_card_devices(card);
		if ((err = snd_card_next(&card)) < 0) {
			error("cannot determine card number: %s", snd_strerror(err));
			break;
		}
	} while (card >= 0);
}

static void rawmidi_list(void)
{
	snd_output_t *output;
	snd_config_t *config;
	int err;

	if ((err = snd_config_update()) < 0) {
		error("snd_config_update failed: %s", snd_strerror(err));
		return;
	}
	if ((err = snd_output_stdio_attach(&output, stdout, 0)) < 0) {
		error("snd_output_stdio_attach failed: %s", snd_strerror(err));
		return;
	}
	if (snd_config_search(snd_config, "rawmidi", &config) >= 0) {
		puts("RawMIDI list:");
		snd_config_save(config, output);
	}
	snd_output_close(output);
}

static int hex_value(char c)
{
	if ('0' <= c && c <= '9')
		return c - '0';
	if ('A' <= c && c <= 'F')
		return c - 'A' + 10;
	if ('a' <= c && c <= 'f')
		return c - 'a' + 10;
	error("invalid character %c", c);
	return -1;
}

/*
 * prints MIDI commands, formatting them nicely
 */
static void print_byte(unsigned char byte)
{
	static enum {
		STATE_UNKNOWN,
		STATE_1PARAM,
		STATE_1PARAM_CONTINUE,
		STATE_2PARAM_1,
		STATE_2PARAM_2,
		STATE_2PARAM_1_CONTINUE,
		STATE_SYSEX
	} state = STATE_UNKNOWN;
	int newline = 0;

	if (byte >= 0xf8)
		newline = 1;
	else if (byte >= 0xf0) {
		newline = 1;
		switch (byte) {
		case 0xf0:
			state = STATE_SYSEX;
			break;
		case 0xf1:
		case 0xf3:
			state = STATE_1PARAM;
			break;
		case 0xf2:
			state = STATE_2PARAM_1;
			break;
		case 0xf4:
		case 0xf5:
		case 0xf6:
			state = STATE_UNKNOWN;
			break;
		case 0xf7:
			newline = state != STATE_SYSEX;
			state = STATE_UNKNOWN;
			break;
		}
	} else if (byte >= 0x80) {
		newline = 1;
		if (byte >= 0xc0 && byte <= 0xdf)
			state = STATE_1PARAM;
		else
			state = STATE_2PARAM_1;
	} else /* b < 0x80 */ {
		int running_status = 0;
		newline = state == STATE_UNKNOWN;
		switch (state) {
		case STATE_1PARAM:
			state = STATE_1PARAM_CONTINUE;
			break;
		case STATE_1PARAM_CONTINUE:
			running_status = 1;
			break;
		case STATE_2PARAM_1:
			state = STATE_2PARAM_2;
			break;
		case STATE_2PARAM_2:
			state = STATE_2PARAM_1_CONTINUE;
			break;
		case STATE_2PARAM_1_CONTINUE:
			running_status = 1;
			state = STATE_2PARAM_2;
			break;
		default:
			break;
		}
		if (running_status)
			fputs("\n  ", stdout);
	}
	printf("%c%02X", newline ? '\n' : ' ', byte);
}

static void sig_handler(int dummy)
{
	stop = 1;
}

int main(int argc, char *argv[])
{
	static char short_options[] = "hVlLp:a";
	static struct option long_options[] = {
		{"help", 0, NULL, 'h'},
		{"version", 0, NULL, 'V'},
		{"list-devices", 0, NULL, 'l'},
		{"list-rawmidis", 0, NULL, 'L'},
		{"port", 1, NULL, 'p'},
		{"active-sensing", 0, NULL, 'a'},
		{ }
	};
	int c, err, ok = 0;
	int do_rawmidi_list = 0;
	int do_device_list = 0;
	int ignore_active_sensing = 1;

	while ((c = getopt_long(argc, argv, short_options,
		     		long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			usage();
			return 0;
		case 'V':
			version();
			return 0;
		case 'l':
			do_device_list = 1;
			break;
		case 'L':
			do_rawmidi_list = 1;
			break;
		case 'p':
			port_name = optarg;
			break;
		case 'a':
			ignore_active_sensing = 0;
			break;
		default:
			error("Try `protosampler --help' for more information.");
			return 1;
		}
	}
	if (argv[optind]) {
		error("%s is not an option.", argv[optind]);
		return 1;
	}

	if (do_rawmidi_list)
		rawmidi_list();
	if (do_device_list)
		device_list();
	if (do_rawmidi_list || do_device_list)
		return 0;

	inputp = &input;

	if ((err = snd_rawmidi_open(inputp, outputp, port_name, 0)) < 0) {
		error("cannot open port \"%s\": %s", port_name, snd_strerror(err));
		goto _exit2;
	}

	if (inputp)
		snd_rawmidi_read(input, NULL, 0); /* trigger reading */

	if (inputp) {
		int read = 0;
		int npfds, time = 0;
		struct pollfd *pfds;

		snd_rawmidi_nonblock(input, 1);
		npfds = snd_rawmidi_poll_descriptors_count(input);
		pfds = alloca(npfds * sizeof(struct pollfd));
		snd_rawmidi_poll_descriptors(input, pfds, npfds);
		signal(SIGINT, sig_handler);
		for (;;) {
			unsigned char buf[256];
			int i, length;
			unsigned short revents;

			err = poll(pfds, npfds, 200);
			if (stop || (err < 0 && errno == EINTR))
				break;
			if (err < 0) {
				error("poll failed: %s", strerror(errno));
				break;
			}
			if (err == 0) {
				time += 200;
				continue;
			}
			if ((err = snd_rawmidi_poll_descriptors_revents(input, pfds, npfds, &revents)) < 0) {
				error("cannot get poll events: %s", snd_strerror(errno));
				break;
			}
			if (revents & (POLLERR | POLLHUP))
				break;
			if (!(revents & POLLIN))
				continue;
			err = snd_rawmidi_read(input, buf, sizeof(buf));
			if (err == -EAGAIN)
				continue;
			if (err < 0) {
				error("cannot read from port \"%s\": %s", port_name, snd_strerror(err));
				break;
			}
			length = 0;
			for (i = 0; i < err; ++i)
				if (!ignore_active_sensing || buf[i] != 0xfe)
					buf[length++] = buf[i];
			if (length == 0)
				continue;
			read += length;
			time = 0;

			for (i = 0; i < length; ++i)
				print_byte(buf[i]);
			fflush(stdout);
		}
		if (isatty(fileno(stdout)))
			printf("\n%d bytes read\n", read);
	}

	ok = 1;
_exit:
	if (inputp)
		snd_rawmidi_close(input);
	if (outputp)
		snd_rawmidi_close(output);
_exit2:
	return !ok;
}
