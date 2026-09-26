// Ahead-of-time TXD converter: PC (D3D8) texture dictionary in, GameCube
// native texture dictionary out.
//
// This is the whole point of the exercise. Converting D3D8 rasters to GX at
// stream time keeps a D3D raster, a full RGBA8 Image and an RGBA8 staging
// buffer live at once for every texture the game loads, and that churn is what
// shattered the console heap — measured 12.3MB live across ~16300 allocations
// with 4.4MB free split into ~9200 chunks. Doing it here means the console
// reads a tiled blob straight into its final buffer and does nothing else.
//
// dca3 (https://gitlab.com/skmp/dca3-game) does exactly this for the Dreamcast
// with src/tools/texconv.cpp, running the game's own librw against an HLE
// layer. We do not need the HLE layer: the tiling is plain byte manipulation
// over rw::Raster, so it compiles for the host untouched.
//
// Textures keep their full resolution unless --shrink asks otherwise (09-03:
// MEM1 ran out with the world at full size, and the cut was the user's call).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <rw.h>

using namespace rw;
typedef uint8 u8;

enum { GXFMT_IA4 = 0x2, GXFMT_RGB5A3 = 0x5, GXFMT_CMPR = 0xE };

// Largest texture axis kept, in texels. 512 is the original; halving to 256
// quarters the bytes of every texture above it. Set with --max-dim.
static int gMaxDim = 512;
// --shrink H PCT: every texture taller than H texels is resampled to PCT% on
// both axes. Dimensions round up to the 8-texel tile so w == tw and the tiler
// never pads (the runtime builds its TexObj from the raster's real w/h).
static int gShrinkH = 0, gShrinkPct = 100;

// ---- the same tiling the console backend uses, byte for byte ---------------
static inline void
sampleSrc(const u8 *src, int w, int h, int dx, int dy, int tw, int th,
	u8 *r, u8 *g, u8 *b, u8 *a)
{
	int sx = dx * w / tw, sy = dy * h / th;
	if(dx >= tw || dy >= th || sx >= w || sy >= h){ *r=*g=*b=*a=0; return; }
	const u8 *p = src + ((size_t)sy*w + sx)*4;
	*r = p[0]; *g = p[1]; *b = p[2]; *a = p[3];
}

static inline uint16 to565(u8 r, u8 g, u8 b)
{ return (uint16)(((r>>3)<<11) | ((g>>2)<<5) | (b>>3)); }

static inline u8 luma(u8 r, u8 g, u8 b)
{ return (u8)((r*77 + g*150 + b*29) >> 8); }

// GX stores these 16-bit fields big-endian. The console wrote them with plain
// stores because it *is* big-endian; a little-endian host has to swap, or the
// blob is byte-reversed on hardware.
static inline void put16be(u8 *p, uint16 v){ p[0] = v>>8; p[1] = v & 0xFF; }

static void
tileRGB5A3(u8 *dst, const u8 *src, int w, int h, int tw, int th)
{
	for(int ty = 0; ty < th; ty += 4)
	for(int tx = 0; tx < tw; tx += 4){
		for(int y = 0; y < 4; y++)
		for(int x = 0; x < 4; x++){
			u8 r,g,b,a;
			sampleSrc(src, w, h, tx+x, ty+y, tw, th, &r,&g,&b,&a);
			uint16 v = a >= 0xE0 ?
			    (uint16)(0x8000 | ((r>>3)<<10) | ((g>>3)<<5) | (b>>3)) :
			    (uint16)(((a>>5)<<12) | ((r>>4)<<8) | ((g>>4)<<4) | (b>>4));
			put16be(dst + (y*4 + x)*2, v);
		}
		dst += 32;
	}
}

// IA4: 4-bit intensity + 4-bit alpha, one byte per texel (high nibble alpha,
// low nibble intensity — the order the GP decodes), tiles 8 wide x 4 high.
// Half of RGB5A3 for a grayscale-plus-alpha texture, and no worse: RGB5A3
// stores any texel with a soft alpha as 4-bit colour / 3-bit alpha, IA4 gives
// 4-bit grey / 4-bit alpha. dca3's rule — the smallest native format each
// texture fits — applied to the GameCube's IA4.
static void
tileIA4(u8 *dst, const u8 *src, int w, int h, int tw, int th)
{
	for(int ty = 0; ty < th; ty += 4)
	for(int tx = 0; tx < tw; tx += 8){
		for(int y = 0; y < 4; y++)
		for(int x = 0; x < 8; x++){
			u8 r,g,b,a;
			sampleSrc(src, w, h, tx+x, ty+y, tw, th, &r,&g,&b,&a);
			dst[y*8 + x] = (u8)((a & 0xF0) | (luma(r,g,b) >> 4));
		}
		dst += 32;
	}
}

// The CMPR palette exactly as the GP (and decodeNative) expands it.
static void
cmprPalette(uint16 c0, uint16 c1, int pal[4][4])
{
	for(int k = 0; k < 2; k++){
		uint16 v = k ? c1 : c0;
		int r = v >> 11, g = (v >> 5) & 63, b = v & 31;
		pal[k][0] = (r << 3) | (r >> 2);
		pal[k][1] = (g << 2) | (g >> 4);
		pal[k][2] = (b << 3) | (b >> 2);
		pal[k][3] = 255;
	}
	for(int c = 0; c < 3; c++){
		pal[2][c] = c0 > c1 ? (2*pal[0][c] + pal[1][c])/3 : (pal[0][c] + pal[1][c])/2;
		pal[3][c] = c0 > c1 ? (pal[0][c] + 2*pal[1][c])/3 : pal[2][c];
	}
	pal[2][3] = 255;
	pal[3][3] = c0 > c1 ? 255 : 0;
}

