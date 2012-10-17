/* framebuffer device */
#define FBDEV_PATH	"/dev/fb0"

/* fb_mode() interpretation */
#define FBM_BPP(m)	(((m) >> 16) & 0x0f)
#define FBM_COLORS(m)	((m) & 0x0fff)

/* main functions */
int fb_init(void);
void fb_free(void);
unsigned fb_mode(void);
void *fb_mem(int r);
int fb_rows(void);
int fb_cols(void);
void fb_cmap(void);

/* helper functions */
struct rgb_conv {
	int rshl, gshl;
	int rskp, gskp, bskp;
	int rmax, gmax, bmax;
};
void fill_rgb_conv(int mode, struct rgb_conv *s);
void fb_set(int r, int c, void *mem, int len);
unsigned fb_val(int r, int g, int b);
