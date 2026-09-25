/*
 * dvdfs.c — ISO9660 reader for the reVC GameCube mini-DVD, written from
 * scratch against the raw libogc DVD API (the devkitPro readsector example
 * is the reference read path). Replaces iso9660_dbg.c.
 *
 * Design, and why it looks like this:
 *
 * 1. The WHOLE directory tree is indexed once at mount into a flat array of
 *    {path-hash, lba, size}. After mount the disc is only ever touched for
 *    file DATA — there is no runtime directory parsing, no path-table cache,
 *    no per-open allocation, and therefore no directory state for anything
 *    to corrupt or leak (both happened to the old driver).
 * 2. Every read is sector-granular through the DISC_INTERFACE. Small or
 *    unaligned reads bounce through ONE 32KB window that is always filled on
 *    a 16-sector-aligned boundary — unaligned window fills produced
 *    deterministic garbage under Dolphin's DI, and the double-issue below is
 *    the measured workaround for its one-command-late delivery. Large
 *    sector-aligned reads into 32-byte-aligned buffers (the streaming
 *    buffers are 2048-aligned) DMA straight into the destination.
 * 3. One mutex serializes every operation on the mount: window, command
 *    block, directory iterators. The old driver left readers of the shared
 *    window racing each other; the audio-vs-streaming overlap during the
 *    intro cutscene is exactly where the ISO build kept dying.
 * 4. Write opens fail immediately with EROFS before touching disc or lock —
 *    crash/watchdog log appends reach open_r from arbitrary thread context.
 */

#define _GNU_SOURCE 1
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <malloc.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/iosupport.h>
#include <ogc/dvd.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/disc_io.h>

#define FS_SECTOR       2048
#define FS_WINDOW_SECS  16              /* 32KB, the proven Dolphin window */
#define FS_MAX_ENTRIES  8192            /* disc has ~6100 files+dirs */
#define FS_MAX_NAME     208
#define FS_DIRFLAG      0x80000000u

typedef struct {
	u64 hash;                       /* FNV-1a64 of lowercased full path */
	u32 lba;
	u32 size;                       /* bit31 = directory */
} FsEnt;

typedef struct {
	const DISC_INTERFACE *disc;
	mutex_t lock;
	u8 *window;                     /* 32KB, 32-byte aligned */
	u32 windowBase;                 /* first sector in window, ~0 = empty */
	FsEnt *ent;
	u32 entN;
	u64 indexSum;                   /* tripwire over ent[] */
	u32 totalSectors;               /* from PVD volume space */
	char name[16];
	devoptab_t dotab;
} FsMount;

typedef struct {
	u32 lba;
	u32 size;                       /* with FS_DIRFLAG stripped */
	u32 pos;
	u32 isdir;
	int inUse;
} FsFile;

typedef struct {
	u32 lba;
	u32 size;
	u32 pos;                        /* byte offset into the directory extent */
	int inUse;
} FsDir;

static FsMount gMount;                  /* one disc drive, one mount */

/* watchdog fingerprint: total read operations served (io= in HANG lines) */
u32 gIsoRdN;
u32 gIsoRdJumps;   /* reads that did not continue the previous one: a seek on the real drive */
static u32 gIsoRdNext;
volatile u32 gIsoRdBusy;   /* MemoryWatcher: sector+1 while a DVD command is in flight */
/* last mount failure, printed by the boot park screen */
char isoMountErr[96];

/* ------------------------------------------------------------------ raw IO */

/* Dolphin's DI delivers each transfer one command late; the double issue is
 * LOAD-BEARING even with aligned fills — the single-issue experiment
 * (08-31) shipped garbage into loaded structures within 500 requests and a
 * wild pointer walked the EFB window. The 2x bandwidth tax is paid on the
 * emulator only in wall-clock terms; set FastDiscSpeed=True in Dolphin to
 * absorb it. Real hardware never showed the quirk and keeps the insurance. */
/* Timed read (09-01, B46). __io_gcdvd's readSectors is DVD_ReadPrio: it
 * sleeps on the DI completion with no bound, and with two threads issuing
 * disc commands (the audio decode thread's Vorbis reads, the streamer) a
 * dropped completion parked every reader behind this file's lock forever —
 * B42/B45 froze with the audio thread stopping first and the main loop two
 * seconds later, the emulated CPU idle. Same double issue as before, each
 * one asynchronous, polled against a deadline and re-issued into the SAME
 * buffer on silence (a late completion then writes identical bytes). */
