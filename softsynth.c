/*
 *  espeakup - interface which allows speakup to use espeak
 *
 *  Copyright (C) 2008 William Hubbs
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include "espeakup.h"

/* max buffer size */
static const size_t maxBufferSize = 1025;

/* synth flush character */
static const int synthFlushChar = 0x18;

static int softFD = 0;

static void queue_add_cmd(enum command_t cmd, enum adjust_t adj, int value)
{
	struct espeak_entry_t *entry;

	entry = malloc(sizeof(struct espeak_entry_t));
	if (!entry) {
		perror("unable to allocate memory for queue entry");
		return;
	}
	entry->cmd = cmd;
	entry->adjust = adj;
	entry->value = value;
	pthread_mutex_lock(&queue_guard);
	queue_add(synth_queue, (void *) entry);
	pthread_cond_signal(&runner_awake);
	pthread_mutex_unlock(&queue_guard);
}

static void queue_add_text(char *txt, size_t length)
{
	struct espeak_entry_t *entry;

	entry = malloc(sizeof(struct espeak_entry_t));
	if (!entry) {
		perror("unable to allocate memory for queue entry");
		return;
	}
	entry->cmd = CMD_SPEAK_TEXT;
	entry->adjust = ADJ_SET;
	entry->buf = strdup(txt);
	if (!entry->buf) {
		perror("unable to allocate space for text");
		free(entry);
		return;
	}
	entry->len = length;
	pthread_mutex_lock(&queue_guard);
	queue_add(synth_queue, (void *) entry);
	pthread_cond_signal(&runner_awake);
	pthread_mutex_unlock(&queue_guard);
}

static int process_command(struct synth_t *s, char *buf, int start)
{
	char *cp;
	int value;
	enum adjust_t adj;
	enum command_t cmd;

	cp = buf + start;
	switch (*cp) {
	case 1:
		cp++;
		switch (*cp) {
		case '+':
			adj = ADJ_INC;
			cp++;
			break;
		case '-':
			adj = ADJ_DEC;
			cp++;
			break;
		default:
			adj = ADJ_SET;
			break;
		}

		value = 0;
		while (isdigit(*cp)) {
			value = value * 10 + (*cp - '0');
			cp++;
		}

		switch (*cp) {
		case 'b':
			cmd = CMD_SET_PUNCTUATION;
			break;
		case 'f':
			cmd = CMD_SET_FREQUENCY;
			break;
		case 'p':
			cmd = CMD_SET_PITCH;
			break;
		case 's':
			cmd = CMD_SET_RATE;
			break;
		case 'v':
			cmd = CMD_SET_VOLUME;
			break;
		default:
			cmd = CMD_UNKNOWN;
			break;
		}
		cp++;
		break;
	default:
		cmd = CMD_UNKNOWN;
		cp++;
		break;
	}

	if (cmd != CMD_FLUSH && cmd != CMD_UNKNOWN)
		queue_add_cmd(cmd, adj, value);

	return cp - (buf + start);
}

static void process_buffer(struct synth_t *s, char *buf, ssize_t length)
{
	int start;
	int end;
	char txtBuf[maxBufferSize];
	size_t txtLen;

	start = 0;
	end = 0;
	while (start < length) {
		while ((buf[end] < 0 || buf[end] >= ' ') && end < length)
			end++;
		if (end != start) {
			txtLen = end - start;
			strncpy(txtBuf, buf + start, txtLen);
			*(txtBuf + txtLen) = 0;
			queue_add_text(txtBuf, txtLen);
		}
		if (end < length)
			start = end = end + process_command(s, buf, end);
		else
			start = length;
	}
}

static void request_espeak_stop(void)
{
	pthread_mutex_lock(&queue_guard);
	stop_audio();
	runner_must_stop = 1;
	pthread_cond_signal(&runner_awake);	/* Wake runner, if necessary. */
	while(should_run && (runner_must_stop == 1))
	pthread_cond_wait(&stop_acknowledged, &queue_guard);	/* wait for acknowledgement. */
	pthread_mutex_unlock(&queue_guard);
}

int open_softsynth(void)
{
	int rc = 0;
	/* open the softsynth. */
	softFD = open("/dev/softsynth", O_RDWR | O_NONBLOCK);
	if (softFD < 0) {
		perror("Unable to open the softsynth device");
		rc = -1;
	}
	return rc;
}

void close_softsynth(void)
{
	if (softFD)
		close(softFD);
}

void *softsynth_thread(void *arg)
{
	struct synth_t *s = (struct synth_t *) arg;
	fd_set set;
	ssize_t length;
	char buf[maxBufferSize];
	char *cp;
	int terminalFD = PIPE_READ_FD;
	int greatestFD;

	if (terminalFD > softFD)
		greatestFD = terminalFD;
	else
		greatestFD = softFD;
	pthread_mutex_lock(&queue_guard);
	while (should_run) {
		pthread_mutex_unlock(&queue_guard);
		FD_ZERO(&set);
		FD_SET(softFD, &set);
		FD_SET(terminalFD, &set);

		if (select(greatestFD + 1, &set, NULL, NULL, NULL) < 0) {
			if (errno == EINTR) {
				pthread_mutex_lock(&queue_guard);
				continue;
			}
			perror("Select failed");
			pthread_mutex_lock(&queue_guard);
			break;
		}

		if (FD_ISSET(terminalFD, &set)) {
			pthread_mutex_lock(&queue_guard);
			break;
		}

		if (!FD_ISSET(softFD, &set)) {
			pthread_mutex_lock(&queue_guard);
			continue;
		}

		length = read(softFD, buf, maxBufferSize - 1);
		if (length < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				pthread_mutex_lock(&queue_guard);
				continue;
			}
			perror("Read from softsynth failed");
			pthread_mutex_lock(&queue_guard);
			break;
		}
		*(buf + length) = 0;
		cp = strrchr(buf, synthFlushChar);
		if (cp) {
			request_espeak_stop();
			memmove(buf, cp + 1, strlen(cp + 1) + 1);
			length = strlen(buf);
		}
		process_buffer(s, buf, length);
		pthread_mutex_lock(&queue_guard);
	}
	pthread_cond_signal(&runner_awake);
	pthread_mutex_unlock(&queue_guard);
	return NULL;
}
