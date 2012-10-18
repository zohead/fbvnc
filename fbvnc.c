/*
 * fbvnc - a small linux framebuffer vnc viewer
 *
 * Copyright (C) 2009-2012 Ali Gholami Rudi
 *
 * This program is released under the modified BSD license.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/input.h>

#include "draw.h"
#include "vnc.h"
#include "vncauth.h"

/* framebuffer depth */
typedef unsigned short fbval_t;

/* optimized version of fb_val() */
#define FB_VAL(r, g, b)	fb_val((r), (g), (b))

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

#define VNC_PORT		"5900"
#define FBVNC_VERSION	"1.0.1"

#define MAXRES			(1 << 12)

static int cols, rows;
static int srv_cols, srv_rows;
static int or, oc;
static int mr, mc;		/* mouse position */
static int nodraw;		/* don't draw anything */

static char buf[MAXRES];
#define MAXPIX		(MAXRES/sizeof(fbval_t))

static char *passwd_file = NULL, *passwd_save = NULL;

static int vnc_connect(char *addr, char *port)
{
	struct addrinfo hints, *addrinfo;
	int fd;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	if (getaddrinfo(addr, port, &hints, &addrinfo))
		return -1;
	fd = socket(addrinfo->ai_family, addrinfo->ai_socktype,
			addrinfo->ai_protocol);

	if (connect(fd, addrinfo->ai_addr, addrinfo->ai_addrlen) == -1) {
		close(fd);
		fd = -2;
	}
	freeaddrinfo(addrinfo);
	return fd;
}

static int bpp, vnc_mode;
static struct rgb_conv format;
static int vnc_init(int fd)
{
	static int vncfmt[] = { 0x40888, 0x20565, 0x10233, 0 };
	char vncver[12] = {0};
	int i;

	struct vnc_client_init clientinit;
	struct vnc_server_init serverinit;
	struct vnc_client_pixelfmt pixfmt_cmd;
	int connstat = VNC_CONN_FAILED;

	read(fd, vncver, 12);
	write(fd, "RFB 003.003\n", 12);

	vncver[11] = '\0';
	printf("VNC server protocol version: %s.\n", vncver);

	if (strncmp(vncver, "RFB ", 4) != 0) {
		fprintf(stderr, "Unregonized VNC server protocol version\n");
		return -7;
	}

	read(fd, &connstat, sizeof(connstat));

	if (ntohl(connstat) == VNC_CONN_FAILED) {
		char *buf = NULL;

		i = read(fd, &connstat, sizeof(connstat));
		// try to get reason from server
		if (i == sizeof(connstat)) {
			buf = calloc(1, ntohl(connstat) + 1);
			if (buf) {
				i = read(fd, buf, ntohl(connstat));
				// Didn't receive correct data, maybe version mismatch
				if (i != sizeof(connstat)) {
					free(buf);
					buf = NULL;
				}
			}
		}
		if (buf == NULL)
			fprintf(stderr, "VNC init failed - Unknown reason, maybe version mismatch?\n");
		else {
			fprintf(stderr, "VNC init failed - %s\n", buf);
			free(buf);
		}
		return -1;
	}

	if (ntohl(connstat) == VNC_CONN_AUTH) {
		unsigned char challenge[CHALLENGESIZE];
		char *passwd = NULL;

		if (read(fd, challenge, sizeof(challenge)) != sizeof(challenge))
			return -2;

		if (passwd_file) {
			passwd = vncDecryptPasswdFromFile(passwd_file);
		} else {
			passwd = getpass("Password: ");
			if (strlen(passwd) == 0) {
				fprintf(stderr, "Reading password failed\n");
				return -3;
			}
			if (strlen(passwd) > MAXPWLEN) {
				passwd[MAXPWLEN] = '\0';
			}
		}

		vncEncryptBytes(challenge, passwd);

		write(fd, challenge, sizeof(challenge));
		i = read(fd, &connstat, sizeof(connstat));
		if (i != sizeof(connstat)) {
			fprintf(stderr, "Invalid response from VNC server\n");
			return -4;
		}

		switch (ntohl(connstat))
		{
		case VNC_AUTH_OK:
			printf("VNC authentication succeeded\n");
			if (passwd_save) {
				vncEncryptAndStorePasswd(passwd, passwd_save);
			}
			break;
		case VNC_AUTH_FAILED:
			fprintf(stderr, "VNC authentication failed\n");
			break;
		case VNC_AUTH_TOOMANY:
			fprintf(stderr, "VNC authentication failed - too many tries\n");
			return -5;
		default:
			fprintf(stderr, "Unknown VNC authentication result: %d\n", ntohl(connstat));
			return -6;
		}

		// clear password in memory
		memset(passwd, 0, strlen(passwd));
	}

	clientinit.shared = 1;
	write(fd, &clientinit, sizeof(clientinit));
	read(fd, &serverinit, sizeof(serverinit));

	i = fb_init();
	if (i)
		return -1 - i;
	srv_cols = ntohs(serverinit.w);
	srv_rows = ntohs(serverinit.h);
	cols = MIN(srv_cols, fb_cols());
	rows = MIN(srv_rows, fb_rows());
	mr = rows / 2;
	mc = cols / 2;
	or = oc = 0;

	read(fd, buf, ntohl(serverinit.len));
	pixfmt_cmd.type = VNC_CLIENT_PIXFMT;
	pixfmt_cmd.format.bigendian = 0;
	pixfmt_cmd.format.truecolor = 1;

	if (bpp < 1)
	  	bpp = FBM_BPP(fb_mode());
	if (bpp >= 3)
		bpp = 4;
	for (i = 0; bpp <= FBM_BPP(vncfmt[i]); i++)
		vnc_mode = vncfmt[i];
	bpp = FBM_BPP(vnc_mode);
	pixfmt_cmd.format.bpp =
	pixfmt_cmd.format.depth = bpp << 3;
 
	fill_rgb_conv(FBM_COLORS(vnc_mode), &format);
	pixfmt_cmd.format.rmax = htons(format.rmax);
	pixfmt_cmd.format.gmax = htons(format.gmax);
	pixfmt_cmd.format.bmax = htons(format.bmax);
	pixfmt_cmd.format.rshl = format.rshl;
	pixfmt_cmd.format.gshl = format.gshl;
	pixfmt_cmd.format.bshl = 0;
	write(fd, &pixfmt_cmd, sizeof(pixfmt_cmd));
	return fd;
}