// One S3TC block. The endpoints are the two texels furthest apart along the
// block's principal colour axis — a min/max box per channel invented colours
// the block never had (red and green texels became black and yellow) — and
// every texel takes the nearest of the palette entries the GP will decode.
// Transparent texels (alpha < 128) force the 3-colour mode (c0 <= c1).
static void
encodeCMPRBlock(u8 *dst, u8 px[16][4])
{
	bool trans = false;
	int n = 0;
	float mean[3] = {0,0,0};
	for(int i = 0; i < 16; i++){
		if(px[i][3] < 128){ trans = true; continue; }
		for(int c = 0; c < 3; c++) mean[c] += px[i][c];
		n++;
	}
	uint16 c0 = 0, c1 = 0;
	if(n > 0){
		for(int c = 0; c < 3; c++) mean[c] /= n;
		float cov[3][3] = {{0}};
		for(int i = 0; i < 16; i++){
			if(px[i][3] < 128) continue;
			float d[3] = { px[i][0]-mean[0], px[i][1]-mean[1], px[i][2]-mean[2] };
			for(int a = 0; a < 3; a++)
				for(int b = 0; b < 3; b++) cov[a][b] += d[a]*d[b];
		}
		float axis[3] = {1.0f, 1.0f, 1.0f};
		for(int it = 0; it < 8; it++){
			float nx[3];
			for(int a = 0; a < 3; a++) nx[a] = cov[a][0]*axis[0] + cov[a][1]*axis[1] + cov[a][2]*axis[2];
			float len = sqrtf(nx[0]*nx[0] + nx[1]*nx[1] + nx[2]*nx[2]);
			if(len < 1e-6f) break;   // flat block: any axis will do
			for(int a = 0; a < 3; a++) axis[a] = nx[a]/len;
		}
		int lo = -1, hi = -1;
		float tlo = 1e30f, thi = -1e30f;
		for(int i = 0; i < 16; i++){
			if(px[i][3] < 128) continue;
			float t = (px[i][0]-mean[0])*axis[0] + (px[i][1]-mean[1])*axis[1] + (px[i][2]-mean[2])*axis[2];
			if(t < tlo){ tlo = t; lo = i; }
			if(t > thi){ thi = t; hi = i; }
		}
		uint16 a = to565(px[hi][0], px[hi][1], px[hi][2]);
		uint16 b = to565(px[lo][0], px[lo][1], px[lo][2]);
		if(trans || a == b){ c0 = a < b ? a : b; c1 = a < b ? b : a; }   // 3-colour mode
		else{ c0 = a > b ? a : b; c1 = a > b ? b : a; }                 // 4-colour mode
	}
	int pal[4][4];
	cmprPalette(c0, c1, pal);
	int last = c0 > c1 ? 3 : 2;   // index 3 is transparent in the 3-colour mode
	put16be(dst, c0);
	put16be(dst+2, c1);
	for(int row = 0; row < 4; row++){
		u8 byte = 0;
		for(int x = 0; x < 4; x++){
			u8 *p = px[row*4 + x];
			int v = 3;
			if(p[3] >= 128){
				int best = 1 << 30;
				for(int k = 0; k <= last; k++){
					int dr = p[0]-pal[k][0], dg = p[1]-pal[k][1], db = p[2]-pal[k][2];
					int e = dr*dr + dg*dg + db*db;
					if(e < best){ best = e; v = k; }
				}
			}
			byte |= v << (6 - 2*x);
		}
		dst[4+row] = byte;
	}
}

static void
tileCMPR(u8 *dst, const u8 *src, int w, int h, int tw, int th)
{
	for(int ty = 0; ty < th; ty += 8)
	for(int tx = 0; tx < tw; tx += 8)
	for(int sub = 0; sub < 4; sub++){
		int bx = tx + (sub & 1)*4, by = ty + (sub >> 1)*4;
		u8 px[16][4];
		for(int i = 0; i < 16; i++)
			sampleSrc(src, w, h, bx + (i&3), by + (i>>2), tw, th,
			    &px[i][0], &px[i][1], &px[i][2], &px[i][3]);
		encodeCMPRBlock(dst, px);
		dst += 8;
	}
}

static u8 *decodeNative(const u8 *src, int w, int h, int fmt);
static int gLosslessCMPR, gEncodedCMPR;   // for the summary line

// DXT1 and CMPR are the same S3TC block: two RGB565 endpoints and sixteen
// 2-bit indices, with the same 3-colour + transparent mode when c0 <= c1.
// Only the byte order (big-endian), the index order in a row (first texel in
// the high bits) and the tiling (2x2 blocks per 8x8 tile) differ, so the PC
// blocks move over bit for bit instead of being decoded and re-encoded by
// tileCMPR's min/max fit. The result is decoded and compared with the source
// decode; any mismatch returns false and the caller re-encodes.
static bool
transcodeDXT1(u8 *dst, const u8 *src, const u8 *rgba, int w, int h, bool opaque)
{
	int bpr = w/4;
	u8 *out = dst;
	for(int ty = 0; ty < h; ty += 8)
	for(int tx = 0; tx < w; tx += 8)
	for(int sub = 0; sub < 4; sub++, out += 8){
		const u8 *b = src + ((size_t)((ty + (sub>>1)*4)/4)*bpr + (tx + (sub&1)*4)/4)*8;
		uint16 c0 = b[0] | b[1]<<8, c1 = b[2] | b[3]<<8;
		out[0] = b[1]; out[1] = b[0]; out[2] = b[3]; out[3] = b[2];
		for(int row = 0; row < 4; row++){
			u8 s = b[4+row], o = 0;
			for(int x = 0; x < 4; x++){
				int v = (s >> 2*x) & 3;
				// An opaque (C565) raster draws index 3 of the 3-colour mode
				// as black; CMPR would make it transparent.
				if(opaque && c0 <= c1 && v == 3) return false;
				o |= v << (6 - 2*x);
			}
			out[4+row] = o;
		}
	}
	u8 *back = decodeNative(dst, w, h, GXFMT_CMPR);
	bool same = true;
	for(size_t i = 0; i < (size_t)w*h*4 && same; i++){
		int d = back[i] - rgba[i];
		same = (i & 3) == 3 ? d == 0 : d >= -4 && d <= 4;   // the decoders round the 1/3 mixes differently
	}
	free(back);
	return same;
}

