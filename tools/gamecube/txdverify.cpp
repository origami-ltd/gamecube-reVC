// Read back a GX-native TXD the way the console reader does and decode it to
// an image, so the converted blob can be compared against the source before it
// is ever put on a disc. The endianness is the thing to catch here: the
// console stores the 16-bit CMPR/RGB5A3 fields big-endian because it is a
// big-endian machine, so a little-endian host converter has to swap on write —
// and the only honest check of that is to decode assuming big-endian and look.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <rw.h>

using namespace rw;
typedef uint8 u8;
enum { GXFMT_RGB5A3 = 0x5, GXFMT_CMPR = 0xE, GXNATIVE_HEADER = 88 };

static inline uint16 get16be(const u8 *p){ return (uint16)((p[0]<<8) | p[1]); }

static void writePPM(const char *path, const u8 *rgb, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if(!f) return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	fwrite(rgb, 1, (size_t)w*h*3, f);
	fclose(f);
}

static void untileCMPR(const u8 *in, u8 *rgb, int tw, int th)
{
	for(int ty = 0; ty < th; ty += 8)
	for(int tx = 0; tx < tw; tx += 8)
	for(int sub = 0; sub < 4; sub++){
		int bx = tx + (sub & 1)*4, by = ty + (sub >> 1)*4;
		uint16 c0 = get16be(in), c1 = get16be(in+2);
		u8 pal[4][3];
		pal[0][0]=((c0>>11)&0x1F)<<3; pal[0][1]=((c0>>5)&0x3F)<<2; pal[0][2]=(c0&0x1F)<<3;
		pal[1][0]=((c1>>11)&0x1F)<<3; pal[1][1]=((c1>>5)&0x3F)<<2; pal[1][2]=(c1&0x1F)<<3;
		for(int c = 0; c < 3; c++){
			if(c0 > c1){
				pal[2][c] = (u8)((2*pal[0][c] + pal[1][c])/3);
				pal[3][c] = (u8)((pal[0][c] + 2*pal[1][c])/3);
			}else{
				pal[2][c] = (u8)((pal[0][c] + pal[1][c])/2);
				pal[3][c] = 0;
			}
		}
		for(int row = 0; row < 4; row++){
			u8 byte = in[4+row];
			for(int x = 0; x < 4; x++){
				int v = (byte >> (6 - 2*x)) & 3;
				u8 *o = rgb + ((size_t)(by+row)*tw + (bx+x))*3;
				o[0]=pal[v][0]; o[1]=pal[v][1]; o[2]=pal[v][2];
			}
		}
		in += 8;
	}
}

static void untileRGB5A3(const u8 *in, u8 *rgb, int tw, int th)
{
	for(int ty = 0; ty < th; ty += 4)
	for(int tx = 0; tx < tw; tx += 4)
	for(int y = 0; y < 4; y++)
	for(int x = 0; x < 4; x++){
		uint16 v = get16be(in); in += 2;
		u8 r,g,b;
		if(v & 0x8000){ r=((v>>10)&0x1F)<<3; g=((v>>5)&0x1F)<<3; b=(v&0x1F)<<3; }
		else          { r=((v>>8)&0xF)<<4;   g=((v>>4)&0xF)<<4;  b=(v&0xF)<<4; }
		u8 *o = rgb + ((size_t)(ty+y)*tw + (tx+x))*3;
		o[0]=r; o[1]=g; o[2]=b;
	}
}

int main(int argc, char **argv)
{
	if(argc < 3){ fprintf(stderr, "usage: %s in_gx.txd outdir\n", argv[0]); return 1; }
	FILE *f = fopen(argv[1], "rb");
	if(!f){ fprintf(stderr, "cannot open\n"); return 1; }
	fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
	u8 *buf = (u8*)malloc(len);
	fread(buf, 1, len, f);
	fclose(f);

	// walk chunks: TEXDICTIONARY > STRUCT(count) > TEXTURENATIVE*
	u8 *p = buf + 12;            // into the dictionary
	uint32 structSize = *(uint32*)(p+4);
	// Read the count exactly as the game does, not as librw does. The game
	// (TexRead.cpp, RwTexDictionaryGtaStreamRead) takes all four struct bytes as
	// one int32 and refuses anything over INT16_MAX; librw takes the upper half
	// as a deviceId and ignores it. Reading it the librw way here is what let a
	// dictionary that the console rejected outright pass verification and reach
	// a disc — so verify the way the consumer parses, not the way the writer
	// meant it.
	uint32 count = *(uint32*)(p+12);
	if(structSize != 4 || count > 0x7FFF){
		fprintf(stderr, "dictionary count %u (struct %u bytes) — the game "
		    "rejects this before reading any texture\n", count, structSize);
		return 1;
	}
	printf("textures in dictionary: %u\n", count);
	p += 12 + structSize;

	int n = 0;
	while(p < buf + len && n < count){
		uint32 type = *(uint32*)p;
		uint32 size = *(uint32*)(p+4);
		if(type != ID_TEXTURENATIVE){ p += 12 + size; continue; }
		u8 *q = p + 12 + 12;                 // past native + struct headers
		uint32 plat = *(uint32*)q;
		const char *name = (const char*)(q+8);
		int tw = *(uint16*)(q+80), th = *(uint16*)(q+82);
		u8 fmt = q[87];
		uint32 dataSize = *(uint32*)(q+GXNATIVE_HEADER);
		u8 *data = q + GXNATIVE_HEADER + 4;
		uint32 expect = fmt == GXFMT_CMPR ? (uint32)tw*th/2 : (uint32)tw*th*2;
		printf("  %-20s plat=%u %dx%d fmt=%02x size=%u (expect %u) %s\n",
		    name, plat, tw, th, fmt, dataSize, expect,
		    dataSize == expect ? "OK" : "MISMATCH");
		if(dataSize == expect){
			u8 *rgb = (u8*)calloc((size_t)tw*th*3, 1);
			if(fmt == GXFMT_CMPR) untileCMPR(data, rgb, tw, th);
			else                  untileRGB5A3(data, rgb, tw, th);
			char path[512];
			snprintf(path, sizeof(path), "%s/gxnative_%02d_%s.ppm", argv[2], n, name);
			writePPM(path, rgb, tw, th);
			free(rgb);
		}
		p += 12 + size;
		n++;
	}
	return 0;
}
