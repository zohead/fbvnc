#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include "draw.h"

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))
#define NLEVELS		(1 << 16)

static int fd;
static void *fb;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static int bytes_per_pixel;

static int fb_len(void)
{
	return finfo.line_length * vinfo.yres_virtual;
}

static void fb_cmap_save(int save)
{
	static unsigned short red[NLEVELS], green[NLEVELS], blue[NLEVELS];
	struct fb_cmap cmap;

	if (finfo.visual == FB_VISUAL_TRUECOLOR)
		return;

	cmap.start = 0;
	cmap.len = NLEVELS;
	cmap.red = red;
	cmap.green = green;
	cmap.blue = blue;
	cmap.transp = NULL;
	ioctl(fd, save ? FBIOGETCMAP : FBIOPUTCMAP, &cmap);
}

void fb_cmap(void)
{
	struct fb_bitfield *color[3] = {
		&vinfo.blue, &vinfo.green, &vinfo.red
	};
	int eye_sensibility[3] = { 2, 0, 1 }; // higher=red, blue, lower=green
	struct fb_cmap cmap;
	unsigned short map[3][NLEVELS];
	int i, j, n, offset;

	if (finfo.visual == FB_VISUAL_TRUECOLOR)
		return;

	for (i = 0, n = vinfo.bits_per_pixel; i < 3; i++) {
		n -= color[eye_sensibility[i]]->length = n / (3 - i);
	}
	n = (1 << vinfo.bits_per_pixel);
	if (n > NLEVELS)
		n = NLEVELS;
	for (i = offset = 0; i < 3; i++) {
		int length = color[i]->length;
		color[i]->offset = offset;
		for (j = 0; j < n; j++) {
			int k = (j >> offset) << (16 - length);
			if (k == (0xFFFF << (16 - length)))
				k = 0xFFFF;
			map[i][j] = k;
		}
		offset += length;
	}

	cmap.start = 0;
	cmap.len = n;
	cmap.red = map[2];
	cmap.green = map[1];
	cmap.blue = map[0];
	cmap.transp = NULL;

	ioctl(fd, FBIOPUTCMAP, &cmap);
}

unsigned fb_mode(void)
{
	return (bytes_per_pixel << 16) | (vinfo.red.length << 8) |
		(vinfo.green.length << 4) | (vinfo.blue.length);
}

int fb_init(void)
{
	int err = 1;
	fd = open(FBDEV_PATH, O_RDWR);
	if (fd == -1)
		goto failed;
	err++;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == -1)
		goto failed;
	err++;
	if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1)
		goto failed;
	fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
	bytes_per_pixel = (vinfo.bits_per_pixel + 7) >> 3;
	fb = mmap(NULL, fb_len(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	err++;
	if (fb == MAP_FAILED)
		goto failed;
	fb_cmap_save(1);
	fb_cmap();
	return 0;
failed:
	perror("fb_init()");
	close(fd);
	return err;
}

void fb_free(void)
{
	fb_cmap_save(0);
	munmap(fb, fb_len());
	close(fd);
}

int fb_rows(void)
{
	return vinfo.yres;
}

int fb_cols(void)
{
	return vinfo.xres;
}

void *fb_mem(int r)
{
	return fb + (r + vinfo.yoffset) * finfo.line_length;
}

void fb_set(int r, int c, void *mem, int len)
{
	memcpy(fb_mem(r) + (c + vinfo.xoffset) * bytes_per_pixel,
		mem, len * bytes_per_pixel);
}

void fill_rgb_conv(int mode, struct rgb_conv *s)
{
	int bits;

	bits = mode & 0xF;  mode >>= 4;
	s->rshl = s->gshl = bits;
	s->bskp = 8 - bits; s->bmax = (1 << bits) -1;
	bits = mode & 0xF;  mode >>= 4;
	s->rshl += bits;
	s->gskp = 8 - bits; s->gmax = (1 << bits) -1;
	bits = mode & 0xF;
	s->rskp = 8 - bits; s->rmax = (1 << bits) -1;
}

unsigned fb_val(int r, int g, int b)
{
	static struct rgb_conv c;

	if (c.rshl == 0)
		fill_rgb_conv(fb_mode(), &c);
	return ((r >> c.rskp) << c.rshl) | ((g >> c.gskp) << c.gshl) 
					 | (b >> c.bskp);
}
