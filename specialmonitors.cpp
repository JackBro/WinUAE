#include "sysconfig.h"
#include "sysdeps.h"

#include <ctype.h>
#include <assert.h>

#include "options.h"
#include "xwin.h"
#include "custom.h"

static bool automatic;
static int monitor;

extern unsigned int bplcon0;
extern int interlace_seen;

static uae_u8 graffiti_palette[256 * 4];

STATIC_INLINE bool FR(struct vidbuffer *src, uae_u8 *dataline)
{
	if (src->pixbytes == 4)
		return (dataline[2] & 0x80) != 0;
	else
		return ((dataline[1] >> 7) & 1) != 0;
}
STATIC_INLINE bool FG(struct vidbuffer *src, uae_u8 *dataline)
{
	if (src->pixbytes == 4)
		return (dataline[1] & 0x80) != 0;
	else
		return ((dataline[1] >> 2) & 1) != 0;
}
STATIC_INLINE bool FB(struct vidbuffer *src, uae_u8 *dataline)
{
	if (src->pixbytes == 4)
		return (dataline[0] & 0x80) != 0;
	else
		return ((dataline[0] >> 4) & 1) != 0;
}
STATIC_INLINE bool FI(struct vidbuffer *src, uae_u8 *dataline)
{
	if (src->pixbytes == 4)
		return (dataline[0] & 0x10) != 0;
	else
		return ((dataline[0] >> 1) & 1) != 0;
}

STATIC_INLINE uae_u8 FIRGB(struct vidbuffer *src, uae_u8 *dataline)
{
	uae_u8 v = 0;
#if 1
	if (FI(src, dataline))
		v |= 1;
	if (FR(src, dataline))
		v |= 8;
	if (FG(src, dataline))
		v |= 4;
	if (FB(src, dataline))
		v |= 2;
#else
	if (FI(src, dataline))
		v |= 1 << scramble[scramble_counter * 4 + 0];
	if (FR(src, dataline))
		v |= 1 << scramble[scramble_counter * 4 + 1];
	if (FG(src, dataline))
		v |= 1 << scramble[scramble_counter * 4 + 2];
	if (FB(src, dataline))
		v |= 1 << scramble[scramble_counter * 4 + 3];
#endif
	return v;
}


STATIC_INLINE void PRGB(struct vidbuffer *dst, uae_u8 *dataline, uae_u8 r, uae_u8 g, uae_u8 b)
{
	if (dst->pixbytes == 4) {
		dataline[0] = b;
		dataline[1] = g;
		dataline[2] = r;
	} else {
		r >>= 3;
		g >>= 2;
		b >>= 3;
		((uae_u16*)dataline)[0] = (r << 11) | (g << 5) | b;
	}
}

static void clearmonitor(struct vidbuffer *dst)
{
	uae_u8 *p = dst->bufmem;
	for (int y = 0; y < dst->height_allocated; y++) {
		memset(p, 0, dst->width_allocated * dst->pixbytes);
		p += dst->rowbytes;
	}
}

