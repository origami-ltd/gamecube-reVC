// mc:/ — a minimal devoptab over the GameCube memory card (slot A), so the
// game's userfiles (gta_vc.set, reVC.ini, the save slots) live on the card
// like a retail GC title's would. Whole-file semantics: open pulls the file
// into RAM, writes land in RAM, close flushes back in one CARD_Write. The
// files involved are a few KB; the biggest (a save) is under 200KB.
//
// GC build only: the Wii dev build keeps dvd:/userfiles on the SD card.
#ifndef HW_RVL

#include <gccore.h>
#include <ogc/card.h>
#include <ogc/system.h>
#include <sys/iosupport.h>
#include <sys/errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <malloc.h>

enum { MC_MAXSIZE = 256*1024 };

typedef struct {
	char  name[CARD_FILENAMELEN];
	u8   *buf;
	u32   cap;       // allocated bytes (grows on write)
	u32   size;      // valid bytes
	u32   pos;
	int   dirty;
	int   writable;
} McFile;

static u8 cardWork[CARD_WORKAREA_SIZE] ATTRIBUTE_ALIGN(32);
static int cardMounted;
static u32 cardSector;

static int mcMount(void)
{
	if(cardMounted)
		return 1;
	CARD_Init("GRVC", "01");
	// A couple of tries: the card firmware needs a beat after power-on.
	for(int i = 0; i < 3; i++){
		if(CARD_Mount(CARD_SLOTA, cardWork, NULL) >= 0){
			CARD_GetSectorSize(CARD_SLOTA, &cardSector);
			if(cardSector == 0) cardSector = 8192;
			cardMounted = 1;
			printf("mc: slot A mounted, sector %u\n", cardSector);
			return 1;
		}
	}
	printf("mc: slot A mount FAILED\n");
	return 0;
}

// basename after "mc:/"; CARD names are flat
static const char *mcName(const char *path)
{
	const char *p = strrchr(path, '/');
	p = p ? p + 1 : path;
	const char *c = strrchr(p, ':');
	return c ? c + 1 : p;
}

static int mc_open(struct _reent *r, void *fd, const char *path, int flags, int mode)
{
	(void)mode;
	McFile *f = (McFile*)fd;
	memset(f, 0, sizeof(*f));
	if(!mcMount()){ r->_errno = EIO; return -1; }
	strncpy(f->name, mcName(path), sizeof(f->name)-1);
	f->writable = (flags & (O_WRONLY|O_RDWR)) != 0;

	// Demand-sized: a flat 256KB per open was pure MEM1 pressure on a build
	// whose texture loads already die of allocation failure. Read-opens get
	// the file's real size; write-opens start at one sector and grow.
	card_file cf;
	int have = CARD_Open(CARD_SLOTA, f->name, &cf) >= 0;
	u32 want = have ? ((u32)cf.len > MC_MAXSIZE ? MC_MAXSIZE : (u32)cf.len) : 8192;
	if(want < 8192) want = 8192;
	f->cap = want;
	f->buf = (u8*)memalign(32, f->cap);
	if(f->buf == NULL){
		if(have) CARD_Close(&cf);
		r->_errno = ENOMEM;
		return -1;
	}

	if(have){
		u32 len = (u32)cf.len > f->cap ? f->cap : (u32)cf.len;
		u32 got = 0;
		// CARD reads want 512-multiples; the file length already is one.
		for(u32 off = 0; off < len; off += 512){
			if(CARD_Read(&cf, f->buf + off, 512, off) < 0)
				break;
			got = off + 512;
		}
		CARD_Close(&cf);
		// A failed sector read leaves the tail unwritten: report only the
		// bytes actually read, so a corrupt card file looks SHORT to the
		// game (LoadSettings' header/version check then falls back to
		// defaults) instead of valid-length with a garbage tail.
		f->size = got < len ? got : len;
	}else if(!f->writable){
		free(f->buf); f->buf = NULL;
		r->_errno = ENOENT;
		return -1;
	}
	if(have && !f->size)
		; // zero-length card file: nothing read, buffer stays
	if(f->writable && (flags & O_TRUNC))
		f->size = 0;
	if(flags & O_APPEND)
		f->pos = f->size;
	return 0;
}