// ---- native GX texture chunk, matching gxraster.cpp's reader ---------------
enum { GXNATIVE_HEADER = 88 };

struct Conv {
	char name[32], mask[32];
	uint32 filterAddressing, format;
	int tw, th;
	u8 gxFmt;
	u8 *tiled;
	uint32 size;
	int levels;   // mip levels in tiled, level 0 first
};

// Area-average resample to any smaller size: each destination texel is the
// coverage-weighted mean of the source texels it overlaps, colour weighted by
// alpha so transparent (usually black) texels do not darken cut-out edges.
// The 2x2 halving loop in convertImage is this filter's power-of-two case.
static u8*
resampleArea(u8 *src, int w, int h, int nw, int nh)
{
	u8 *dst = (u8*)malloc((size_t)nw*nh*4);
	double sx = (double)w/nw, sy = (double)h/nh;
	for(int y = 0; y < nh; y++){
		double y0 = y*sy, y1 = y0 + sy;
		for(int x = 0; x < nw; x++){
			double x0 = x*sx, x1 = x0 + sx;
			double rgb[3] = {0,0,0}, plain[3] = {0,0,0}, alpha = 0, cover = 0;
			for(int j = (int)y0; j < h && j < y1; j++){
				double wy = fmin(y1, j+1.0) - fmax(y0, (double)j);
				for(int i = (int)x0; i < w && i < x1; i++){
					double wt = wy*(fmin(x1, i+1.0) - fmax(x0, (double)i));
					const u8 *p = src + ((size_t)j*w + i)*4;
					for(int c = 0; c < 3; c++){ rgb[c] += p[c]*p[3]*wt; plain[c] += p[c]*wt; }
					alpha += p[3]*wt;
					cover += wt;
				}
			}
			// Fully transparent texels keep the artists' bled colour (plain
			// mean) so GX bilinear filtering at cut-out edges does not blend
			// towards black the way a zeroed RGB would.
			u8 *o = dst + ((size_t)y*nw + x)*4;
			for(int c = 0; c < 3; c++)
				o[c] = alpha > 0 ? (u8)(rgb[c]/alpha + 0.5) :
				       cover > 0 ? (u8)(plain[c]/cover + 0.5) : 0;
			o[3] = cover > 0 ? (u8)(alpha/cover + 0.5) : 0;
		}
	}
	free(src);
	return dst;
}

