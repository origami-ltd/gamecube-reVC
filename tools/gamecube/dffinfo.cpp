// Host-side DFF inspector: what the GX default pipeline would actually see.
// The console probe said 165 of 235 ped meshes report numTexCoordSets == 0,
// i.e. no UV array — yet the player is one 256x256 atlas that cannot be drawn
// without UVs. This prints the truth per geometry/mesh so that claim can be
// checked in a second instead of a boot.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <rw.h>

using namespace rw;

int
main(int argc, char **argv)
{
	if(argc < 2){ fprintf(stderr, "usage: %s file.dff\n", argv[0]); return 1; }

	Engine::init();
	registerHAnimPlugin();
	registerMatFXPlugin();
	registerSkinPlugin();
	registerNativeDataPlugin();
	Engine::open(nil);
	Engine::start();

	StreamFile in;
	if(in.open(argv[1], "rb") == nil){ fprintf(stderr, "cannot open\n"); return 1; }
	if(!findChunk(&in, ID_CLUMP, nil, nil)){ fprintf(stderr, "not a clump\n"); return 1; }
	Clump *c = Clump::streamRead(&in);
	in.close();
	if(c == nil){ fprintf(stderr, "clump read failed\n"); return 1; }

	printf("clump: %d atomics\n", c->countAtomics());
	int ai = 0;
	FORLIST(alnk, c->atomics){
		Atomic *a = Atomic::fromClump(alnk);
		Geometry *g = a->geometry;
		if(g == nil){ printf("atomic %d: <no geometry>\n", ai++); continue; }
		Skin *skin = Skin::get(g);
		printf("\natomic %d  pipeline=%p  geo=%p\n", ai, (void*)a->pipeline, (void*)g);
		printf("  flags=%08x  NATIVE=%d TEXTURED=%d TEXTURED2=%d PRELIT=%d NORMALS=%d LIGHT=%d MODULATE=%d\n",
		    g->flags,
		    !!(g->flags & Geometry::NATIVE),
		    !!(g->flags & Geometry::TEXTURED),
		    !!(g->flags & Geometry::TEXTURED2),
		    !!(g->flags & Geometry::PRELIT),
		    !!(g->flags & Geometry::NORMALS),
		    !!(g->flags & Geometry::LIGHT),
		    !!(g->flags & Geometry::MODULATE));
		printf("  numVertices=%d numTriangles=%d numTexCoordSets=%d texCoords[0]=%p colors=%p\n",
		    g->numVertices, g->numTriangles, g->numTexCoordSets,
		    (void*)g->texCoords[0], (void*)g->colors);
		printf("  morphTargets=%p numMorphTargets=%d verts=%p normals=%p  skin=%p\n",
		    (void*)g->morphTargets, g->numMorphTargets,
		    g->morphTargets ? (void*)g->morphTargets[0].vertices : nil,
		    g->morphTargets ? (void*)g->morphTargets[0].normals : nil,
		    (void*)skin);
		if(skin)
			printf("  skin: numBones=%d numUsedBones=%d weights=%p indices=%p\n",
			    skin->numBones, skin->numUsedBones,
			    (void*)skin->weights, (void*)skin->indices);
		// UV sanity BEFORE the meshHeader check: a freshly streamed geometry
		// has no meshHeader until buildMeshes runs, and the UVs are the
		// question here.
		if(g->texCoords[0] && g->numVertices){
			float mnu=1e9f, mxu=-1e9f, mnv=1e9f, mxv=-1e9f;
			int outside = 0;
			for(int32 i = 0; i < g->numVertices; i++){
				float u = g->texCoords[0][i].u, v = g->texCoords[0][i].v;
				if(u<mnu)mnu=u; if(u>mxu)mxu=u;
				if(v<mnv)mnv=v; if(v>mxv)mxv=v;
				if(u < -0.01f || u > 1.01f || v < -0.01f || v > 1.01f) outside++;
			}
			printf("  UV: u[%.3f..%.3f] v[%.3f..%.3f]  outside0..1=%d/%d\n",
			    mnu, mxu, mnv, mxv, outside, g->numVertices);
			printf("  first 6 UVs:");
			for(int32 i = 0; i < 6 && i < g->numVertices; i++)
				printf(" (%.3f,%.3f)", g->texCoords[0][i].u, g->texCoords[0][i].v);
			printf("\n");
		}else
			printf("  UV: <NONE>\n");

		if(g->meshHeader == nil)
			g->buildMeshes();   // the game builds these when instancing
		MeshHeader *mh = g->meshHeader;
		if(mh == nil){ printf("  <no meshHeader yet>\n"); ai++; continue; }
		printf("  meshHeader: numMeshes=%d totalIndices=%d flags=%x (TRISTRIP=%d)\n",
		    mh->numMeshes, mh->totalIndices, mh->flags,
		    !!(mh->flags & MeshHeader::TRISTRIP));
		Mesh *m = mh->getMeshes();
		for(uint16 i = 0; i < mh->numMeshes; i++, m++){
			Material *mat = m->material;
			Texture *t = mat ? mat->texture : nil;
			printf("    mesh %2d: indices=%5d mat=%p tex=%-16s matcol=%02x%02x%02x%02x\n",
			    i, m->numIndices, (void*)mat, t ? t->name : "-",
			    mat ? mat->color.red : 0, mat ? mat->color.green : 0,
			    mat ? mat->color.blue : 0, mat ? mat->color.alpha : 0);
		}
		// UV sanity: range of texCoords[0]
		if(g->texCoords[0] && g->numVertices){
			float mnu=1e9f, mxu=-1e9f, mnv=1e9f, mxv=-1e9f;
			for(int32 i = 0; i < g->numVertices; i++){
				float u = g->texCoords[0][i].u, v = g->texCoords[0][i].v;
				if(u<mnu)mnu=u; if(u>mxu)mxu=u;
				if(v<mnv)mnv=v; if(v>mxv)mxv=v;
			}
			printf("  uv range: u[%.3f..%.3f] v[%.3f..%.3f]\n", mnu, mxu, mnv, mxv);
		}else
			printf("  uv range: <NO TEXCOORDS>\n");
		ai++;
	}
	return 0;
}