static bool dctv(struct vidbuffer *src, struct vidbuffer *dst, bool doublelines, int oddlines)
{
	int y, x, vdbl, hdbl;
	int ystart, yend, isntsc;
	int xadd;

	isntsc = (beamcon0 & 0x20) ? 0 : 1;
	if (!(currprefs.chipset_mask & CSMASK_ECS_AGNUS))
		isntsc = currprefs.ntscmode ? 1 : 0;

	vdbl = gfxvidinfo.ychange;
	hdbl = gfxvidinfo.xchange;

	xadd = ((1 << 1) / hdbl) * src->pixbytes;

	ystart = isntsc ? VBLANK_ENDLINE_NTSC : VBLANK_ENDLINE_PAL;
	yend = isntsc ? MAXVPOS_NTSC : MAXVPOS_PAL;

	oddlines = 1;

	uae_u8 r, g, b;
	for (y = ystart; y < yend; y += 2) {
		int yoff = (((y * 2 + oddlines) - src->yoffset) / vdbl);
		if (yoff < 0)
			continue;
		if (yoff >= src->inheight)
			continue;
		uae_u8 *line = src->bufmem + yoff * src->rowbytes;
		uae_u8 *dstline = dst->bufmem + (((y * 2 + oddlines) - dst->yoffset) / vdbl) * dst->rowbytes;

		if (y < 60) {
			write_log(_T("%d:\n"), y);
			for (x = 22; x < 300; x += 1) {
				uae_u8 *s = line + ((x << 1) / hdbl) * src->pixbytes;
				write_log(_T("%01x"), FIRGB(src, s));
			}
			write_log(_T("*\n"));
			for (x = 21; x < 300; x += 1) {
				uae_u8 *s = line + ((x << 1) / hdbl) * src->pixbytes;
				write_log(_T("%01x"), FIRGB(src, s));
			}
			write_log(_T("\n"));
		}

		for (x = 1; x < src->inwidth; x += 4) {
			uae_u8 *s = line + ((x << 1) / hdbl) * src->pixbytes;
			uae_u8 *d = dstline + ((x << 1) / hdbl) * dst->pixbytes;
			uae_u8 *s2 = s + src->rowbytes;
			uae_u8 *d2 = d + dst->rowbytes;
			uae_u8 newval = FIRGB(src, s);

			r = newval << 4;
			g = newval << 4;
			b = newval << 4;

			for (int xx = 0; xx < 4; xx++) {
				PRGB(dst, d + dst->pixbytes * xx, r, g, b);
				PRGB(dst, d + dst->rowbytes + dst->pixbytes * xx, r, g, b);
			}


		}
	}
	dst->nativepositioning = true;
	return true;
}

static const uae_u8 ham_e_magic_cookie[] = { 0xa2, 0xf5, 0x84, 0xdc, 0x6d, 0xb0, 0x7f  };
static const uae_u8 ham_e_magic_cookie_reg = 0x14;
static const uae_u8 ham_e_magic_cookie_ham = 0x18;