// Takes an Image rather than a Texture so the same tiling serves both inputs:
// a dictionary read off disc, and a loose TGA. Destroys img.
static bool
convertImage(Image *img, const char *name, const char *mask,
	uint32 filterAddressing, uint32 format, Conv *out, const u8 *dxt1 = nil)
{
	if(img == nil) return false;
	img->unpalettize(true);
	int w = img->width, h = img->height;
	const int srcW = w, srcH = h;   // dxt1 describes the texture at this size
	if(w <= 0 || h <= 0){ img->destroy(); return false; }

	// Halve until both axes are inside gMaxDim, keeping powers of two: GX
	// wrapping and mipmapping want them, and 512 -> 480 would break that for a
	// 12% saving while 512 -> 256 is a clean quarter of the bytes.
	//
	// Texture data is 191.6MB of the 329.8MB archive — 58% — so this is the
	// only lever on disc size that is worth pulling, and on a 480-line display
	// the detail being dropped is not detail anyone can see.
	int shift = 0;
	while((w >> shift) > gMaxDim || (h >> shift) > gMaxDim){
		if((w >> shift) <= 4 || (h >> shift) <= 4)
			break;   // never reduce below a single compression block
		shift++;
	}

	u8 *rgba = (u8*)malloc((size_t)w*h*4);
	bool gradientAlpha = false;
	// Grayscale test, for the IA4 decision below: count opaque texels whose
	// channels disagree by more than a hair. A texture that is (near) pure
	// grey wastes 8 of RGB5A3's 16 bits on three equal colour channels.
	long opaque = 0, coloured = 0;
	for(int y = 0; y < h; y++)
	for(int x = 0; x < w; x++){
		u8 *s = img->pixels + (size_t)y*img->stride + (size_t)x*img->bpp;
		u8 *d = rgba + ((size_t)y*w + x)*4;
		d[0]=s[0]; d[1]=s[1]; d[2]=s[2];
		d[3] = img->bpp >= 4 ? s[3] : 255;
		if(d[3] > 16 && d[3] < 240) gradientAlpha = true;
		if(d[3] > 16){
			opaque++;
			int mx = d[0] > d[1] ? d[0] : d[1]; if(d[2] > mx) mx = d[2];
			int mn = d[0] < d[1] ? d[0] : d[1]; if(d[2] < mn) mn = d[2];
			if(mx - mn > 8) coloured++;
		}
	}
	// Conservative: only a texture that is essentially all grey (<1% of its
	// opaque texels carry real colour) takes IA4. The foliage in generic.txd
	// is 7-62% coloured and stays RGB5A3; fonts and effect sprites are 0%.
	bool grayscale = opaque > 0 && coloured*100 < opaque;

	// Box filter, one halving at a time. Averaging 2x2 rather than dropping
	// every other texel matters here: point sampling a diffuse texture down
	// two levels aliases badly on the sort of tiled surfaces this game is
	// mostly made of.
	for(int s = 0; s < shift; s++){
		int nw = w/2, nh = h/2;
		if(nw < 1 || nh < 1) break;
		u8 *dst = (u8*)malloc((size_t)nw*nh*4);
		for(int y = 0; y < nh; y++)
		for(int x = 0; x < nw; x++)
		for(int c = 0; c < 4; c++){
			const u8 *a = rgba + (((size_t)(2*y)  *w + 2*x  )*4);
			const u8 *b = rgba + (((size_t)(2*y)  *w + 2*x+1)*4);
			const u8 *e = rgba + (((size_t)(2*y+1)*w + 2*x  )*4);
			const u8 *f = rgba + (((size_t)(2*y+1)*w + 2*x+1)*4);
			dst[((size_t)y*nw + x)*4 + c] =
			    (u8)((a[c] + b[c] + e[c] + f[c] + 2)/4);
		}
		free(rgba); rgba = dst; w = nw; h = nh;
	}

	// --shrink: textures taller than gShrinkH go to gShrinkPct% on both axes,
	// rounded up to the 8-texel tile. Non-power-of-two sizes are fine for
	// CLAMP and for Dolphin; hardware GX_REPEAT/MIRROR expect powers of two.
	if(gShrinkH > 0 && h > gShrinkH){
		int nw = (w*gShrinkPct/100 + 7) & ~7, nh = (h*gShrinkPct/100 + 7) & ~7;
		if(nw > w) nw = w;
		if(nh > h) nh = h;
		if(nw < w || nh < h){
			rgba = resampleArea(rgba, w, h, nw, nh);
			w = nw; h = nh;
		}
	}

	// CMPR is 4bpp and fine for anything without a gradient alpha ramp;
	// RGB5A3 is 16bpp and keeps the ramp. Full resolution either way.
	// Small textures stay RGB5A3 outright: at 64px and below CMPR saves a
	// few KB total while its 4x4 blocks butcher soft effect gradients —
	// the additive rain drip drew its DXT block edges as a square halo.
	// Three native formats, smallest that fits each texture (dca3's rule):
	//   CMPR  4bpp  — no gradient alpha (opaque diffuse, the bulk of the map)
	//   IA4   8bpp  — gradient alpha AND grayscale (fonts, shadows, most FX)
	//   RGB5A3 16bpp — gradient alpha WITH colour (blood, arrows, foliage)
	// (No small-size floor: the reference card tree compresses even 16px
	// textures, and a 64px RGB5A3 floor inflated the archive from 174MB
	// to 207MB, enough to put the streamer into permanent eviction thrash.)
	u8 gxFmt = !gradientAlpha ? GXFMT_CMPR : grayscale ? GXFMT_IA4 : GXFMT_RGB5A3;
	int align = gxFmt == GXFMT_RGB5A3 ? 3 : 7;
	int tw = (w + align) & ~align;
	int th = (h + align) & ~align;
	if(tw < align+1) tw = align+1;
	if(th < align+1) th = align+1;
	out->size = gxFmt == GXFMT_CMPR ? (uint32)tw*th/2 :
	            gxFmt == GXFMT_IA4  ? (uint32)tw*th   : (uint32)tw*th*2;
	out->tiled = (u8*)malloc(out->size);
	if(gxFmt == GXFMT_CMPR){
		if(dxt1 && w == srcW && h == srcH && tw == w && th == h &&
		   transcodeDXT1(out->tiled, dxt1, rgba, w, h, format == Raster::C565))
			gLosslessCMPR++;
		else{
			tileCMPR(out->tiled, rgba, w, h, tw, th);
			gEncodedCMPR++;
		}
	}
	else if(gxFmt == GXFMT_IA4)  tileIA4(out->tiled, rgba, w, h, tw, th);
	else                         tileRGB5A3(out->tiled, rgba, w, h, tw, th);

	// Mip chain for power-of-two CMPR, down to the 8x8 tile. The PC builds
	// these at load (filter 6, LINEARMIPLINEAR, on 99% of its textures) and
	// draws with them; the console pages into MEM1 only the levels a draw
	// needs (gxraster.cpp gxPageIn), so a wall 100 m away costs its 16x16
	// level in the window rather than its 256x256 one. Level 0 stays as
	// converted above; the smaller levels are area-averaged like the PC's.
	out->levels = 1;
	if(gxFmt == GXFMT_CMPR && tw == w && th == h && w >= 16 && h >= 16 &&
	   !(w & (w-1)) && !(h & (h-1))){
		std::vector<u8> chain(out->tiled, out->tiled + out->size);
		u8 *lv = (u8*)malloc((size_t)w*h*4);
		memcpy(lv, rgba, (size_t)w*h*4);
		int lw = w, lh = h;
		while(lw >= 16 && lh >= 16){
			lv = resampleArea(lv, lw, lh, lw/2, lh/2);
			lw /= 2; lh /= 2;
			size_t at = chain.size();
			chain.resize(at + (size_t)lw*lh/2);
			tileCMPR(chain.data() + at, lv, lw, lh, lw, lh);
			out->levels++;
		}
		free(lv);
		free(out->tiled);
		out->size = (uint32)chain.size();
		out->tiled = (u8*)malloc(out->size);
		memcpy(out->tiled, chain.data(), out->size);
	}

	out->tw = tw; out->th = th;
	out->gxFmt = gxFmt;
	out->format = format;
	out->filterAddressing = filterAddressing;
	memset(out->name, 0, 32); strncpy(out->name, name, 31);
	memset(out->mask, 0, 32); strncpy(out->mask, mask, 31);

	free(rgba);
	img->destroy();
	return true;
}

