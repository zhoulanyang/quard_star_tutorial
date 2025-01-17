/* $OpenBSD: cu.c,v 1.14 2013/04/24 16:01:10 tedu Exp $ */

/*
 * Copyright (c) 2012 Nicholas Marriott <nicm@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/ioctl.h>

#include <ctype.h>
#include <err.h>
#include <event.h>
#include <fcntl.h>
#include <getopt.h>
#include <paths.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <stdbool.h>

#include "cu.h"

extern char		*__progname;

FILE			*record_file;
struct termios		 saved_tio;
struct bufferevent	*input_ev;
struct bufferevent	*output_ev;
int			 line_fd;
struct termios		 line_tio;
struct bufferevent	*line_ev;
struct event		 sigterm_ev;
struct event		 sighup_ev;
enum {
	STATE_NONE,
	STATE_NEWLINE,
	STATE_TILDE
} last_state = STATE_NEWLINE;
bool locally_echo = false;

#if !defined(__dead)
#define __dead __attribute__((__noreturn__))
#endif

__dead void	usage(void);
void		signal_event(int, short, void *);
void		stream_read(struct bufferevent *, void *);
void		stream_error(struct bufferevent *, short, void *);
void		line_read(struct bufferevent *, void *);
void		line_error(struct bufferevent *, short, void *);

extern int asprintf(char **restrict strp, const char *restrict fmt, ...);
extern long long strtonum(const char *nptr, long long minval, long long maxval,const char **errstr);

__dead void
usage(void)
{
	fprintf(stderr, "usage: %s [-l line] [-s speed | -speed] [-h | --halfduplex]\n",
	    __progname);
	exit(1);
}

int
main(int argc, char **argv)
{
	const char	*line, *errstr;
	char		*tmp;
	int		 opt, speed, i;

	line = "/dev/cua00";
	speed = 9600;

	/*
	 * Convert obsolescent -### speed to modern -s### syntax which getopt()
	 * can handle.
	 */
	for (i = 1; i < argc; i++) {
		if (strcmp("--", argv[i]) == 0)
			break;
		if (argv[i][0] != '-' || !isdigit(argv[i][1]))
			continue;

		if (asprintf(&argv[i], "-s%s", &argv[i][1]) == -1)
			errx(1, "speed asprintf");
	}

	while ((opt = getopt(argc, argv, "l:s:h")) != -1) {
		switch (opt) {
		case 'l':
			line = optarg;
			break;
		case 's':
			speed = strtonum(optarg, 0, UINT_MAX, &errstr);
			if (errstr != NULL)
				errx(1, "speed is %s: %s", errstr, optarg);
			break;
		case 'h':
			locally_echo = true;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 0)
		usage();

	if (strchr(line, '/') == NULL) {
		if (asprintf(&tmp, "%s%s", _PATH_DEV, line) == -1)
			err(1, "asprintf");
		line = tmp;
	}

	line_fd = open(line, O_RDWR);
	if (line_fd < 0)
		err(1, "open(\"%s\")", line);
	if (ioctl(line_fd, TIOCEXCL) != 0)
		err(1, "ioctl(TIOCEXCL)");
	if (tcgetattr(line_fd, &line_tio) != 0)
		err(1, "tcgetattr");
	if (set_line(speed) != 0)
		err(1, "tcsetattr");

	if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved_tio) != 0)
		err(1, "tcgetattr");

	event_init();

	signal_set(&sigterm_ev, SIGTERM, signal_event, NULL);
	signal_add(&sigterm_ev, NULL);
	signal_set(&sighup_ev, SIGHUP, signal_event, NULL);
	signal_add(&sighup_ev, NULL);
	if (signal(SIGINT, SIG_IGN) == SIG_ERR)
		err(1, "signal");
	if (signal(SIGQUIT, SIG_IGN) == SIG_ERR)
		err(1, "signal");

	set_termios(); /* after this use cu_err and friends */

	/* stdin and stdout get separate events */
	input_ev = bufferevent_new(STDIN_FILENO, stream_read, NULL,
	    stream_error, NULL);
	bufferevent_enable(input_ev, EV_READ);
	output_ev = bufferevent_new(STDOUT_FILENO, NULL, NULL, stream_error,
	    NULL);
	bufferevent_enable(output_ev, EV_WRITE);

	line_ev = bufferevent_new(line_fd, line_read, NULL, line_error,
	    NULL);
	bufferevent_enable(line_ev, EV_READ|EV_WRITE);

	printf("Connected (speed %d)\r\n", speed);
	event_dispatch();

	restore_termios();
	printf("\r\n[EOT]\n");

	exit(0);
}

void
signal_event(int fd, short events, void *data)
{
	restore_termios();
	printf("\r\n[SIG%d]\n", fd);

	exit(0);
	(void)events;
	(void)data;
}