static bool ham_e(struct vidbuffer *src, struct vidbuffer *dst, bool doublelines, int oddlines)
{
	int y, x, vdbl, hdbl;
	int ystart, yend, isntsc;
	int xadd;
	bool hameplus = currprefs.monitoremu == MONITOREMU_HAM_E_PLUS;

	isntsc = (beamcon0 & 0x20) ? 0 : 1;
	if (!(currprefs.chipset_mask & CSMASK_ECS_AGNUS))
		isntsc = currprefs.ntscmode ? 1 : 0;

	vdbl = gfxvidinfo.ychange;
	hdbl = gfxvidinfo.xchange;

	xadd = ((1 << 1) / hdbl) * src->pixbytes;

	ystart = isntsc ? VBLANK_ENDLINE_NTSC : VBLANK_ENDLINE_PAL;
	yend = isntsc ? MAXVPOS_NTSC : MAXVPOS_PAL;

	uae_u8 r, g, b;
	uae_u8 or, og, ob;
	int pcnt = 0;
	int bank = 0;
	int mode_active = 0;
	bool prevzeroline = false;
	int was_active = 0;
	bool cookie_line = false;
	int cookiestartx = 10000;
	for (y = ystart; y < yend; y++) {
		int yoff = (((y * 2 + oddlines) - src->yoffset) / vdbl);
		if (yoff < 0)
			continue;
		if (yoff >= src->inheight)
			continue;
		uae_u8 *line = src->bufmem + yoff * src->rowbytes;
		uae_u8 *dstline = dst->bufmem + (((y * 2 + oddlines) - dst->yoffset) / vdbl) * dst->rowbytes;

		bool getpalette = false;
		uae_u8 prev = 0;
		bool zeroline = true;
		int oddeven = 0;
		for (x = 0; x < src->inwidth; x++) {
			uae_u8 *s = line + ((x << 1) / hdbl) * src->pixbytes;
			uae_u8 *d = dstline + ((x << 1) / hdbl) * dst->pixbytes;
			uae_u8 *s2 = s + src->rowbytes;
			uae_u8 *d2 = d + dst->rowbytes;
			uae_u8 newval = FIRGB(src, s);
			uae_u8 val = prev | newval;

			if (newval)
				zeroline = false;
			if (val == ham_e_magic_cookie[0] && x + sizeof ham_e_magic_cookie + 1 < src->inwidth) {
				int i;
				for (i = 1; i <= sizeof ham_e_magic_cookie; i++) {
					uae_u8 val2 = (FIRGB(src, s + (i * 2 - 1) * xadd) << 4) | FIRGB(src, s + (i * 2 + 0) * xadd);
					if (i < sizeof ham_e_magic_cookie) {
						if (val2 != ham_e_magic_cookie[i])
							break;
					} else if (val2 == ham_e_magic_cookie_reg || val2 == ham_e_magic_cookie_ham) {
						mode_active = val2;
						getpalette = true;
						prevzeroline = false;
						cookiestartx = x - 1;
						x += i * 2;
						oddeven = 0;
						cookie_line = true;
					}
				}
				if (i == sizeof ham_e_magic_cookie + 1)
					continue;
			}

			if (!cookie_line && x == cookiestartx)
				oddeven = 0;

			if (oddeven) {
				if (getpalette) {
					graffiti_palette[pcnt] = val;
					pcnt++;
					if ((pcnt & 3) == 3)
						pcnt++;
					// 64 colors/line
					if ((pcnt & ((4 * 64) - 1)) == 0)
						getpalette = false;
					pcnt &= (4 * 256) - 1;
				}
				if (mode_active) {
					if (cookie_line || x < cookiestartx) {
						r = g = b = 0;
						or = og = ob = 0;
					} else {
						if (mode_active == ham_e_magic_cookie_reg) {
							uae_u8 *pal = &graffiti_palette[val * 4];
							r = pal[0];
							g = pal[1];
							b = pal[2];
						} else if (mode_active == ham_e_magic_cookie_ham) {
							int mode = val >> 6;
							int color = val & 63;
							if (mode == 0 && color <= 59) {
								uae_u8 *pal = &graffiti_palette[(bank + color) * 4];
								r = pal[0];
								g = pal[1];
								b = pal[2];
							} else if (mode == 0) {
								bank = (color & 3) * 64;
							} else if (mode == 1) {
								b = color << 2;
							} else if (mode == 2) {
								r = color << 2;
							} else if (mode == 3) {
								g = color << 2;
							}
						}
					}
					if (hameplus) {
						uae_u8 ar, ag, ab;

						if (abs(r - or) < 64 && abs(g - og) < 64 && abs(b - ob) < 64) {
							ar = (r + or) / 2;
							ag = (g + og) / 2;
							ab = (b + ob) / 2;
						} else {
							ar = r;
							ag = g;
							ab = b;
						}

						if (xadd > 2) {
							PRGB(dst, d - dst->pixbytes, ar, ag, ab);
							PRGB(dst, d, ar, ag, ab);
							PRGB(dst, d + 1 * dst->pixbytes, r, g, b);
							PRGB(dst, d + 2 * dst->pixbytes, r, g, b);
							if (doublelines) {
								PRGB(dst, d2 - dst->pixbytes, ar, ag, ab);
								PRGB(dst, d2, ar, ag, ab);
								PRGB(dst, d2 + 1 * dst->pixbytes, r, g, b);
								PRGB(dst, d2 + 2 * dst->pixbytes, r, g, b);
							}
						} else {
							PRGB(dst, d - dst->pixbytes, ar, ag, ab);
							PRGB(dst, d, r, g, b);
							if (doublelines) {
								PRGB(dst, d2 - dst->pixbytes, ar, ag, ab);
								PRGB(dst, d2, r, g, b);
							}
						}
						or = r;
						og = g;
						ob = b;
					} else {
						PRGB(dst, d - dst->pixbytes, r, g, b);
						PRGB(dst, d, r, g, b);
						if (xadd > 2) {
							PRGB(dst, d + 1 * dst->pixbytes, r, g, b);
							PRGB(dst, d + 2 * dst->pixbytes, r, g, b);
						}
						if (doublelines) {
							PRGB(dst, d2 - dst->pixbytes, r, g, b);
							PRGB(dst, d2, r, g, b);
							if (xadd > 2) {
								PRGB(dst, d2 + 1 * dst->pixbytes, r, g, b);
								PRGB(dst, d2 + 2 * dst->pixbytes, r, g, b);
							}
						}
					}
				} else {
					if (dst->pixbytes == 4) {
						((uae_u32*)d)[-1] = ((uae_u32*)s)[-1];
						((uae_u32*)d)[0] = ((uae_u32*)s)[0];
					} else {
						((uae_u16*)d)[-1] = ((uae_u16*)s)[-1];
						((uae_u16*)d)[0] = ((uae_u16*)s)[0];
					}
					if (doublelines) {
						if (dst->pixbytes == 4) {
							((uae_u32*)d2)[-1] = ((uae_u32*)s2)[-1];
							((uae_u32*)d2)[0] = ((uae_u32*)s2)[0];
						} else {
							((uae_u16*)d2)[-1] = ((uae_u16*)s2)[-1];
							((uae_u16*)d2)[0] = ((uae_u16*)s2)[0];
						}
					}
				}
			}

			oddeven = oddeven ? 0 : 1;
			prev = val << 4;
		}

		if (cookie_line) {
			// Erase magic cookie. I assume real HAM-E would erase it
			// because not erasing it would look really ugly.
			memset(dstline, 0, dst->outwidth * dst->pixbytes);
			if (doublelines)
				memset(dstline + dst->rowbytes, 0, dst->outwidth * dst->pixbytes);
		}

		cookie_line = false;
		if (mode_active)
			was_active = mode_active;
		if (zeroline) {
			if (prevzeroline) {
				mode_active = 0;
				pcnt = 0;
				cookiestartx = 10000;
			}
			prevzeroline = true;
		} else {
			prevzeroline = false;
		}

	}

	if (was_active) {
		dst->nativepositioning = true;
		if (monitor != MONITOREMU_HAM_E) {
			monitor = MONITOREMU_HAM_E;
			write_log (_T("HAM-E mode, %s\n"), was_active == ham_e_magic_cookie_reg ? _T("REG") : _T("HAM"));
		}
	}

	return was_active != 0;
}