// Manual walk of a D3D8 texture dictionary librw's reader refuses (old RW
// versions). Returns texture count or -1.
static int
readD3D8TxdManually(const char *path, Conv *convs, int maxn)
{
	FILE *f = fopen(path, "rb");
	if(f == nil) return -1;
	fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
	u8 *d = (u8*)malloc(len);
	if(fread(d, 1, len, f) != (size_t)len){ fclose(f); free(d); return -1; }
	fclose(f);

	auto rd32 = [&](long o){ return (uint32)d[o] | d[o+1]<<8 | d[o+2]<<16 | ((uint32)d[o+3]<<24); };
	auto rd16 = [&](long o){ return (uint32)d[o] | d[o+1]<<8; };
	if(len < 24 || rd32(0) != ID_TEXDICTIONARY){ free(d); return -1; }
	long off = 12;
	if(rd32(off) != ID_STRUCT){ free(d); return -1; }
	int count = rd16(off+12);
	off += 12 + rd32(off+4);

	int n = 0;
	for(int t = 0; t < count && n < maxn && off + 24 < len; t++){
		if(rd32(off) != ID_TEXTURENATIVE) break;
		long chunkEnd = off + 12 + rd32(off+4);
		long p = off + 12;
		if(rd32(p) != ID_STRUCT) break;
		long body = p + 12;
		uint32 plat = rd32(body);
		if(plat != 8){ off = chunkEnd; continue; }   // PLATFORM_D3D8
		uint32 filterAddr = rd32(body+4);
		char name[33]; memcpy(name, d+body+8, 32); name[32] = 0;
		char mask[33]; memcpy(mask, d+body+40, 32); mask[32] = 0;
		uint32 rasterFmt = rd32(body+72);
		/* uint32 hasAlpha = rd32(body+76); */
		int w = rd16(body+80), h = rd16(body+82);
		int depth = d[body+84];
		/* numLevels d[85], type d[86] */
		int compression = d[body+87];
		long q = body + 88;

		Image *img = nil;
		if(compression){
			uint32 lvlSize = rd32(q); q += 4;
			img = Image::create(w, h, 32);
			img->allocate();
			img->setPixelsDXT(compression, d+q);
			if((rasterFmt & 0xF00) == Raster::C565)
				img->removeMask();
		}else if(depth == 8 && (rasterFmt & Raster::PAL8)){
			const u8 *pal = d + q; q += 256*4;
			uint32 lvlSize = rd32(q); q += 4;
			img = Image::create(w, h, 32);
			img->allocate();
			for(long i = 0; i < (long)lvlSize && i < (long)w*h; i++){
				const u8 *c = pal + d[q+i]*4;
				u8 *o = img->pixels + i*4;
				o[0]=c[0]; o[1]=c[1]; o[2]=c[2]; o[3]=c[3];
			}
		}else if(depth == 32 || depth == 24 || depth == 16){
			uint32 lvlSize = rd32(q); q += 4;
			img = Image::create(w, h, 32);
			img->allocate();
			int bpp = depth/8;
			for(long i = 0; i < (long)w*h && (i+1)*bpp <= (long)lvlSize; i++){
				const u8 *c = d + q + i*bpp;
				u8 *o = img->pixels + i*4;
				if(depth == 32){ o[0]=c[2]; o[1]=c[1]; o[2]=c[0]; o[3]=c[3]; }
				else if(depth == 24){ o[0]=c[2]; o[1]=c[1]; o[2]=c[0]; o[3]=255; }
				else{ // 1555
					uint32 v = c[0] | c[1]<<8;
					o[0]=(v>>10&31)*255/31; o[1]=(v>>5&31)*255/31;
					o[2]=(v&31)*255/31; o[3]=(v&0x8000)?255:0;
				}
			}
		}
		if(img){
			if(convertImage(img, name, mask, filterAddr, rasterFmt & 0xF00, &convs[n],
			    compression == 1 ? d+q : nil))
				n++;
		}
		off = chunkEnd;
	}
	free(d);
	return n;
}

static bool
convertTexture(Texture *tex, Conv *out)
{
	Raster *ras = tex->raster;
	if(ras == nil) return false;
	// Level 0 of a DXT1 raster as stored, for the lossless CMPR transcode.
	const u8 *dxt1 = nil;
	if(ras->platform == PLATFORM_D3D8 || ras->platform == PLATFORM_D3D9){
		d3d::D3dRaster *dr = GETD3DRASTEREXT(ras);
		if(dr->customFormat && dr->format == d3d::D3DFMT_DXT1 && dr->texture)
			dxt1 = ((RasterLevels*)dr->texture)->levels[0].data;
	}
	return convertImage(ras->toImage(), tex->name, tex->mask,
	    tex->filterAddressing, ras->format & 0xF00, out, dxt1);
}