u32 gIsoRdTimeouts;
static u32 gDiscCommands, gDiscBytes, gDiscReadMs, gDiscMaxMs;
static u64 gDiscFirstTime;

/* B177: who pays the disc. 0 = gta3.img (streamer), 1 = absolute reads (audio
 * rings, ped lines), 2 = everything else. Per class: commands, bytes, jumps,
 * ms. dvd:/dvdtrace.txt on the disc adds one DVDT line per command. */
enum { SRC_IMG, SRC_AUD, SRC_OTH, SRC_N };
static u32 gSrcCmd[SRC_N], gSrcBytes[SRC_N], gSrcJumps[SRC_N], gSrcMs[SRC_N];
static int gSrcAbs;                     /* set by fsReadSectorsAbs under m->lock */
static u32 gImgLba, gImgEnd;            /* gta3.img extent, looked up once */
/* DVDT records are written by whichever thread reads (under m->lock) and
 * printed only by fsDiscStatsPrint on the main thread: printing them straight
 * from the CdStream worker raced the main thread on newlib's unbuffered
 * stdout (b177b/b177d: dvdfs index scribbled, a boot freeze). */
enum { TRACE_N = 4096 };
static u32 (*gTrace)[4];                /* ms, src, lba, count<<16 | elapsed ms */
static u32 gTraceHead, gTraceTail;

void fsDiscStatsPrint(void)
{
	static u32 lastCommands, lastBytes, lastReadMs, lastJumps;
	static u32 lastCmd[SRC_N], lastBytesS[SRC_N], lastJ[SRC_N], lastMs[SRC_N];
	static u64 lastTime;
	u64 now = gettime();
	if(!lastTime) lastTime = gDiscFirstTime;
	printf("DVDIO interval %ums commands %u bytes %u nonseq %u read %ums peak %ums"
	       " | img %u/%uK/%u/%ums aud %u/%uK/%u/%ums oth %u/%uK/%u/%ums\n",
	       (unsigned)diff_msec(lastTime, now), gDiscCommands - lastCommands,
	       gDiscBytes - lastBytes, gIsoRdJumps - lastJumps,
	       gDiscReadMs - lastReadMs, gDiscMaxMs,
	       gSrcCmd[0]-lastCmd[0], (gSrcBytes[0]-lastBytesS[0])/1024, gSrcJumps[0]-lastJ[0], gSrcMs[0]-lastMs[0],
	       gSrcCmd[1]-lastCmd[1], (gSrcBytes[1]-lastBytesS[1])/1024, gSrcJumps[1]-lastJ[1], gSrcMs[1]-lastMs[1],
	       gSrcCmd[2]-lastCmd[2], (gSrcBytes[2]-lastBytesS[2])/1024, gSrcJumps[2]-lastJ[2], gSrcMs[2]-lastMs[2]);
	lastTime = now; lastCommands = gDiscCommands; lastBytes = gDiscBytes;
	lastReadMs = gDiscReadMs; lastJumps = gIsoRdJumps;
	for(int i = 0; i < SRC_N; i++){ lastCmd[i] = gSrcCmd[i]; lastBytesS[i] = gSrcBytes[i]; lastJ[i] = gSrcJumps[i]; lastMs[i] = gSrcMs[i]; }
	if(gTrace){
		u32 head = gTraceHead;
		if(head - gTraceTail > TRACE_N){
			printf("DVDT lost %u\n", (unsigned)(head - gTraceTail - TRACE_N));
			gTraceTail = head - TRACE_N;
		}
		for(; gTraceTail != head; gTraceTail++){
			u32 *e = gTrace[gTraceTail % TRACE_N];
			printf("DVDT %u %c %u %u %u\n", (unsigned)e[0], "iao"[e[1]], (unsigned)e[2],
			       (unsigned)(e[3] >> 16), (unsigned)(e[3] & 0xFFFF));
		}
	}
}