static void vnc_free(void)
{
	fb_free();
}

static void vnc_refresh(int fd, int inc)
{
	struct vnc_client_fbup fbup_req;
	fbup_req.type = VNC_CLIENT_FBUP;
	fbup_req.inc = inc;
	fbup_req.x = htons(oc);
	fbup_req.y = htons(or);
	fbup_req.w = htons(oc + cols);
	fbup_req.h = htons(or + rows);
	write(fd, &fbup_req, sizeof(fbup_req));
}

static void drawfb(char *s, int x, int y, int w)
{
	int mode = fb_mode();
	if (mode != vnc_mode) {
		fbval_t slice[MAXRES];
		unsigned char *byte = (unsigned char *) slice;
		int j;
		int fb_bpp = FBM_BPP(mode);
		for (j = 0; j < w; j++, byte += fb_bpp, s += bpp) {
			fbval_t c = * (fbval_t *) s;
			int r = ((c >> format.rshl) & format.rmax) << format.rskp;
			int g = ((c >> format.gshl) & format.gmax) << format.gskp;
			int b = (c & format.bmax) << format.bskp;
			* (fbval_t *) byte = FB_VAL(r, g, b);
		}
		s = (void *) slice;
	}
	fb_set(y, x, s, w);
}

static void xread(int fd, void *buf, int len)
{
	int nr = 0;
	int n;
	while (nr < len && (n = read(fd, buf + nr, len - nr)) > 0)
		nr += n;
	if (nr < len) {
		fprintf(stderr,"partial vnc read!\n");
		exit(99);
	}
}

static void skip(int fd, int len)
{
	int n;
	while (len > 0 && (n = read(fd, buf, MIN(len, sizeof(buf)))) > 0)
		len -= n;
}

