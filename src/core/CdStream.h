#ifndef __GTA_CDSTREAM_H__
#define __GTA_CDSTREAM_H__

// IMPORTANT: this file has to be includable from C,
// so no custom types like int32 &c.

#define CDSTREAM_SECTOR_SIZE 2048

#define _GET_INDEX(a) (a >> 24)
#define _GET_OFFSET(a)  (a & 0xFFFFFF)

#ifdef    __cplusplus
extern "C"
{
#endif

// These have to match IOP and also some SCE defines
enum
{
	STREAM_NONE            = 0,
	STREAM_SUCCESS         = 1,
	STREAM_READING         = 0xFF,
	STREAM_ERROR           = 0xFE,	// SCECdErREADCFR
	STREAM_ERROR_NOCD      = 0xFD,	// SCECdErREADCF
	STREAM_ERROR_WRONGCD   = 0xFC,
	STREAM_ERROR_OPENCD    = 0xFB,
	STREAM_WAITING         = 0xFA
};

void CdStreamInit(int numChannels);
int CdStreamRead(int channel, void *buffer, unsigned int offset, unsigned int size);
int CdStreamGetStatus(int channel);
int CdStreamGetLastPosn(void);
int CdStreamSync(int channel);

#ifndef GTA_PS2
typedef struct Queue Queue;
struct Queue
{
	int32 *items;
	int32 head;
	int32 tail;
	int32 size;
};

VALIDATE_SIZE(Queue, 0x10);

void CdStreamShutdown(void);
// on IOP on PS2
void CdStreamInitThread(void);
void AddToQueue(Queue *queue, int item);
int32 GetFirstInQueue(Queue *queue);
void RemoveFirstInQueue(Queue *queue);

// not on PS2 at all
uint32 GetGTA3ImgSize(void);
bool CdStreamAddImage(char const *path);
char *CdStreamGetImageName(int cd);
void CdStreamRemoveImages(void);
int32 CdStreamGetNumImages(void);
#ifdef GTA_OGC
uint32 CdStreamGetImageSectorCount(int image);
#endif
#endif

#ifdef FLUSHABLE_STREAMING
extern bool flushStream[MAX_CDCHANNELS];
#endif

#ifdef    __cplusplus
}
#endif


#ifdef GTA_OGC
// libfat is one shared resource: the streaming worker reads the archive
// through it while the main thread writes dvd:/ logs through it. Serialise
// every use behind one lock — see the note in CdStream_gamecube.cpp for what
// not doing so cost.
extern "C" {
void CdStreamFsLock(void);
bool CdStreamFsTryLock(void);
void CdStreamFsUnlock(void);
}
struct CdStreamFsGuard {
	CdStreamFsGuard(void) { CdStreamFsLock(); }
	~CdStreamFsGuard(void) { CdStreamFsUnlock(); }
};
// Declare at the top of the scope that owns the fopen, so the lock spans
// open through close rather than just the open. Named by line so two file
// operations in one scope do not collide.
#define DVD_FS_CAT2(a, b) a##b
#define DVD_FS_CAT(a, b) DVD_FS_CAT2(a, b)
#define DVD_FS_GUARD CdStreamFsGuard DVD_FS_CAT(_dvdFsGuard_, __LINE__)
#endif

#endif // __GTA_CDSTREAM_H__