static int fsDvdRead(u32 sector, u32 count, void *dst)
{
	static dvdcmdblk blk;                 /* callers hold m->lock: one at a time */
	gIsoRdBusy = sector + 1;
	int jump = sector != gIsoRdNext;
	if(jump) gIsoRdJumps++;
	gIsoRdNext = sector + count;
	int src = gSrcAbs ? SRC_AUD : (gImgEnd && sector >= gImgLba && sector < gImgEnd) ? SRC_IMG : SRC_OTH;
	for(int attempt = 0; attempt < 8; attempt++){
		if(DVD_ReadAbsAsyncPrio(&blk, dst, count * FS_SECTOR,
		                        (s64)sector * FS_SECTOR, NULL, 2) < 0){
			usleep(2000);
			continue;
		}
		u64 t0 = gettime();
		if(!gDiscFirstTime) gDiscFirstTime = t0;
		gDiscCommands++;
		for(;;){
			s32 st = DVD_GetCmdBlockStatus(&blk);
			if(st == DVD_STATE_END){
				u32 elapsed = diff_msec(t0, gettime());
				gDiscBytes += count * FS_SECTOR;
				gDiscReadMs += elapsed;
				if(elapsed > gDiscMaxMs) gDiscMaxMs = elapsed;
				gSrcCmd[src]++; gSrcBytes[src] += count * FS_SECTOR; gSrcJumps[src] += jump; gSrcMs[src] += elapsed;
				if(gTrace){
					u32 *e = gTrace[gTraceHead % TRACE_N];
					e[0] = diff_msec(gDiscFirstTime, t0); e[1] = src; e[2] = sector;
					e[3] = count << 16 | (elapsed > 0xFFFF ? 0xFFFF : elapsed);
					gTraceHead++;
				}
				gIsoRdBusy = 0;
				return 1;
			}
			if(st == DVD_STATE_FATAL_ERROR)
				break;
			if(diff_msec(t0, gettime()) > 1500){
				gIsoRdTimeouts++;
				printf("DVD: read %u+%u silent 1.5s, re-issue %d\n",
				       (unsigned)sector, (unsigned)count, attempt);
				break;
			}
			usleep(500);
		}
	}
	gIsoRdBusy = 0;
	return 0;
}

/* B65: single issue by default. The double issue was insurance against a
 * Dolphin FastDiscSpeed quirk; on the real drive (CAV ~2-3MB/s, ~128ms seeks)
 * it halves the bandwidth. Runs use emulated disc speed now; flip to 1 if the
 * index tripwire or garbage loads return. */
#ifndef FS_DOUBLE_READ
#define FS_DOUBLE_READ 0
#endif
static int fsReadSectors(FsMount *m, u32 sector, u32 count, void *dst)
{
	(void)m;
#if FS_DOUBLE_READ
	fsDvdRead(sector, count, dst);
#endif
	return fsDvdRead(sector, count, dst);
}

/* Bounce path: any offset, any length, any destination alignment. */
static int fsReadDisc(FsMount *m, u64 offset, u32 len, void *dst)
{
	u8 *out = (u8*)dst;
	while(len > 0){
		u32 base = (u32)(offset / FS_SECTOR) & ~(FS_WINDOW_SECS - 1);
		if(m->windowBase != base){
			u32 n = FS_WINDOW_SECS;
			if(m->totalSectors && base + n > m->totalSectors){
				if(base >= m->totalSectors)
					return 0;
				n = m->totalSectors - base;
				memset(m->window, 0, FS_WINDOW_SECS * FS_SECTOR);
			}
			if(!fsReadSectors(m, base, n, m->window)){
				m->windowBase = ~0u;
				return 0;
			}
			m->windowBase = base;
		}
		u32 winOff = (u32)(offset - (u64)base * FS_SECTOR);
		u32 take = FS_WINDOW_SECS * FS_SECTOR - winOff;
		if(take > len)
			take = len;
		memcpy(out, m->window + winOff, take);
		out += take;
		offset += take;
		len -= take;
	}
	return 1;
}

/* ------------------------------------------------------------- path hashing */

static u64 fsHashInit(void) { return 14695981039346656037ull; }
static u64 fsHashByte(u64 h, u8 c) { return (h ^ c) * 1099511628211ull; }