void
set_termios(void)
{
	struct termios tio;

	if (!isatty(STDIN_FILENO))
		return;

	memcpy(&tio, &saved_tio, sizeof(tio));
	tio.c_lflag &= ~(ICANON|IEXTEN|ECHO);
	tio.c_iflag &= ~(INPCK|ICRNL);
	tio.c_iflag &= ~(IXON);       /* disable XON/XOFF flow control on output. */
	tio.c_oflag &= ~OPOST;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;
	tio.c_cc[VDISCARD] = _POSIX_VDISABLE;
#if defined(VDSUSP)
	tio.c_cc[VDSUSP] = _POSIX_VDISABLE;
#endif
	tio.c_cc[VINTR] = _POSIX_VDISABLE;
	tio.c_cc[VLNEXT] = _POSIX_VDISABLE;
	tio.c_cc[VQUIT] = _POSIX_VDISABLE;
	tio.c_cc[VSUSP] = _POSIX_VDISABLE;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &tio) != 0)
		cu_err(1, "tcsetattr");
}

void
restore_termios(void)
{
	if (isatty(STDIN_FILENO))
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tio);
}

int
set_line(int speed)
{
	struct termios	 tio;

	memcpy(&tio, &line_tio, sizeof(tio));
	tio.c_iflag &= ~(ISTRIP|ICRNL);
	tio.c_oflag &= ~OPOST;
	tio.c_lflag &= ~(ICANON|ISIG|IEXTEN|ECHO);
	tio.c_cflag &= ~(CSIZE|PARENB);
	tio.c_cflag |= CREAD|CS8|CLOCAL;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;
	cfsetspeed(&tio, speed);
	if (tcsetattr(line_fd, TCSAFLUSH, &tio) != 0)
		return (-1);
	return (0);
}

void
stream_read(struct bufferevent *bufev, void *data)
{
	char	*new_data, *ptr;
	size_t	 new_size;
	int	 state_change;

	new_data = (char *)EVBUFFER_DATA(input_ev->input);
	new_size = EVBUFFER_LENGTH(input_ev->input);
	if (new_size == 0)
		return;

	state_change = isatty(STDIN_FILENO);
	for (ptr = new_data; ptr < new_data + new_size; ptr++) {
		switch (last_state) {
		case STATE_NONE:
			if (state_change && *ptr == '\r')
				last_state = STATE_NEWLINE;
			break;
		case STATE_NEWLINE:
			if (state_change && *ptr == '~') {
				printf("~");
				fflush(stdout);
				last_state = STATE_TILDE;
				continue;
			}
			if (*ptr != '\r')
				last_state = STATE_NONE;
			break;
		case STATE_TILDE:
			printf("%c\r\n",*ptr);
			fflush(stdout);
			do_command(*ptr);
			last_state = STATE_NEWLINE;
			continue;
		}
		if(locally_echo) {
			printf("%c",*ptr);
			if(*ptr=='\r')
				printf("\n");
			fflush(stdout);
		}
		bufferevent_write(line_ev, ptr, 1);
		if(*ptr=='\r')
			bufferevent_write(line_ev, "\n", 1);
	}

	evbuffer_drain(input_ev->input, new_size);
	(void)bufev;
	(void)data;
}

void
stream_error(struct bufferevent *bufev, short what, void *data)
{
	event_loopexit(NULL);
	(void)bufev;
	(void)what;
	(void)data;
}

void
line_read(struct bufferevent *bufev, void *data)
{
	char	*new_data;
	size_t	 new_size;

	new_data = (char *)EVBUFFER_DATA(line_ev->input);
	new_size = EVBUFFER_LENGTH(line_ev->input);
	if (new_size == 0)
		return;

	if (record_file != NULL)
		fwrite(new_data, 1, new_size, record_file);
	bufferevent_write(output_ev, new_data, new_size);

	evbuffer_drain(line_ev->input, new_size);
	(void)bufev;
	(void)data;
}

void
line_error(struct bufferevent *bufev, short what, void *data)
{
	event_loopexit(NULL);
	(void)bufev;
	(void)what;
	(void)data;
}

/* Expands tildes in the file name. Based on code from ssh/misc.c. */
char *
tilde_expand(const char *filename1)
{
	const char	*filename, *path, *sep;
	char		 user[128], *out;
	struct passwd	*pw;
	u_int		 len, slash;
	int		 rv;

	if (*filename1 != '~')
		goto no_change;
	filename = filename1 + 1;

	path = strchr(filename, '/');
	if (path != NULL && path > filename) {		/* ~user/path */
		slash = path - filename;
		if (slash > sizeof(user) - 1)
			goto no_change;
		memcpy(user, filename, slash);
		user[slash] = '\0';
		if ((pw = getpwnam(user)) == NULL)
			goto no_change;
	} else if ((pw = getpwuid(getuid())) == NULL)	/* ~/path */
		goto no_change;

	/* Make sure directory has a trailing '/' */
	len = strlen(pw->pw_dir);
	if (len == 0 || pw->pw_dir[len - 1] != '/')
		sep = "/";
	else
		sep = "";

	/* Skip leading '/' from specified path */
	if (path != NULL)
		filename = path + 1;

	if ((rv = asprintf(&out, "%s%s%s", pw->pw_dir, sep, filename)) == -1)
		cu_err(1, "asprintf");
	if (rv >= MAXPATHLEN) {
		free(out);
		goto no_change;
	}

	return (out);

no_change:
	out = strdup(filename1);
	if (out == NULL)
		cu_err(1, "strdup");
	return (out);
}