static void
writeNative(StreamFile *s, Conv *c)
{
	// TEXTURENATIVE holds the struct *and* a per-texture extension chunk.
	// TexDictionary::streamRead calls Texture::s_plglist.streamRead right
	// after the native read and then checks that the stream landed exactly on
	// the declared end of this chunk. Omit the extension and every converted
	// dictionary fails to load, the streamer re-requests it forever, and the
	// console drains its heap on the retry loop while streaming sits frozen.
	uint32 payload = GXNATIVE_HEADER + 4 + c->size;
	writeChunkHeader(s, ID_TEXTURENATIVE, 12 + payload + 12);
	writeChunkHeader(s, ID_STRUCT, payload);
	u8 header[GXNATIVE_HEADER];
	memset(header, 0, sizeof(header));
	writeLE32(&header[0], PLATFORM_GAMECUBE);
	writeLE32(&header[4], c->filterAddressing);
	memcpy(&header[8], c->name, 32);
	memcpy(&header[40], c->mask, 32);
	writeLE32(&header[72], c->format);
	writeLE32(&header[76], c->gxFmt != GXFMT_CMPR);
	writeLE16(&header[80], (uint16)c->tw);
	writeLE16(&header[82], (uint16)c->th);
	header[84] = 16;
	header[85] = (u8)c->levels;
	header[86] = Raster::TEXTURE;
	header[87] = c->gxFmt;
	s->write8(header, sizeof(header));
	s->writeU32(c->size);
	s->write8(c->tiled, c->size);
	writeChunkHeader(s, ID_EXTENSION, 0);
}

static void
decode5A3(uint16 v, u8 *p)
{
	if(v & 0x8000){
		for(int c = 0; c < 3; c++){
			int n = (v >> (10 - 5*c)) & 31;
			p[c] = (n << 3) | (n >> 2);
		}
		p[3] = 255;
	}else{
		for(int c = 0; c < 3; c++) p[c] = ((v >> (8 - 4*c)) & 15)*17;
		p[3] = ((v >> 12) & 7)*255/7;
	}
}

static u8*
decodeNative(const u8 *src, int w, int h, int fmt)
{
	u8 *rgba = (u8*)malloc((size_t)w*h*4);
	if(rgba == nil) throw std::runtime_error("out of host memory");
	if(fmt == GXFMT_CMPR){
		for(int ty = 0; ty < h; ty += 8)
		for(int tx = 0; tx < w; tx += 8)
		for(int sub = 0; sub < 4; sub++, src += 8){
			uint16 c0 = (src[0] << 8) | src[1], c1 = (src[2] << 8) | src[3];
			u8 pal[4][4];
			for(int k = 0; k < 2; k++){
				uint16 v = k ? c1 : c0;
				int r = v >> 11, g = (v >> 5) & 63, b = v & 31;
				pal[k][0] = (r << 3) | (r >> 2);
				pal[k][1] = (g << 2) | (g >> 4);
				pal[k][2] = (b << 3) | (b >> 2);
				pal[k][3] = 255;
			}
			for(int c = 0; c < 3; c++){
				pal[2][c] = c0 > c1 ? (2*pal[0][c] + pal[1][c])/3 : (pal[0][c] + pal[1][c])/2;
				pal[3][c] = c0 > c1 ? (pal[0][c] + 2*pal[1][c])/3 : pal[2][c];
			}
			pal[2][3] = 255;
			pal[3][3] = c0 > c1 ? 255 : 0;
			for(int y = 0; y < 4; y++)
			for(int x = 0; x < 4; x++){
				int px = tx + (sub & 1)*4 + x, py = ty + (sub >> 1)*4 + y;
				memcpy(rgba + ((size_t)py*w + px)*4, pal[(src[4+y] >> (6-2*x)) & 3], 4);
			}
		}
	}else{
		int tileW = fmt == GXFMT_RGB5A3 ? 4 : 8;
		const u8 *palette = src + w*h;
		for(int ty = 0; ty < h; ty += 4)
		for(int tx = 0; tx < w; tx += tileW)
		for(int y = 0; y < 4; y++)
		for(int x = 0; x < tileW; x++){
			u8 *p = rgba + ((size_t)(ty+y)*w + tx+x)*4;
			if(fmt == GXFMT_RGB5A3){
				decode5A3((src[0] << 8) | src[1], p); src += 2;
			}else if(fmt == GXFMT_IA4){
				p[0] = p[1] = p[2] = (*src & 15)*17;
				p[3] = (*src >> 4)*17; src++;
			}else{
				const u8 *v = palette + 2*(*src++);
				decode5A3((v[0] << 8) | v[1], p);
			}
		}
	}
	return rgba;
}

static void
tileCI8(u8 *dst, const u8 *rgba, int w, int h, const u8 *palette)
{
	int pal[256][4];
	for(int i = 0; i < 256; i++){
		u8 p[4]; decode5A3((palette[i*2] << 8) | palette[i*2+1], p);
		for(int c = 0; c < 3; c++) pal[i][c] = p[c]*p[3]/255;
		pal[i][3] = p[3];
	}
	for(int ty = 0; ty < h; ty += 4)
	for(int tx = 0; tx < w; tx += 8)
	for(int y = 0; y < 4; y++)
	for(int x = 0; x < 8; x++){
		const u8 *p = rgba + ((size_t)(ty+y)*w + tx+x)*4;
		int target[4] = {p[0]*p[3]/255, p[1]*p[3]/255, p[2]*p[3]/255, p[3]};
		int best = 0, bestError = 0x7fffffff;
		for(int i = 0; i < 256; i++){
			int error = 0;
			for(int c = 0; c < 4; c++){
				int d = pal[i][c] - target[c]; error += d*d;
			}
			if(error < bestError){ bestError = error; best = i; }
			if(error == 0) break;
		}
		*dst++ = best;
	}
	memcpy(dst, palette, 512);
}