static int vnc_event(int fd)
{
	struct vnc_rect uprect;
	union {
		struct vnc_server_fbup fbup;
		struct vnc_server_cuttext cuttext;
		struct vnc_server_colormap colormap;
	} msg;
	int j, n;

	if (read(fd, &msg.fbup.type, 1) != 1)
		return -1;
	switch (msg.fbup.type) {
	case VNC_SERVER_FBUP:
		xread(fd, &msg.fbup.pad, sizeof(msg.fbup) - 1);
		n = ntohs(msg.fbup.n);
		for (j = 0; j < n; j++) {
			int x, y, w, h, l, i;
			xread(fd, &uprect, sizeof(uprect));
			if (uprect.enc != 0) {
				fprintf(stderr,"Encoding not RAW: %d\n",
					ntohl(uprect.enc));
				return -1;
			}
			x = ntohs(uprect.x);
			y = ntohs(uprect.y);
			w = ntohs(uprect.w);
			h = ntohs(uprect.h);
			x -= oc;
			y -= or;
			i = 0;
			l = MIN(w, cols - x);
			if (x < 0) {
				l = MIN(w + x, cols);
				i = MIN(w, -x);
				x = 0;
			}
			if (l < 0)
				l = 0;
			for (; h--; y++) {
				int n = l;
				int xj = x;
				skip(fd, i * bpp);
				while (n > 0) {
					int j = MIN(n, MAXPIX);
					xread(fd, buf, j * bpp);
					if (y >= 0 && y < rows)
						if (!nodraw)
							drawfb(buf, xj, y, j);
					xj += j; n -= j;
				}
				skip(fd, (w - l - i) * bpp);
			}
		}
		break;
	case VNC_SERVER_BELL:
		break;
	case VNC_SERVER_CUTTEXT:
		xread(fd, &msg.cuttext.pad1, sizeof(msg.cuttext) - 1);
		skip(fd, ntohl(msg.cuttext.len));
		break;
	case VNC_SERVER_COLORMAP:
		xread(fd, &msg.colormap.pad, sizeof(msg.colormap) - 1);
		skip(fd, ntohs(msg.colormap.n) * 3 * 2);
		break;
	default:
		fprintf(stderr, "unknown vnc msg: %d\n", msg.fbup.type);
		return -1;
	}
	return 0;
}

static int rat_event(int fd, int ratfd)
{
	char ie[3];
	struct vnc_client_ratevent me = {VNC_CLIENT_RATEVENT};
	int mask = 0;
	int refresh = 2;
	if (read(ratfd, &ie, sizeof(ie)) != 3)
		return -1;
	/* ignore mouse movements when nodraw */
	if (nodraw)
		return 0;
	mc += ie[1];
	mr -= ie[2];
	if (mc < oc) {
		if ((oc -= cols / 5) < 0)
			oc = 0;
	}
	else if (mc >= oc + cols && oc + cols < srv_cols) {
		if ((oc += cols / 5) > srv_cols - cols)
			oc = srv_cols - cols;
	}
	else refresh--;
	if (mr < or) {
		if ((or -= rows / 5) < 0)
			or = 0;
	}
	else if (mr >= or + rows && or + rows < srv_rows) {
		if ((or += rows / 5) > srv_rows - rows)
			or = srv_rows - rows;
	}
	else refresh--;
	mc = MAX(oc, MIN(oc + cols - 1, mc));
	mr = MAX(or, MIN(or + rows - 1, mr));
	if (ie[0] & 0x01)
		mask |= VNC_BUTTON1_MASK;
	if (ie[0] & 0x04)
		mask |= VNC_BUTTON2_MASK;
	if (ie[0] & 0x02)
		mask |= VNC_BUTTON3_MASK;
	me.y = htons(mr);
	me.x = htons(mc);
	me.mask = mask;
	write(fd, &me, sizeof(me));
	if (refresh)
		vnc_refresh(fd, 0);
	return 0;
}

static int press(int fd, int key, int down)
{
	struct vnc_client_keyevent ke = {VNC_CLIENT_KEYEVENT};
	ke.key = htonl(key);
	ke.down = down;
	return write(fd, &ke, sizeof(ke));
}

static void showmsg(void)
{
	char *msg = "\x1b[H\t\t\t*** fbvnc ***\r";
	write(STDOUT_FILENO, msg, strlen(msg));
}

static int kbd_event(int fd, int kbdfd)
{
	char key[1024];
	int i, nr;

	if ((nr = read(kbdfd, key, sizeof(key))) <= 0 )
		return -1;
	for (i = 0; i < nr; i++) {
		int k = -1;
		int mod[4];
		int nmod = 0;
		switch (key[i]) {
		case 0x08:
		case 0x7f:
			k = 0xff08;
			break;
		case 0x09:
			k = 0xff09;
			break;
		case 0x1b:
			if (i + 2 < nr && key[i + 1] == '[') {
				if (key[i + 2] == 'A')
					k = 0xff52;
				if (key[i + 2] == 'B')
					k = 0xff54;
				if (key[i + 2] == 'C')
					k = 0xff53;
				if (key[i + 2] == 'D')
					k = 0xff51;
				if (key[i + 2] == 'H')
					k = 0xff50;
				if (k > 0) {
					i += 2;
					break;
				}
			}
			k = 0xff1b;
			if (i + 1 < nr) {
				mod[nmod++] = 0xffe9;
				k = key[++i];
				if (k == 0x03)	/* esc-^C: quit */
					return -1;
			}
			break;
		case 0x0d:
			k = 0xff0d;
			break;
 		case 0x0c:	/* ^L: redraw */
			vnc_refresh(fd, 0);
		case 0x0:	/* c-space: stop/start drawing */
			if (!nodraw) {
				nodraw = 1;
				showmsg();
			} else {
				nodraw = 0;
				vnc_refresh(fd, 0);
			}
		default:
			k = (unsigned char) key[i];
		}
		if ((k >= 'A' && k <= 'Z') || strchr(":\"<>?{}|+_()*&^%$#@!~", k))
			mod[nmod++] = 0xffe1;
		if (k >= 1 && k <= 26) {
			k = 'a' + k - 1;
			mod[nmod++] = 0xffe3;
		}
		if (k > 0) {
			int j;
			for (j = 0; j < nmod; j++)
				press(fd, mod[j], 1);
			press(fd, k, 1);
			press(fd, k, 0);
			for (j = 0; j < nmod; j++)
				press(fd, mod[j], 0);
		}
	}
	return 0;
}