static int mc_close(struct _reent *r, void *fd)
{
	McFile *f = (McFile*)fd;
	int ret = 0;
	if(f->dirty && f->writable && cardMounted){
		u32 padded = (f->size + cardSector - 1) / cardSector * cardSector;
		if(padded == 0) padded = cardSector;
		if(padded > f->cap){
			// pad space beyond the buffer: grow once for the flush
			u8 *nb = (u8*)memalign(32, padded);
			if(nb){ memcpy(nb, f->buf, f->size); free(f->buf); f->buf = nb; f->cap = padded; }
		}
		if(padded <= f->cap)
			memset(f->buf + f->size, 0, padded - f->size);
		card_file cf;
		int have = CARD_Open(CARD_SLOTA, f->name, &cf) >= 0;
		if(have && (u32)cf.len != padded){
			CARD_Close(&cf);
			CARD_Delete(CARD_SLOTA, f->name);
			have = 0;
		}
		if(!have){
			if(CARD_Create(CARD_SLOTA, f->name, padded, &cf) < 0){
				printf("mc: create %s (%u) failed\n", f->name, padded);
				r->_errno = ENOSPC; ret = -1;
				goto out;
			}
		}
		for(u32 off = 0; off < padded && ret == 0; off += cardSector)
			if(CARD_Write(&cf, f->buf + off, cardSector, off) < 0){
				r->_errno = EIO; ret = -1;
			}
		CARD_Close(&cf);
	}
out:
	free(f->buf);
	f->buf = NULL;
	return ret;
}

static ssize_t mc_write(struct _reent *r, void *fd, const char *buf, size_t len)
{
	McFile *f = (McFile*)fd;
	if(!f->writable){ r->_errno = EBADF; return -1; }
	if(f->pos + len > MC_MAXSIZE) len = MC_MAXSIZE - f->pos;
	if(f->pos + len > f->cap){
		u32 ncap = f->cap ? f->cap : 8192;
		while(ncap < f->pos + len) ncap <<= 1;
		if(ncap > MC_MAXSIZE) ncap = MC_MAXSIZE;
		u8 *nb = (u8*)memalign(32, ncap);
		if(nb == NULL){ r->_errno = ENOMEM; return -1; }
		memcpy(nb, f->buf, f->size);
		free(f->buf);
		f->buf = nb;
		f->cap = ncap;
	}
	memcpy(f->buf + f->pos, buf, len);
	f->pos += len;
	if(f->pos > f->size) f->size = f->pos;
	f->dirty = 1;
	return (ssize_t)len;
}

static ssize_t mc_read(struct _reent *r, void *fd, char *buf, size_t len)
{
	(void)r;
	McFile *f = (McFile*)fd;
	if(f->pos >= f->size) return 0;
	if(f->pos + len > f->size) len = f->size - f->pos;
	memcpy(buf, f->buf + f->pos, len);
	f->pos += len;
	return (ssize_t)len;
}

static off_t mc_seek(struct _reent *r, void *fd, off_t pos, int dir)
{
	McFile *f = (McFile*)fd;
	off_t base = dir == SEEK_SET ? 0 : dir == SEEK_CUR ? (off_t)f->pos : (off_t)f->size;
	off_t np = base + pos;
	if(np < 0 || np > (off_t)MC_MAXSIZE){ r->_errno = EINVAL; return -1; }
	f->pos = (u32)np;
	return np;
}

static int mc_fstat(struct _reent *r, void *fd, struct stat *st)
{
	(void)r;
	McFile *f = (McFile*)fd;
	memset(st, 0, sizeof(*st));
	st->st_size = f->size;
	st->st_mode = S_IFREG;
	return 0;
}

static int mc_stat(struct _reent *r, const char *path, struct stat *st)
{
	if(!mcMount()){ r->_errno = EIO; return -1; }
	card_file cf;
	if(CARD_Open(CARD_SLOTA, mcName(path), &cf) < 0){
		r->_errno = ENOENT; return -1;
	}
	memset(st, 0, sizeof(*st));
	st->st_size = cf.len;
	st->st_mode = S_IFREG;
	CARD_Close(&cf);
	return 0;
}

static int mc_unlink(struct _reent *r, const char *path)
{
	if(!mcMount()){ r->_errno = EIO; return -1; }
	if(CARD_Delete(CARD_SLOTA, mcName(path)) < 0){
		r->_errno = ENOENT; return -1;
	}
	return 0;
}

static const devoptab_t dotab_mc = {
	.name = "mc",
	.structSize = sizeof(McFile),
	.open_r = mc_open, .close_r = mc_close, .write_r = mc_write,
	.read_r = mc_read, .seek_r = mc_seek, .fstat_r = mc_fstat,
	.stat_r = mc_stat, .unlink_r = mc_unlink,
};

int GcCardMountDevice(void)
{
	if(FindDevice("mc:") >= 0)
		return 1;
	return AddDevice(&dotab_mc) >= 0;
}

#endif // !HW_RVL