static std::vector<u8>
halveNativeStruct(const u8 *body, size_t length)
{
	if(length < 92 || readLE32(body) != PLATFORM_GAMECUBE)
		throw std::runtime_error("expected a GX native texture");
	int w = readLE16(body+80), h = readLE16(body+82), fmt = body[87];
	if(fmt != GXFMT_CMPR && fmt != GXFMT_RGB5A3 && fmt != GXFMT_IA4 && fmt != 9)
		throw std::runtime_error("unsupported GX texture format");
	int tileW = fmt == GXFMT_RGB5A3 ? 4 : 8, tileH = fmt == GXFMT_CMPR ? 8 : 4;
	if(w <= 0 || h <= 0 || w > 1024 || h > 1024 || w % tileW || h % tileH || body[85] != 1)
		throw std::runtime_error("invalid GX texture dimensions or mip count");
	uint32 size = readLE32(body+88);
	auto nativeSize = [fmt](int width, int height){
		return fmt == GXFMT_CMPR ? width*height/2 : fmt == GXFMT_RGB5A3 ? width*height*2 :
		       width*height + (fmt == 9 ? 512 : 0);
	};
	if(size != (uint32)nativeSize(w, h)) throw std::runtime_error("invalid GX texture size");
	if(readLE32(body+4) & 0x80000000u){
		if(length != 100) throw std::runtime_error("invalid shared reference");
		return std::vector<u8>(body, body+length);
	}
	if(length != 92+size) throw std::runtime_error("truncated GX texture");
	int nw = std::max(tileW, w/2), nh = std::max(tileH, h/2);
	if(nw % tileW || nh % tileH) throw std::runtime_error("half size is not tile aligned");
	if(nw == w && nh == h) return std::vector<u8>(body, body+length);
	u8 *rgba = resampleArea(decodeNative(body+92, w, h, fmt), w, h, nw, nh);
	std::vector<u8> out(92 + nativeSize(nw, nh));
	memcpy(out.data(), body, 88);
	writeLE16(out.data()+80, nw); writeLE16(out.data()+82, nh);
	writeLE32(out.data()+88, out.size()-92);
	if(fmt == GXFMT_CMPR) tileCMPR(out.data()+92, rgba, nw, nh, nw, nh);
	else if(fmt == GXFMT_RGB5A3) tileRGB5A3(out.data()+92, rgba, nw, nh, nw, nh);
	else if(fmt == GXFMT_IA4) tileIA4(out.data()+92, rgba, nw, nh, nw, nh);
	else tileCI8(out.data()+92, rgba, nw, nh, body+92+w*h);
	free(rgba);
	return out;
}

static std::vector<u8>
halveNativeChunks(const u8 *data, size_t length, uint32 parent)
{
	std::vector<u8> out;
	for(size_t p = 0; p < length; ){
		if(std::all_of(data+p, data+length, [](u8 b){ return b == 0; })) break;
		if(length-p < 12) throw std::runtime_error("truncated chunk header");
		uint32 id = readLE32(data+p), size = readLE32(data+p+4);
		if(size > length-p-12) throw std::runtime_error("chunk overruns parent");
		const u8 *body = data+p+12;
		std::vector<u8> next;
		if(id == ID_TEXDICTIONARY || id == ID_TEXTURENATIVE)
			next = halveNativeChunks(body, size, id);
		else if(id == ID_STRUCT && parent == ID_TEXTURENATIVE)
			next = halveNativeStruct(body, size);
		else next.assign(body, body+size);
		size_t start = out.size();
		out.insert(out.end(), data+p, data+p+12);
		writeLE32(out.data()+start+4, next.size());
		out.insert(out.end(), next.begin(), next.end());
		p += 12+size;
	}
	return out;
}

static int
halveNativeFile(const char *input, const char *output)
{
	try{
		FILE *f = fopen(input, "rb");
		if(!f) throw std::runtime_error("cannot open input");
		fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
		if(len < 12){ fclose(f); throw std::runtime_error("truncated input"); }
		std::vector<u8> data(len);
		size_t n = fread(data.data(), 1, data.size(), f); fclose(f);
		if(n != data.size() || readLE32(data.data()) != ID_TEXDICTIONARY)
			throw std::runtime_error("invalid texture dictionary");
		std::vector<u8> out = halveNativeChunks(data.data(), data.size(), 0);
		f = fopen(output, "wb");
		if(!f) throw std::runtime_error("cannot open output");
		n = fwrite(out.data(), 1, out.size(), f); int closed = fclose(f);
		if(n != out.size() || closed) throw std::runtime_error("cannot write output");
		return 0;
	}catch(const std::exception &e){
		fprintf(stderr, "%s: %s\n", input, e.what());
		return 1;
	}
}