static u8 fsLower(u8 c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Hash a game path: strips "dvd:" and leading slashes, folds case, treats
 * '\' as '/', collapses duplicate separators, trims trailing separators and
 * whitespace (FAT forgave "file.dat " and "data/"; so do we). */
static u64 fsHashPath(const char *path, int *outIsEmpty)
{
	const char *p = strchr(path, ':');
	p = p ? p + 1 : path;
	int end = strlen(p);
	while(end > 0){
		char c = p[end-1];
		if(c == '/' || c == '\\' || c == ' ' || c == '\t' || c == '\r')
			end--;
		else
			break;
	}
	u64 h = fsHashInit();
	int wrote = 0, pendingSep = 0;
	for(int i = 0; i < end; i++){
		u8 c = (u8)p[i];
		if(c == '/' || c == '\\'){
			if(wrote)
				pendingSep = 1;
			continue;
		}
		if(pendingSep){
			h = fsHashByte(h, '/');
			pendingSep = 0;
		}
		h = fsHashByte(h, fsLower(c));
		wrote = 1;
	}
	if(outIsEmpty)
		*outIsEmpty = !wrote;
	return h;
}

static FsEnt *fsLookup(FsMount *m, const char *path)
{
	int empty = 0;
	u64 h = fsHashPath(path, &empty);
	if(empty)
		return &m->ent[0];      /* root */
	for(u32 i = 0; i < m->entN; i++)
		if(m->ent[i].hash == h)
			return &m->ent[i];
	return NULL;
}

static u64 fsIndexSum(FsMount *m)
{
	u64 h = fsHashInit();
	const u8 *b = (const u8*)m->ent;
	for(u32 i = 0; i < m->entN * sizeof(FsEnt); i++)
		h = fsHashByte(h, b[i]);
	return h;
}

/* --------------------------------------------------------------- mount walk */

/* ISO9660 directory record fields (ECMA-119). Big-endian halves of the
 * both-endian fields — this code is PPC-only. */
#define REC_LEN(r)      ((r)[0])
#define REC_LBA(r)      (((u32)(r)[6]<<24)|((u32)(r)[7]<<16)|((u32)(r)[8]<<8)|(r)[9])
#define REC_SIZE(r)     (((u32)(r)[14]<<24)|((u32)(r)[15]<<16)|((u32)(r)[16]<<8)|(r)[17])
#define REC_FLAGS(r)    ((r)[25])
#define REC_NAMELEN(r)  ((r)[32])
#define REC_NAME(r)     ((const char*)&(r)[33])

typedef struct { u32 lba, size; char name[64]; } FsPendingDir;

static int fsAddEntry(FsMount *m, u64 hash, u32 lba, u32 size, int isdir)
{
	if(m->entN >= FS_MAX_ENTRIES){
		printf("dvdfs: entry cap hit\n");
		return 0;
	}
	/* collisions are ~impossible at 64 bits over 6k paths, but a silent one
	 * would open the wrong file forever — refuse the mount instead */
	for(u32 i = 0; i < m->entN; i++)
		if(m->ent[i].hash == hash){
			printf("dvdfs: hash collision, index unusable\n");
			return 0;
		}
	m->ent[m->entN].hash = hash;
	m->ent[m->entN].lba = lba;
	m->ent[m->entN].size = size | (isdir ? FS_DIRFLAG : 0);
	m->entN++;
	return 1;
}

static int fsWalkDir(FsMount *m, u32 lba, u32 size, char *path, int pathLen, int depth)
{
	if(depth > 8)
		return 0;
	FsPendingDir *kids = malloc(sizeof(FsPendingDir) * 64);
	if(kids == NULL)
		return 0;
	int nKids = 0, ok = 1;
	u8 rec[256];

	for(u32 off = 0; off < size; ){
		u8 lenByte;
		if(!fsReadDisc(m, (u64)lba * FS_SECTOR + off, 1, &lenByte)){ ok = 0; break; }
		if(lenByte == 0){
			off = (off / FS_SECTOR + 1) * FS_SECTOR;    /* records never cross sectors */
			continue;
		}
		if(!fsReadDisc(m, (u64)lba * FS_SECTOR + off, lenByte, rec)){ ok = 0; break; }
		off += lenByte;

		u32 nameLen = REC_NAMELEN(rec);
		if(nameLen == 0 || nameLen > FS_MAX_NAME)
			continue;
		if(nameLen == 1 && (REC_NAME(rec)[0] == 0 || REC_NAME(rec)[0] == 1))
			continue;                                   /* "." and ".." */

		/* primary-tree name: uppercase, may carry ";1" — strip it */
		char name[FS_MAX_NAME + 1];
		memcpy(name, REC_NAME(rec), nameLen);
		name[nameLen] = 0;
		char *semi = strchr(name, ';');
		if(semi)
			*semi = 0;
		if(name[0] == 0)
			continue;

		u64 h = fsHashInit();
		for(int i = 0; i < pathLen; i++)
			h = fsHashByte(h, (u8)path[i]);
		if(pathLen)
			h = fsHashByte(h, '/');
		for(char *c = name; *c; c++)
			h = fsHashByte(h, fsLower((u8)*c));

		int isdir = (REC_FLAGS(rec) & 2) != 0;
		if(!fsAddEntry(m, h, REC_LBA(rec) + rec[1], REC_SIZE(rec), isdir)){ ok = 0; break; }
		if(isdir){
			/* a subtree this walk cannot represent must fail the mount
			 * loudly, same policy as the entry cap — a silent skip is
			 * ENOENT at 200 km/h months from now */
			if(nKids >= 64 || strlen(name) > 63){
				printf("dvdfs: dir cap hit at '%s'\n", name);
				ok = 0;
				break;
			}
			kids[nKids].lba = REC_LBA(rec) + rec[1];
			kids[nKids].size = REC_SIZE(rec);
			strcpy(kids[nKids].name, name);
			nKids++;
		}
	}

	for(int k = 0; ok && k < nKids; k++){
		int nl = strlen(kids[k].name);
		if(pathLen + 1 + nl >= 240){
			printf("dvdfs: path cap hit at '%s'\n", kids[k].name);
			ok = 0;
			break;
		}
		if(pathLen)
			path[pathLen] = '/';
		for(int i = 0; i <= nl; i++)
			path[pathLen + (pathLen ? 1 : 0) + i] = (char)fsLower((u8)kids[k].name[i]);
		ok = fsWalkDir(m, kids[k].lba, kids[k].size, path,
		    pathLen + (pathLen ? 1 : 0) + nl, depth + 1);
		path[pathLen] = 0;
	}
	free(kids);
	return ok;
}

/* ------------------------------------------------------------ devoptab ops */

static FsMount *fsFromPath(const char *path)
{
	/* single mount; accept anything once mounted (newlib routed it to us) */
	return gMount.ent ? &gMount : NULL;
}

/* B69: raw sector read for CdStream's audio channel (absolute disc LBA). */
int fsReadSectorsAbs(u32 lba, u32 count, void *dst)
{
	FsMount *m = fsFromPath("dvd:/");
	if(m == NULL) return 0;
	LWP_MutexLock(m->lock);   /* one DVD command at a time, like fs_read */
	gSrcAbs = 1;
	int ok = fsReadSectors(m, lba, count, dst);
	gSrcAbs = 0;
	LWP_MutexUnlock(m->lock);
	return ok;
}
/* B68: the audio streams read the disc through CdStream on their own channel;
 * they need the file's extent, not a FILE*. The index is immutable after mount. */
int fsLookupLba(const char *path, u32 *lba, u32 *size)
{
	FsMount *m = fsFromPath(path);
	if(m == NULL) return 0;
	FsEnt *e = fsLookup(m, path);
	if(e == NULL || (e->size & 0x80000000u)) return 0;
	*lba = e->lba; *size = e->size;
	return 1;
}
static int fs_open(struct _reent *r, void *fileStruct, const char *path, int flags, int mode)
{
	(void)mode;
	FsFile *f = (FsFile*)fileStruct;
	FsMount *m = fsFromPath(path);
	if(m == NULL){ r->_errno = ENODEV; return -1; }
	if(flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)){
		r->_errno = EROFS;
		return -1;
	}
	LWP_MutexLock(m->lock);
	if(fsIndexSum(m) != m->indexSum){
		/* tripwire: something scribbled the index — say so loudly once */
		static int said;
		if(!said++)
			printf("dvdfs: INDEX CORRUPTED (scribbler)\n");
	}
	FsEnt *e = fsLookup(m, path);
	LWP_MutexUnlock(m->lock);
	if(e == NULL){ r->_errno = ENOENT; return -1; }
	if(e->size & FS_DIRFLAG){ r->_errno = EISDIR; return -1; }
	f->lba = e->lba;
	f->size = e->size & ~FS_DIRFLAG;
	f->pos = 0;
	f->isdir = 0;
	f->inUse = 1;
	return (int)(intptr_t)fileStruct;
}