static void term_setup(struct termios *ti)
{
	struct termios termios;
	char *hide = "\x1b[?25l";
	char *clear = "\x1b[2J";

	write(STDIN_FILENO, hide, strlen(hide));
	write(STDOUT_FILENO, clear, strlen(clear));
	showmsg();
	tcgetattr(0, &termios);
	*ti = termios;
	cfmakeraw(&termios);
	tcsetattr(0, TCSANOW, &termios);
}

static void term_cleanup(struct termios *ti)
{
	char *show = "\x1b[?25h";
	tcsetattr(0, TCSANOW, ti);
	write(STDIN_FILENO, show, strlen(show));
}

static int mainloop(int vnc_fd, int kbd_fd, int rat_fd)
{
	struct pollfd ufds[3];
	int pending = 0;
	int err;
	ufds[0].fd = kbd_fd;
	ufds[1].fd = vnc_fd;
	ufds[2].fd = rat_fd;
	ufds[0].events =
	ufds[1].events =
	ufds[2].events = POLLIN;
	vnc_refresh(vnc_fd, 0);
	while (1) {
		err = poll(ufds, 3, 500);
		if (err == -1 && errno != EINTR)
			break;
		if (!err)
			continue;
		err = -2;
		if (ufds[0].revents & POLLIN)
			if (kbd_event(vnc_fd, kbd_fd) == -1)
				break;
		err--;
		if (ufds[1].revents & POLLIN) {
			if (vnc_event(vnc_fd) == -1)
				break;
			pending = 0;
		}
		err--;
		if (ufds[2].revents & POLLIN)
			if (rat_event(vnc_fd, rat_fd) == -1)
				break;
		if (!pending++)
			vnc_refresh(vnc_fd, 1);
	}
	return err;
}

void show_usage(char *prog)
{
	printf("Usage : %s [options] server [port]\n", prog);
	printf("Valid options:\n");
	printf("\t[-b bpp-bits] specify bits per pixel\n");
	printf("\t[-p passwd-file] read encrypted password from this file\n");
	printf("\t[-w save-passwd-file] write encrypted password to this file\n");
	printf("\t[-h] show this help message\n");
	printf("\t[-v] show version information\n");
}

void show_version(char *prog)
{
	printf("%s "FBVNC_VERSION" - Uranus Zhou\n\n", prog);
}

int main(int argc, char * argv[])
{
	char *port = VNC_PORT;
	char *host = "127.0.0.1";
	struct termios ti;
	int vnc_fd, rat_fd, status;

	while (1)
	{
		int ch = getopt(argc, argv, "b:p:w:hv");
		if (ch == -1) break;
		switch (ch)
		{
		case 'b':
  			bpp = atoi(optarg) >> 3;
			break;
		case 'p':
			passwd_file = optarg;
			break;
		case 'w':
			passwd_save = optarg;
			break;
		case 'h':
			show_usage(argv[0]);
			return 0;
		case 'v':
			show_version(argv[0]);
			return 0;
		}
	}

	if (argc <= optind) {
		show_usage(argv[0]);
		return 1;
	}

	show_version(argv[0]);

	host = argv[optind];
	if (argc > optind + 1)
		port = argv[optind + 1];

	if ((vnc_fd = vnc_connect(host, port)) < 0) {
		fprintf(stderr, "could not connect! %s %s : %d\n",
			host,port,vnc_fd);
		return 1;
	}
	status = vnc_init(vnc_fd);
	if (status < 0) {
		close(vnc_fd);
		fprintf(stderr, "vnc init failed! %d\n", status);
		return 2;
	}
	term_setup(&ti);
	rat_fd = open("/dev/input/mice", O_RDONLY);

	status = mainloop(vnc_fd, 0, rat_fd);

	term_cleanup(&ti);
	vnc_free();
	close(vnc_fd);
	close(rat_fd);
	return 2 - status;
}