static bool graffiti(struct vidbuffer *src, struct vidbuffer *dst)
{
	int y, x;
	int ystart, yend, isntsc;
	int xstart, xend;
	uae_u8 *srcbuf, *srcend;
	uae_u8 *dstbuf;
	bool command, hires, found;
	int xadd, xpixadd, extrapix;
	int waitline = 0, dbl;
	uae_u8 read_mask = 0xff, color = 0, color2 = 0;

	if (!(bplcon0 & 0x0100)) // GAUD
		return false;

	command = true;
	found = false;
	isntsc = (beamcon0 & 0x20) ? 0 : 1;
	if (!(currprefs.chipset_mask & CSMASK_ECS_AGNUS))
		isntsc = currprefs.ntscmode ? 1 : 0;

	dbl = gfxvidinfo.ychange == 1 ? 2 : 1;

	ystart = isntsc ? VBLANK_ENDLINE_NTSC : VBLANK_ENDLINE_PAL;
	yend = isntsc ? MAXVPOS_NTSC : MAXVPOS_PAL;
	if (src->yoffset >= (ystart << VRES_MAX))
		ystart = src->yoffset >> VRES_MAX;

	xadd = gfxvidinfo.xchange == 1 ? src->pixbytes * 2 : src->pixbytes;
	xpixadd = gfxvidinfo.xchange == 1 ? 4 : 2;

	xstart = 0x1c * 2 + 1;
	xend = 0xf0 * 2 + 1;
	if (!(currprefs.chipset_mask & CSMASK_AGA)) {
		xstart++;
		xend++;
	}

	srcbuf = src->bufmem + (((ystart << VRES_MAX) - src->yoffset) / gfxvidinfo.ychange) * src->rowbytes + (((xstart << RES_MAX) - src->xoffset) / gfxvidinfo.xchange) * src->pixbytes;
	srcend = src->bufmem + (((yend << VRES_MAX) - src->yoffset) / gfxvidinfo.ychange) * src->rowbytes;
	extrapix = 0;

	dstbuf = dst->bufmem + (((ystart << VRES_MAX) - src->yoffset) / gfxvidinfo.ychange) * dst->rowbytes + (((xstart << RES_MAX) - src->xoffset) / gfxvidinfo.xchange) * dst->pixbytes;

	y = 0;
	while (srcend > srcbuf && dst->bufmemend > dstbuf) {
		uae_u8 *srcp = srcbuf + extrapix;
		uae_u8 *dstp = dstbuf;

		x = xstart;
		while (x < xend) {
			
			uae_u8 mask = 0x80;
			uae_u8 chunky[4] = { 0, 0, 0, 0 };
			while (mask) {
				if (FR(src, srcp)) // R
					chunky[3] |= mask;
				if (FG(src, srcp)) // G
					chunky[2] |= mask;
				if (FB(src, srcp)) // B
					chunky[1] |= mask;
				if (FI(src, srcp)) // I
					chunky[0] |= mask;
				srcp += xadd;
				mask >>= 1;
			}

			if (command) {
				if (chunky[0] || chunky[1] || chunky[2] || chunky[3] || found) {
					for (int pix = 0; pix < 2; pix++) {
						uae_u8 cmd = chunky[pix * 2 + 0];
						uae_u8 parm = chunky[pix * 2 + 1];

						if (automatic && cmd >= 0x40)
							return false;
						if (cmd != 0)
							found = true;
						if (cmd & 8) {
							command = false;
							dbl = 1;
							waitline = 2;
							if (0 && (cmd & 16)) {
								hires = true;
								xadd /= 2;
								xpixadd /= 2;
								extrapix = -4 * src->pixbytes;
							} else {
								hires = false;
							}
							if (xpixadd == 0) // shres needed
								return false;
							if (monitor != MONITOREMU_GRAFFITI)
								clearmonitor(dst);
						} else if (cmd & 4) {
							if ((cmd & 3) == 1) {
								read_mask = parm;
							} else if ((cmd & 3) == 2) {
								graffiti_palette[color * 4 + color2] = (parm << 2) | (parm & 3);
								color2++;
								if (color2 == 3) {
									color2 = 0;
									color++;
								}
							} else if ((cmd & 3) == 0) {
								color = parm;
								color2 = 0;
							}
						}
					}
				}

				memset(dstp, 0, dst->pixbytes * 4 * 2);
				dstp += dst->pixbytes * 4 * 2;

			} else if (waitline) {
			
				memset(dstp, 0, dst->pixbytes * 4 * 2);
				dstp += dst->pixbytes * 4 * 2;
			
			} else {

				for (int pix = 0; pix < 4; pix++) {
					uae_u8 r, g, b, c;
					
					c = chunky[pix] & read_mask;
					r = graffiti_palette[c * 4 + 0];
					g = graffiti_palette[c * 4 + 1];
					b = graffiti_palette[c * 4 + 2];
					PRGB(dst, dstp, r, g, b);
					dstp += dst->pixbytes;
					PRGB(dst, dstp, r, g, b);
					dstp += dst->pixbytes;
					
					if (gfxvidinfo.xchange == 1 && !hires) {
						PRGB(dst, dstp, r, g, b);
						dstp += dst->pixbytes;
						PRGB(dst, dstp, r, g, b);
						dstp += dst->pixbytes;
					}
				}

			}

			x += xpixadd;
		}

		y++;
		srcbuf += src->rowbytes * dbl;
		dstbuf += dst->rowbytes * dbl;
		if (waitline > 0)
			waitline--;
	}

	dst->nativepositioning = true;

	if (monitor != MONITOREMU_GRAFFITI) {
		monitor = MONITOREMU_GRAFFITI;
		write_log (_T("GRAFFITI %s mode\n"), hires ? _T("hires") : _T("lores"));
	}

	return true;
}