static int fs_close(struct _reent *r, void *fd)
{
	FsFile *f = (FsFile*)fd;
	if(!f->inUse){ r->_errno = EBADF; return -1; }
	f->inUse = 0;
	return 0;
}

static ssize_t fs_read(struct _reent *r, void *fd, char *ptr, size_t len)
{
	FsFile *f = (FsFile*)fd;
	FsMount *m = &gMount;
	if(!f->inUse){ r->_errno = EBADF; return -1; }
	if(f->pos >= f->size || len == 0)
		return 0;
	if(len > f->size - f->pos)
		len = f->size - f->pos;

	u8 *out = (u8*)ptr;
	u32 remaining = (u32)len;
	gIsoRdN++;
	LWP_MutexLock(m->lock);

	/* direct DMA path: sector-aligned position, 32-byte-aligned destination,
	 * whole sectors — the streaming buffers (2048-aligned) take this */
	while(remaining >= FS_SECTOR && (f->pos & (FS_SECTOR-1)) == 0 &&
	      ((uintptr_t)out & 31) == 0){
		u32 nsec = remaining / FS_SECTOR;
		if(!fsReadSectors(m, f->lba + f->pos / FS_SECTOR, nsec, out)){
			LWP_MutexUnlock(m->lock);
			r->_errno = EIO;
			return -1;
		}
		u32 got = nsec * FS_SECTOR;
		out += got; f->pos += got; remaining -= got;
	}
	if(remaining > 0){
		if(!fsReadDisc(m, (u64)f->lba * FS_SECTOR + f->pos, remaining, out)){
			LWP_MutexUnlock(m->lock);
			r->_errno = EIO;
			return -1;
		}
		f->pos += remaining;
	}
	LWP_MutexUnlock(m->lock);
	return (ssize_t)len;
}

