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
// Textures keep their full resolution. The GameCube has the memory for it once
// the conversion churn is gone; downsampling is a Dreamcast concession.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <rw.h>

using namespace rw;
typedef uint8 u8;

enum { GXFMT_RGB5A3 = 0x5, GXFMT_CMPR = 0xE };

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

static void
tileCMPR(u8 *dst, const u8 *src, int w, int h, int tw, int th)
{
	for(int ty = 0; ty < th; ty += 8)
	for(int tx = 0; tx < tw; tx += 8)
	for(int sub = 0; sub < 4; sub++){
		int bx = tx + (sub & 1)*4, by = ty + (sub >> 1)*4;
		u8 px[16][4];
		bool trans = false;
		u8 mn[3] = {255,255,255}, mx[3] = {0,0,0};
		for(int i = 0; i < 16; i++){
			sampleSrc(src, w, h, bx + (i&3), by + (i>>2), tw, th,
			    &px[i][0], &px[i][1], &px[i][2], &px[i][3]);
			if(px[i][3] < 128){ trans = true; continue; }
			for(int c = 0; c < 3; c++){
				if(px[i][c] < mn[c]) mn[c] = px[i][c];
				if(px[i][c] > mx[c]) mx[c] = px[i][c];
			}
		}
		uint16 lo = to565(mn[0],mn[1],mn[2]);
		uint16 hi = to565(mx[0],mx[1],mx[2]);
		int dir[3] = { mx[0]-mn[0], mx[1]-mn[1], mx[2]-mn[2] };
		int len2 = dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2];
		u8 *idx = dst + 4;
		if(trans){
			put16be(dst, lo);
			put16be(dst+2, hi >= lo ? hi : lo);
			for(int row = 0; row < 4; row++){
				u8 byte = 0;
				for(int x = 0; x < 4; x++){
					u8 *p = px[row*4 + x];
					u8 v;
					if(p[3] < 128) v = 3;
					else if(len2 == 0) v = 0;
					else{
						int t = ((p[0]-mn[0])*dir[0] + (p[1]-mn[1])*dir[1] +
						    (p[2]-mn[2])*dir[2]) * 4 / len2;
						v = t <= 0 ? 0 : t >= 3 ? 1 : 2;
					}
					byte |= v << (6 - 2*x);
				}
				idx[row] = byte;
			}
		}else if(hi == lo){
			put16be(dst, (uint16)(hi | 1));
			put16be(dst+2, (uint16)(lo & ~1));
			idx[0]=idx[1]=idx[2]=idx[3] = 0x55;
		}else{
			put16be(dst, hi > lo ? hi : lo);
			put16be(dst+2, hi > lo ? lo : hi);
			bool flip = hi < lo;
			for(int row = 0; row < 4; row++){
				u8 byte = 0;
				for(int x = 0; x < 4; x++){
					u8 *p = px[row*4 + x];
					int t = len2 ? ((p[0]-mn[0])*dir[0] + (p[1]-mn[1])*dir[1] +
					    (p[2]-mn[2])*dir[2]) * 6 / len2 : 0;
					u8 v = t >= 5 ? 0 : t <= 1 ? 1 : t >= 3 ? 2 : 3;
					if(flip) v = v == 0 ? 1 : v == 1 ? 0 : v == 2 ? 3 : 2;
					byte |= v << (6 - 2*x);
				}
				idx[row] = byte;
			}
		}
		dst += 8;
	}
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
};

static bool
convertTexture(Texture *tex, Conv *out)
{
	Raster *ras = tex->raster;
	if(ras == nil) return false;
	Image *img = ras->toImage();
	if(img == nil) return false;
	img->unpalettize(true);
	int w = img->width, h = img->height;
	if(w <= 0 || h <= 0){ img->destroy(); return false; }

	u8 *rgba = (u8*)malloc((size_t)w*h*4);
	bool gradientAlpha = false;
	for(int y = 0; y < h; y++)
	for(int x = 0; x < w; x++){
		u8 *s = img->pixels + (size_t)y*img->stride + (size_t)x*img->bpp;
		u8 *d = rgba + ((size_t)y*w + x)*4;
		d[0]=s[0]; d[1]=s[1]; d[2]=s[2];
		d[3] = img->bpp >= 4 ? s[3] : 255;
		if(d[3] > 16 && d[3] < 240) gradientAlpha = true;
	}

	// CMPR is 4bpp and fine for anything without a gradient alpha ramp;
	// RGB5A3 is 16bpp and keeps the ramp. Full resolution either way.
	bool cmpr = !gradientAlpha;
	int align = cmpr ? 7 : 3;
	int tw = (w + align) & ~align;
	int th = (h + align) & ~align;
	if(tw < align+1) tw = align+1;
	if(th < align+1) th = align+1;
	out->size = cmpr ? (uint32)tw*th/2 : (uint32)tw*th*2;
	out->tiled = (u8*)malloc(out->size);
	if(cmpr) tileCMPR(out->tiled, rgba, w, h, tw, th);
	else     tileRGB5A3(out->tiled, rgba, w, h, tw, th);

	out->tw = tw; out->th = th;
	out->gxFmt = cmpr ? GXFMT_CMPR : GXFMT_RGB5A3;
	out->format = ras->format & 0xF00;
	out->filterAddressing = tex->filterAddressing;
	memset(out->name, 0, 32); strncpy(out->name, tex->name, 31);
	memset(out->mask, 0, 32); strncpy(out->mask, tex->mask, 31);

	free(rgba);
	img->destroy();
	return true;
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
	header[85] = 1;
	header[86] = Raster::TEXTURE;
	header[87] = c->gxFmt;
	s->write8(header, sizeof(header));
	s->writeU32(c->size);
	s->write8(c->tiled, c->size);
	writeChunkHeader(s, ID_EXTENSION, 0);
}

int
main(int argc, char **argv)
{
	if(argc < 3){ fprintf(stderr, "usage: %s in.txd out.txd\n", argv[0]); return 1; }

	Engine::init();
	Engine::open(nil);
	Engine::start();

	StreamFile in;
	if(in.open(argv[1], "rb") == nil){ fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
	if(!findChunk(&in, ID_TEXDICTIONARY, nil, nil)){ fprintf(stderr, "not a TXD\n"); return 1; }
	TexDictionary *txd = TexDictionary::streamRead(&in);
	in.close();
	if(txd == nil){ fprintf(stderr, "TXD read failed\n"); return 1; }

	Conv convs[512];
	int n = 0;
	FORLIST(lnk, txd->textures){
		if(n >= 512) break;
		if(convertTexture(Texture::fromDict(lnk), &convs[n]))
			n++;
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

	StreamFile out;
	if(out.open(argv[2], "wb") == nil){ fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
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
	printf("%s -> %s : %d textures, %u KB tiled\n", argv[1], argv[2], n, bytes>>10);
	return 0;
}
