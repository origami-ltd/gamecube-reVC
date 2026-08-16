// Host-side census of what RpGeometry costs on the console, over the whole
// archive, against what a dca3-style packed native format would cost.
//
// The GX backend renders straight out of the RW arrays every frame
// (gx.cpp atomicRenderCB reads morphTargets[0].vertices, texCoords[0], colors,
// meshHeader indices), so every loaded model holds the full float32 RW layout
// resident. dca3 holds one packed blob per geometry instead and never allocates
// the RW arrays at all (rwdc.cpp readNativeData / Geometry::NATIVE).
//
// Usage: dffcensus <img> <dir>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <rw.h>

using namespace rw;

enum { SECTOR = 2048 };

struct Totals {
	long long geoms, verts, tris;
	long long rwBytes, packedBytes, triBytes;
	long long vertsNormals, vertsPrelit, vertsUV2;
};

// What librw's Geometry::allocateData + meshHeader actually take from the heap.
static long long
rwCost(Geometry *g)
{
	long long n = g->numVertices, t = g->numTriangles;
	long long b = 0;
	b += (long long)g->numMorphTargets * sizeof(MorphTarget);
	b += n * sizeof(V3d);                                  // positions
	if(g->flags & Geometry::NORMALS) b += n * sizeof(V3d);  // normals
	if(g->flags & Geometry::PRELIT)  b += n * sizeof(RGBA); // prelit colours
	b += (long long)g->numTexCoordSets * n * sizeof(TexCoords);
	b += t * sizeof(Triangle);
	if(g->meshHeader){
		b += sizeof(MeshHeader) + (long long)g->meshHeader->numMeshes*sizeof(Mesh);
		b += (long long)g->meshHeader->totalIndices * sizeof(uint16);
	}
	return b;
}

// dca3 keeps float32 positions because the PVR wants them. GX does not: every
// attribute here is a format GX_SetVtxAttrFmt takes natively, with a per-
// geometry shift, so nothing is unpacked at draw time.
//   pos GX_POS_XYZ/GX_S16  6   nrm GX_NRM_XYZ/GX_S8   3
//   uv  GX_TEX_ST/GX_S16   4   clr GX_CLR_RGBA/RGBA8  4
static long long
packedCost(Geometry *g)
{
	long long n = g->numVertices, t = g->numTriangles;
	long long vsz = 6;                                     // s16 position
	if(g->flags & Geometry::NORMALS) vsz += 3;             // s8 normal
	if(g->numTexCoordSets > 0)       vsz += 4;             // s16 u,v
	if(g->flags & Geometry::PRELIT)  vsz += 4;             // rgba8
	long long b = n * vsz + t * 3 * sizeof(uint16);
	if(g->meshHeader)
		b += sizeof(MeshHeader) + (long long)g->meshHeader->numMeshes*sizeof(Mesh);
	return b;
}

// The part of the RW cost that is pure redundancy: after the mesh header
// exists, nothing in the GX path reads triangles[] again — it renders from the
// mesh index lists. Same data, stored twice, 8 bytes a triangle.
static long long
triangleCost(Geometry *g)
{
	return (long long)g->numTriangles * sizeof(Triangle);
}

static void
account(Clump *c, Totals *tot)
{
	FORLIST(alnk, c->atomics){
		Geometry *g = Atomic::fromClump(alnk)->geometry;
		if(g == nil || g->flags & Geometry::NATIVE)
			continue;
		tot->geoms++;
		tot->verts += g->numVertices;
		tot->tris += g->numTriangles;
		tot->rwBytes += rwCost(g);
		tot->packedBytes += packedCost(g);
		tot->triBytes += triangleCost(g);
		if(g->flags & Geometry::NORMALS) tot->vertsNormals += g->numVertices;
		if(g->flags & Geometry::PRELIT)  tot->vertsPrelit += g->numVertices;
		if(g->numTexCoordSets > 1)       tot->vertsUV2 += g->numVertices;
	}
}

int
main(int argc, char **argv)
{
	if(argc < 3){ fprintf(stderr, "usage: %s img dir\n", argv[0]); return 1; }

	Engine::init();
	registerHAnimPlugin();
	registerMatFXPlugin();
	registerSkinPlugin();
	registerNativeDataPlugin();
	Engine::open(nil);
	Engine::start();

	FILE *dir = fopen(argv[2], "rb");
	FILE *img = fopen(argv[1], "rb");
	if(dir == nil || img == nil){ fprintf(stderr, "cannot open\n"); return 1; }

	Totals tot;
	memset(&tot, 0, sizeof(tot));
	int files = 0, failed = 0;
	uint8 ent[32];
	uint8 *buf = nil;
	uint32 bufsz = 0;

	while(fread(ent, 1, 32, dir) == 32){
		uint32 off = ent[0] | ent[1]<<8 | ent[2]<<16 | (uint32)ent[3]<<24;
		uint32 siz = ent[4] | ent[5]<<8 | ent[6]<<16 | (uint32)ent[7]<<24;
		char name[25];
		memcpy(name, &ent[8], 24);
		name[24] = '\0';
		const char *dot = strrchr(name, '.');
		if(dot == nil || strcasecmp(dot, ".dff") != 0)
			continue;
		uint32 bytes = siz * SECTOR;
		if(bytes > bufsz){ free(buf); buf = (uint8*)malloc(bytes); bufsz = bytes; }
		fseek(img, (long)off * SECTOR, SEEK_SET);
		if(fread(buf, 1, bytes, img) != bytes){ failed++; continue; }

		StreamMemory in;
		in.open(buf, bytes);
		if(!findChunk(&in, ID_CLUMP, nil, nil)){ failed++; in.close(); continue; }
		Clump *c = Clump::streamRead(&in);
		in.close();
		if(c == nil){ failed++; continue; }
		account(c, &tot);
		c->destroy();
		files++;
	}

	printf("dff files read : %d  (%d unreadable)\n", files, failed);
	printf("geometries     : %lld\n", tot.geoms);
	printf("vertices       : %lld   triangles: %lld\n", tot.verts, tot.tris);
	printf("  with normals : %lld (%.0f%%)\n", tot.vertsNormals,
	    tot.verts ? 100.0*tot.vertsNormals/tot.verts : 0);
	printf("  with prelit  : %lld (%.0f%%)\n", tot.vertsPrelit,
	    tot.verts ? 100.0*tot.vertsPrelit/tot.verts : 0);
	printf("  with 2nd UV  : %lld\n", tot.vertsUV2);
	printf("\nwhole archive resident if every model were loaded at once:\n");
	printf("  RW arrays as loaded today : %.1f MB\n", tot.rwBytes/1048576.0);
	printf("  dca3-style packed native  : %.1f MB  (%.0f%% of today)\n",
	    tot.packedBytes/1048576.0,
	    tot.rwBytes ? 100.0*tot.packedBytes/tot.rwBytes : 0);
	printf("  of which triangles[]      : %.1f MB  (dead once meshes exist)\n",
	    tot.triBytes/1048576.0);
	printf("  bytes per vertex today    : %.1f -> packed %.1f\n",
	    tot.verts ? (double)tot.rwBytes/tot.verts : 0,
	    tot.verts ? (double)tot.packedBytes/tot.verts : 0);
	return 0;
}