static off_t fs_seek(struct _reent *r, void *fd, off_t pos, int dir)
{
	FsFile *f = (FsFile*)fd;
	if(!f->inUse){ r->_errno = EBADF; return -1; }
	s64 next;
	switch(dir){
	case SEEK_SET: next = pos; break;
	case SEEK_CUR: next = (s64)f->pos + pos; break;
	case SEEK_END: next = (s64)f->size + pos; break;
	default: r->_errno = EINVAL; return -1;
	}
	/* pos == size is the standard ftell size probe; past it is refused,
	 * matching the old driver the game grew up against */
	if(next < 0 || next > (s64)f->size){ r->_errno = EINVAL; return -1; }
	f->pos = (u32)next;
	return (off_t)next;
}

static void fsStat(u32 lba, u32 sizeField, struct stat *st)
{
	memset(st, 0, sizeof *st);
	st->st_ino = lba;
	st->st_size = sizeField & ~FS_DIRFLAG;
	st->st_mode = (sizeField & FS_DIRFLAG) ? (S_IFDIR | 0555) : (S_IFREG | 0444);
	st->st_nlink = 1;
	st->st_blksize = FS_SECTOR;
	st->st_blocks = (st->st_size + 511) / 512;
}

static int fs_fstat(struct _reent *r, void *fd, struct stat *st)
{
	FsFile *f = (FsFile*)fd;
	if(!f->inUse){ r->_errno = EBADF; return -1; }
	fsStat(f->lba, f->size | (f->isdir ? FS_DIRFLAG : 0), st);
	return 0;
}

static int fs_stat(struct _reent *r, const char *file, struct stat *st)
{
	FsMount *m = fsFromPath(file);
	if(m == NULL){ r->_errno = ENODEV; return -1; }
	LWP_MutexLock(m->lock);
	FsEnt *e = fsLookup(m, file);
	LWP_MutexUnlock(m->lock);
	if(e == NULL){ r->_errno = ENOENT; return -1; }
	fsStat(e->lba, e->size, st);
	return 0;
}