/* A2024 information comes from US patent 4851826 */

static bool a2024(struct vidbuffer *src, struct vidbuffer *dst)
{
	int y;
	uae_u8 *srcbuf, *dstbuf;
	uae_u8 *dataline;
	int px, py, doff, pxcnt, dbl;
	int panel_width, panel_width_draw, panel_height, srcxoffset;
	bool f64, interlace, expand, wpb, less16;
	uae_u8 enp, dpl;
	bool hires, ntsc, found;
	int idline;
	int total_width, total_height;
	
	dbl = gfxvidinfo.ychange == 1 ? 2 : 1;
	doff = (128 * 2 / gfxvidinfo.xchange) * src->pixbytes;
	found = false;

	for (idline = 21; idline <= 29; idline += 8) {
		if (src->yoffset > (idline << VRES_MAX))
			continue;
		// min 178 max 234
		dataline = src->bufmem + (((idline << VRES_MAX) - src->yoffset) / gfxvidinfo.ychange) * src->rowbytes + (((200 << RES_MAX) - src->xoffset) / gfxvidinfo.xchange) * src->pixbytes;

#if 0
		write_log (_T("%02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x\n"),
			dataline[0 * doff + 0], dataline[0 * doff + 1], dataline[0 * doff + 2],
			dataline[1 * doff + 0], dataline[1 * doff + 1], dataline[1 * doff + 2],
			dataline[2 * doff + 0], dataline[2 * doff + 1], dataline[2 * doff + 2],
			dataline[3 * doff + 0], dataline[3 * doff + 1], dataline[3 * doff + 2]);
#endif

		if (FB(src, &dataline[0 * doff]))			// 0:B = 0
			continue;
		if (!FI(src, &dataline[0 * doff]))			// 0:I = 1
			continue;
		if (FI(src, &dataline[2 * doff]))			// 2:I = 0
			continue;
		if (!FI(src, &dataline[3 * doff]))			// 3:I = 1
			continue;

		ntsc = idline < 26;
		found = true;
		break;
	}

	if (!found)
		return false;

	px = py = 0;
	if (FB(src, &dataline[1 * doff])) // 1:B FN2
		px |= 2;
	if (FG(src, &dataline[1 * doff])) // 1:G FN1
		px |= 1;
	if (FR(src, &dataline[1 * doff])) // 1:R FN0
		py |= 1;

	f64 = FR(src, &dataline[0 * doff]) != 0;		// 0:R
	interlace = FG(src, &dataline[0 * doff]) != 0;	// 0:G (*Always zero)
	expand = FI(src, &dataline[1 * doff]) != 0;		// 1:I (*Always set)
	enp = FR(src, &dataline[2 * doff]) ? 1 : 0;		// 2:R (*ENP=3)
	enp |= FG(src, &dataline[2 * doff]) ? 2 : 0;	// 2:G
	wpb = FB(src, &dataline[2 * doff]) != 0;		// 2:B (*Always zero)
	dpl = FR(src, &dataline[3 * doff]) ? 1 : 0;		// 3:R (*DPL=3)
	dpl |= FG(src, &dataline[3 * doff]) ? 2 : 0;	// 3:G
	less16 = FB(src, &dataline[3 * doff]) != 0;		// 3:B

	/* (*) = AOS A2024 driver static bits. Not yet implemented in emulation. */

	if (f64) {
		panel_width = 336;
		panel_width_draw = px == 2 ? 352 : 336;
		pxcnt = 3;
		hires = false;
		srcxoffset = 113;
		if (px > 2)
			return false;
		total_width = 336 + 336 + 352;
	} else {
		panel_width = 512;
		panel_width_draw = 512;
		pxcnt = 2;
		hires = true;
		srcxoffset = 129;
		if (px > 1)
			return false;
		total_width = 512 + 512;
	}
	panel_height = ntsc ? 400 : 512;

	if (monitor != MONITOREMU_A2024) {
		clearmonitor(dst);
	}

#if 0
	write_log (_T("0 = F6-4:%d INTERLACE:%d\n"), f64, interlace);
	write_log (_T("1 = FN:%d EXPAND:%d\n"), py + px *2, expand);
	write_log (_T("2 = ENP:%d WPB=%d\n"), enp, wpb);
	write_log (_T("3 = DPL:%d LESS16=%d\n"), dpl, less16);
#endif
#if 0
	write_log (_T("%02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x %dx%d\n"),
		dataline[0 * doff + 0], dataline[0 * doff + 1], dataline[0 * doff + 2],
		dataline[1 * doff + 0], dataline[1 * doff + 1], dataline[1 * doff + 2],
		dataline[2 * doff + 0], dataline[2 * doff + 1], dataline[2 * doff + 2],
		dataline[3 * doff + 0], dataline[3 * doff + 1], dataline[3 * doff + 2],
		px, py);
#endif

	if (less16) {
		total_width -= 16;
		if (px == pxcnt - 1)
			panel_width_draw -= 16;
	}
	total_height = panel_height * dbl;
	
	srcbuf = src->bufmem + (((44 << VRES_MAX) - src->yoffset) / gfxvidinfo.ychange) * src->rowbytes + (((srcxoffset << RES_MAX) - src->xoffset) / gfxvidinfo.xchange) * src->pixbytes;
	dstbuf = dst->bufmem + py * (panel_height / gfxvidinfo.ychange) * dst->rowbytes + px * ((panel_width * 2) / gfxvidinfo.xchange) * dst->pixbytes;

	for (y = 0; y < (panel_height / (dbl == 1 ? 1 : 2)) / gfxvidinfo.ychange; y++) {
#if 0
		memcpy (dstbuf, srcbuf, ((panel_width * 2) / gfxvidinfo.xchange) * dst->pixbytes);
#else
		uae_u8 *srcp = srcbuf;
		uae_u8 *dstp1 = dstbuf;
		uae_u8 *dstp2 = dstbuf + dst->rowbytes;
		int x;
		for (x = 0; x < (panel_width_draw * 2) / gfxvidinfo.xchange; x++) {
			uae_u8 c1 = 0, c2 = 0;
			if (FR(src, srcp)) // R
				c1 |= 2;
			if (FG(src, srcp)) // G
				c2 |= 2;
			if (FB(src, srcp)) // B
				c1 |= 1;
			if (FI(src, srcp)) // I
				c2 |= 1;
			if (dpl == 0) {
				c1 = c2 = 0;
			} else if (dpl == 1) {
				c1 &= 1;
				c1 |= c1 << 1;
				c2 &= 1;
				c2 |= c2 << 1;
			} else if (dpl == 2) {
				c1 &= 2;
				c1 |= c1 >> 1;
				c2 &= 2;
				c2 |= c2 >> 1;
			}
			if (dbl == 1) {
				c1 = (c1 + c2 + 1) / 2;
				c1 = (c1 << 6) | (c1 << 4) | (c1 << 2) | (c1 << 0);
				PRGB(dst, dstp1, c1, c1, c1);
			} else {
				c1 = (c1 << 6) | (c1 << 4) | (c1 << 2) | (c1 << 0);
				c2 = (c2 << 6) | (c2 << 4) | (c2 << 2) | (c2 << 0);
				PRGB(dst, dstp1, c1, c1, c1);
				PRGB(dst, dstp2, c2, c2, c2);
				dstp2 += dst->pixbytes;
			}
			srcp += src->pixbytes;
			if (!hires)
				srcp += src->pixbytes;
			dstp1 += dst->pixbytes;
		}
#endif
		srcbuf += src->rowbytes * dbl;
		dstbuf += dst->rowbytes * dbl;
	}

	total_width /= 2;
	total_width <<= currprefs.gfx_resolution;

	dst->outwidth = total_width;
	dst->outheight = total_height;
	dst->inwidth = total_width;
	dst->inheight = total_height;
	dst->inwidth2 = total_width;
	dst->inheight2 = total_height;
	dst->nativepositioning = false;

	if (monitor != MONITOREMU_A2024) {
		monitor = MONITOREMU_A2024;
		write_log (_T("A2024 %dHz %s mode\n"), hires ? 10 : 15, ntsc ? _T("NTSC") : _T("PAL"));
	}

	return true;
}