int
main(int argc, char **argv)
{
	if(argc == 4 && strcmp(argv[1], "--native-half") == 0)
		return halveNativeFile(argv[2], argv[3]);
	// --max-dim N caps the largest texture axis, halving in powers of two.
	// Texture data is 191.6MB of the 329.8MB archive, so this is the only
	// lever on disc size worth pulling.
	// --image texname=file.tga builds a dictionary from loose images instead
	// of converting one. Needed because a GameCube has no DualShock: the pad
	// diagram in the frontend has to be drawn from something that is not in
	// the PC game's files. Uncompressed TGA only — librw's readTGA asserts on
	// RLE (imageType 10).
	const char *imgName[16], *imgPath[16];
	int nimg = 0;

	int argi = 1;
	while(argi < argc && strncmp(argv[argi], "--", 2) == 0){
		if(strcmp(argv[argi], "--max-dim") == 0 && argi+1 < argc){
			gMaxDim = atoi(argv[argi+1]);
			if(gMaxDim < 8) gMaxDim = 8;
			argi += 2;
		}else if(strcmp(argv[argi], "--shrink") == 0 && argi+2 < argc){
			gShrinkH = atoi(argv[argi+1]);
			gShrinkPct = atoi(argv[argi+2]);
			if(gShrinkH < 1 || gShrinkPct < 1 || gShrinkPct > 100){
				fprintf(stderr, "bad --shrink %s %s\n", argv[argi+1], argv[argi+2]);
				return 1;
			}
			argi += 3;
		}else if(strcmp(argv[argi], "--image") == 0 && argi+1 < argc){
			char *eq = strchr(argv[argi+1], '=');
			if(eq == nil || nimg >= 16){
				fprintf(stderr, "bad --image %s\n", argv[argi+1]);
				return 1;
			}
			*eq = '\0';
			imgName[nimg] = argv[argi+1];
			imgPath[nimg] = eq+1;
			nimg++;
			argi += 2;
		}else{
			fprintf(stderr, "unknown option %s\n", argv[argi]);
			return 1;
		}
	}
	if(argc - argi < (nimg ? 1 : 2)){
		fprintf(stderr, "usage: %s [--max-dim N] [--shrink H PCT] in.txd out.txd\n"
		                "       %s --image name=img.tga [...] out.txd\n"
		                "       %s --native-half in-gx.txd out-gx.txd\n",
		    argv[0], argv[0], argv[0]);
		return 1;
	}

	Engine::init();
	Engine::open(nil);
	Engine::start();

	Conv convs[512];
	int n = 0;
	if(nimg){
		for(int i = 0; i < nimg; i++){
			// LINEAR filter, WRAP on both axes — the frontend overrides
			// addressing to BORDER on these sprites anyway.
			if(!convertImage(readTGA(imgPath[i]), imgName[i], "",
			    0x1102, Raster::C8888, &convs[n])){
				fprintf(stderr, "cannot read %s\n", imgPath[i]);
				return 1;
			}
			n++;
		}
	}else{
	StreamFile in;
	if(in.open(argv[argi], "rb") == nil){ fprintf(stderr, "cannot open %s\n", argv[argi]); return 1; }
	if(!findChunk(&in, ID_TEXDICTIONARY, nil, nil)){ fprintf(stderr, "not a TXD\n"); return 1; }
	TexDictionary *txd = TexDictionary::streamRead(&in);
	in.close();
	if(txd == nil){
		// Old-version D3D8 dictionaries (neo.txd is RW 3.5) fail librw's
		// reader wholesale. The d3d8 native layout is simple enough to walk
		// by hand: header, optional palette, mip levels — level 0 is all the
		// converter keeps anyway.
		n = readD3D8TxdManually(argv[argi], convs, 512);
		if(n < 0){ fprintf(stderr, "TXD read failed\n"); return 1; }
		fprintf(stderr, "%s: librw refused it; manual d3d8 walk got %d textures\n",
		    argv[argi], n);
	}else
	FORLIST(lnk, txd->textures){
		if(n >= 512) break;
		if(convertTexture(Texture::fromDict(lnk), &convs[n]))
			n++;
	}
	}
	// An empty dictionary is legal — Vice City ships several — and writing a
	// valid empty GX one matters, because copying the D3D8 original through
	// instead leaves a mixed-platform archive. On this build every
	// engine->driver slot is overridden with the GX functions, so a D3D8
	// raster that survives into the game would be handed to gx::rasterToImage
	// and its DXT payload read as linear pixels.
	if(n == 0)
		fprintf(stderr, "%s: empty dictionary, writing an empty GX one\n", argv[1]);

	uint32 total = 12 + 4;              // struct header + texture count
	for(int i = 0; i < n; i++)
		total += 12 + 12 + GXNATIVE_HEADER + 4 + convs[i].size + 12;
	total += 12;                        // empty extension

	const char *outPath = nimg ? argv[argi] : argv[argi+1];
	StreamFile out;
	if(out.open(outPath, "wb") == nil){ fprintf(stderr, "cannot write %s\n", outPath); return 1; }
	writeChunkHeader(&out, ID_TEXDICTIONARY, total);
	writeChunkHeader(&out, ID_STRUCT, 4);
	out.writeU16((uint16)n);
	// The dictionary struct is uint16 count + uint16 deviceId only to librw.
	// The game reads all four bytes as one int32 count (TexRead.cpp,
	// RwTexDictionaryGtaStreamRead) and rejects anything over INT16_MAX, so a
	// deviceId of PLATFORM_GAMECUBE made every converted dictionary read as
	// 393216+n textures and fail before a single texture was touched — which is
	// why dvd:/native.log stayed empty, ms_memoryUsed froze and the streamer
	// re-requested the same 78 models forever. Every stock Vice City TXD writes
	// 0 here; so do we. Per-texture dispatch uses the platform field in the
	// native header, not this.
	out.writeU16(0);
	for(int i = 0; i < n; i++)
		writeNative(&out, &convs[i]);
	writeChunkHeader(&out, ID_EXTENSION, 0);
	out.close();

	uint32 bytes = 0;
	for(int i = 0; i < n; i++) bytes += convs[i].size;
	printf("%s -> %s : %d textures, %u KB tiled, CMPR %d lossless %d re-encoded\n",
	    nimg ? "images" : argv[argi], outPath, n, bytes>>10, gLosslessCMPR, gEncodedCMPR);
	return 0;
}