static int fs_chdir(struct _reent *r, const char *name)
{
	FsMount *m = fsFromPath(name);
	if(m == NULL){ r->_errno = ENODEV; return -1; }
	LWP_MutexLock(m->lock);
	FsEnt *e = fsLookup(m, name);
	LWP_MutexUnlock(m->lock);
	if(e == NULL){ r->_errno = ENOENT; return -1; }
	if(!(e->size & FS_DIRFLAG)){ r->_errno = ENOTDIR; return -1; }
	return 0;       /* newlib keeps the cwd string; existence is all we owe */
}

static DIR_ITER *fs_diropen(struct _reent *r, DIR_ITER *dirState, const char *path)
{
	FsDir *d = (FsDir*)dirState->dirStruct;
	FsMount *m = fsFromPath(path);
	if(m == NULL){ r->_errno = ENODEV; return NULL; }
	LWP_MutexLock(m->lock);
	FsEnt *e = fsLookup(m, path);
	LWP_MutexUnlock(m->lock);
	if(e == NULL){ r->_errno = ENOENT; return NULL; }
	if(!(e->size & FS_DIRFLAG)){ r->_errno = ENOTDIR; return NULL; }
	d->lba = e->lba;
	d->size = e->size & ~FS_DIRFLAG;
	d->pos = 0;
	d->inUse = 1;
	return dirState;
}

static int fs_dirreset(struct _reent *r, DIR_ITER *dirState)
{
	FsDir *d = (FsDir*)dirState->dirStruct;
	if(!d->inUse){ r->_errno = EBADF; return -1; }
	d->pos = 0;
	return 0;
}

static int fs_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *st)
{
	FsDir *d = (FsDir*)dirState->dirStruct;
	FsMount *m = &gMount;
	if(!d->inUse){ r->_errno = EBADF; return -1; }
	u8 rec[256];
	LWP_MutexLock(m->lock);
	while(d->pos < d->size){
		u8 lenByte;
		if(!fsReadDisc(m, (u64)d->lba * FS_SECTOR + d->pos, 1, &lenByte))
			break;
		if(lenByte == 0){
			d->pos = (d->pos / FS_SECTOR + 1) * FS_SECTOR;
			continue;
		}
		if(!fsReadDisc(m, (u64)d->lba * FS_SECTOR + d->pos, lenByte, rec))
			break;
		d->pos += lenByte;
		u32 nameLen = REC_NAMELEN(rec);
		if(nameLen == 0 || nameLen > FS_MAX_NAME)
			continue;
		if(nameLen == 1 && (REC_NAME(rec)[0] == 0 || REC_NAME(rec)[0] == 1))
			continue;
		LWP_MutexUnlock(m->lock);
		u32 n = nameLen < FS_MAX_NAME ? nameLen : FS_MAX_NAME - 1;
		memcpy(filename, REC_NAME(rec), n);
		filename[n] = 0;
		char *semi = strchr(filename, ';');
		if(semi)
			*semi = 0;
		if(st)
			fsStat(REC_LBA(rec) + rec[1],
			    REC_SIZE(rec) | ((REC_FLAGS(rec) & 2) ? FS_DIRFLAG : 0), st);
		return 0;
	}
	LWP_MutexUnlock(m->lock);
	r->_errno = ENOENT;     /* end-of-directory, per devoptab contract */
	return -1;
}

static int fs_dirclose(struct _reent *r, DIR_ITER *dirState)
{
	FsDir *d = (FsDir*)dirState->dirStruct;
	if(!d->inUse){ r->_errno = EBADF; return -1; }
	d->inUse = 0;
	return 0;
}

static int fs_statvfs(struct _reent *r, const char *path, struct statvfs *buf)
{
	(void)r; (void)path;
	memset(buf, 0, sizeof *buf);
	buf->f_bsize = FS_SECTOR;
	buf->f_frsize = FS_SECTOR;
	buf->f_blocks = gMount.totalSectors;
	buf->f_flag = ST_RDONLY | ST_NOSUID;
	buf->f_namemax = FS_MAX_NAME;
	return 0;
}

/* ------------------------------------------------------------ mount/unmount */