static bool emulate_specialmonitors2(struct vidbuffer *src, struct vidbuffer *dst)
{
	if (currprefs.monitoremu == MONITOREMU_AUTO) {
		automatic = true;
		bool v = a2024(src, dst);
		if (!v)
			v = graffiti(src, dst);
		return v;
	} else if (currprefs.monitoremu == MONITOREMU_A2024) {
		automatic = false;
		return a2024(src, dst);
	} else if (currprefs.monitoremu == MONITOREMU_GRAFFITI) {
		automatic = false;
		return graffiti(src, dst);
	} else if (currprefs.monitoremu == MONITOREMU_DCTV) {
		automatic = false;
		return dctv(src, dst, false, 0);
	} else if (currprefs.monitoremu == MONITOREMU_HAM_E || currprefs.monitoremu == MONITOREMU_HAM_E_PLUS) {
		bool v;
		automatic = false;
		if (interlace_seen) {
			v = ham_e(src, dst, false, 0);
			v |= ham_e(src, dst, false, 1);
		} else {
			v = ham_e(src, dst, true, 0);
		}
		return v;
	}
	return false;
}


bool emulate_specialmonitors(struct vidbuffer *src, struct vidbuffer *dst)
{
	if (!emulate_specialmonitors2(src, dst)) {
		if (monitor) {
			clearmonitor(dst);
			monitor = 0;
			write_log (_T("Native mode\n"));
		}
		return false;
	}
	return true;
}