bool ISO9660_MountDbg(const char *name, const DISC_INTERFACE *disc)
{
	FsMount *m = &gMount;
	if(m->ent)
		return false;
	if(!disc->startup()){
		snprintf(isoMountErr, sizeof isoMountErr, "disc startup failed");
		printf("dvdfs: disc startup failed\n");
		return false;
	}

	memset(m, 0, sizeof *m);
	m->disc = disc;
	m->windowBase = ~0u;
	m->window = memalign(32, FS_WINDOW_SECS * FS_SECTOR);
	m->ent = malloc(sizeof(FsEnt) * FS_MAX_ENTRIES);
	if(m->window == NULL || m->ent == NULL)
		goto fail;
	if(LWP_MutexInit(&m->lock, false) != 0)
		goto fail;

	/* PVD at sector 16: "CD001", root record at offset 156, volume space
	 * (big-endian half) at offset 84. Read through the (32-aligned) window. */
	const u8 *pvd = m->window;
	if(!fsReadSectors(m, 16, 1, m->window) || memcmp(pvd + 1, "CD001", 5) != 0){
		printf("dvdfs: no ISO9660 PVD\n");
		goto fail;
	}
	m->totalSectors = ((u32)pvd[84]<<24)|((u32)pvd[85]<<16)|((u32)pvd[86]<<8)|pvd[87];
	const u8 *root = pvd + 156;
	u32 rootLba = REC_LBA(root) + root[1];
	u32 rootSize = REC_SIZE(root);

	/* entry 0 is the root itself (empty-path lookups resolve here) */
	m->ent[0].hash = 0;
	m->ent[0].lba = rootLba;
	m->ent[0].size = rootSize | FS_DIRFLAG;
	m->entN = 1;

	char path[256] = "";
	if(!fsWalkDir(m, rootLba, rootSize, path, 0, 0)){
		printf("dvdfs: directory walk failed\n");
		goto fail;
	}
	/* the walk is done — give back the unused tail of the cap allocation */
	FsEnt *packed = realloc(m->ent, sizeof(FsEnt) * m->entN);
	if(packed)
		m->ent = packed;
	m->indexSum = fsIndexSum(m);
	printf("dvdfs: %u entries, vol=%u sectors\n", (unsigned)m->entN,
	    (unsigned)m->totalSectors);
	{
		FsEnt *img = fsLookup(m, "/models/gta3.img");
		if(img){ gImgLba = img->lba; gImgEnd = img->lba + ((img->size & ~FS_DIRFLAG) + FS_SECTOR - 1) / FS_SECTOR; }
		if(fsLookup(m, "/dvdtrace.txt") != NULL && (gTrace = malloc(sizeof(*gTrace) * TRACE_N)) != NULL)
			printf("dvdtrace.txt: DVDT line per command (ms src lba sectors elapsed), 64K ring\n");
	}

	/* register the devoptab; the name lives in the mount */
	strncpy(m->name, name, sizeof m->name - 1);
	memset(&m->dotab, 0, sizeof m->dotab);
	m->dotab.name = m->name;
	m->dotab.structSize = sizeof(FsFile);
	m->dotab.open_r = fs_open;
	m->dotab.close_r = fs_close;
	m->dotab.read_r = fs_read;
	m->dotab.seek_r = fs_seek;
	m->dotab.fstat_r = fs_fstat;
	m->dotab.stat_r = fs_stat;
	m->dotab.chdir_r = fs_chdir;
	m->dotab.dirStateSize = sizeof(FsDir);
	m->dotab.diropen_r = fs_diropen;
	m->dotab.dirreset_r = fs_dirreset;
	m->dotab.dirnext_r = fs_dirnext;
	m->dotab.dirclose_r = fs_dirclose;
	m->dotab.statvfs_r = fs_statvfs;
	m->dotab.deviceData = m;
	if(AddDevice(&m->dotab) < 0){
		printf("dvdfs: AddDevice failed\n");
		goto fail;
	}
	return true;

fail:
	free(m->window);
	free(m->ent);
	m->window = NULL;
	m->ent = NULL;
	return false;
}

void ISO9660_UnmountDbg(const char *name)
{
	FsMount *m = &gMount;
	if(m->ent == NULL)
		return;
	RemoveDevice(name);
	LWP_MutexDestroy(m->lock);
	free(m->window);
	free(m->ent);
	m->window = NULL;
	m->ent = NULL;
	m->entN = 0;
}
