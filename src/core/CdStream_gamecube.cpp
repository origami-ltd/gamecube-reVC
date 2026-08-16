#include "common.h"
#include "CdStream.h"
#include "crossplatform.h"

#include <stdio.h>
#include <ogc/lwp.h>
#include <ogc/semaphore.h>
#include <ogc/mutex.h>

// Asynchronous disc reads.
//
// This used to fread() straight from CdStreamRead on the game thread, with
// CdStreamSync just returning the status of a read that had already finished.
// That is not the contract reVC is written against: it issues a read, keeps
// simulating, and only blocks in CdStreamSync when it actually needs the data.
// Doing the I/O inline meant every model, texture and collision load stalled
// the frame on the SD/DVD, which is what the stutter is — the frame timer
// showed it as work time with no vsync wait behind it.
//
// One worker thread per channel would be closer to the real DVD queue, but a
// single worker already takes the blocking read off the game thread, which is
// the part that costs frames. ponytail: one thread, one request slot per
// channel; add per-channel workers only if the queue depth measures deep.
struct CdRequest {
	void *buffer;
	uint32 offset;
	uint32 size;
	volatile int32 status;   // STREAM_* , or STREAM_READING while queued
};
static CdRequest requests[MAX_CDCHANNELS];
static lwp_t     ioThread = LWP_THREAD_NULL;
static sem_t     ioPending;
static mutex_t   ioLock = LWP_MUTEX_NULL;
static volatile bool ioQuit;
static volatile int32 ioQueue[MAX_CDCHANNELS+1];
static volatile int32 ioHead, ioTail;

// libfat is shared by this worker and by every dvd:/ log the game writes from
// the main thread, and nothing serialised the two. The evidence was in the
// harness the whole time: boot.sh has to run fsck_msdos on the SD image before
// every single boot to keep it mountable, which is what unserialised writes to
// a FAT volume look like. The failure mode when it goes further than corruption
// is a stop with no exception, no GP stall and near-zero CPU — the freeze the
// pause menu triggers, because opening it bursts synchronous texture reads
// through here while the 5-second heartbeat is writing hb.log from the main
// thread. It also explains why the freeze got easier to hit with the retrace
// wait off: the loop simply reaches the race more often.
//
// Recursive, so a log written from inside a path that already holds it cannot
// deadlock against itself.
static mutex_t fsLock = LWP_MUTEX_NULL;

extern "C" void
CdStreamFsLock(void)
{
	if(fsLock != LWP_MUTEX_NULL)
		LWP_MutexLock(fsLock);
}

extern "C" void
CdStreamFsUnlock(void)
{
	if(fsLock != LWP_MUTEX_NULL)
		LWP_MutexUnlock(fsLock);
}

static FILE *imageFiles[MAX_CDIMAGES];
static char imageNames[MAX_CDIMAGES][64];
static uint64 imageBytes[MAX_CDIMAGES];
static uint32 imageSectors[MAX_CDIMAGES];
static int32 imageCount;
static int32 channelCount;
static int32 lastPosition;
static int32 channelStatus[MAX_CDCHANNELS];

void
CdStreamInit(int32 numChannels)
{
	CdStreamRemoveImages();
	lastPosition = 0;
	channelCount = numChannels >= 0 && numChannels <= MAX_CDCHANNELS ? numChannels : 0;
	for(int32 i = 0; i < MAX_CDCHANNELS; i++)
		channelStatus[i] = STREAM_NONE;
	// Nothing in the game calls CdStreamInitThread — it is only declared in
	// CdStream.h — so start the worker here, from the entry point that is
	// actually called. Without this every read was queued to a thread that
	// did not exist and the game hung on the loading screen.
	CdStreamInitThread();
}

// Perform one queued read. Runs on the worker thread only.
static int32
CdStreamDoRead(int32 channel)
{
	CdRequest *r = &requests[channel];
	uint32 offset = r->offset, size = r->size;
	void *buffer = r->buffer;

	uint32 image = _GET_INDEX(offset);
	uint32 sector = _GET_OFFSET(offset);
	if(image >= (uint32)imageCount || imageFiles[image] == nil || size == 0 ||
	   size > UINT32_MAX - offset || sector > imageSectors[image] ||
	   size > imageSectors[image] - sector)
		return STREAM_ERROR;

	FILE *file = imageFiles[image];
	uint64 byteOffset = (uint64)sector * CDSTREAM_SECTOR_SIZE;
	uint64 byteCount64 = (uint64)size * CDSTREAM_SECTOR_SIZE;
	if(byteOffset > imageBytes[image] || byteCount64 > imageBytes[image] - byteOffset ||
	   (uint64)(off_t)byteOffset != byteOffset || byteCount64 > SIZE_MAX)
		return STREAM_ERROR;
	size_t byteCount = (size_t)byteCount64;

	CdStreamFsLock();
	bool ok = fseeko(file, (off_t)byteOffset, SEEK_SET) == 0 &&
	          fread(buffer, 1, byteCount, file) == byteCount;
	CdStreamFsUnlock();
	if(!ok)
		return STREAM_ERROR;

	lastPosition = offset + size;
	return STREAM_NONE;
}

static void*
CdStreamThread(void*)
{
	for(;;){
		LWP_SemWait(ioPending);
		if(ioQuit)
			break;
		LWP_MutexLock(ioLock);
		int32 ch = ioHead == ioTail ? -1 : ioQueue[ioHead];
		if(ch >= 0)
			ioHead = (ioHead + 1) % (MAX_CDCHANNELS+1);
		LWP_MutexUnlock(ioLock);
		if(ch < 0)
			continue;
		int32 st = CdStreamDoRead(ch);
		channelStatus[ch] = st;
		requests[ch].status = st;   // published last: Sync watches this
	}
	return nil;
}

void
CdStreamInitThread(void)
{
	if(ioThread != LWP_THREAD_NULL)
		return;
	ioQuit = false;
	ioHead = ioTail = 0;
	LWP_SemInit(&ioPending, 0, MAX_CDCHANNELS);
	LWP_MutexInit(&ioLock, false);
	if(fsLock == LWP_MUTEX_NULL)
		LWP_MutexInit(&fsLock, true);   // recursive, see the note above
	for(int32 i = 0; i < MAX_CDCHANNELS; i++)
		requests[i].status = STREAM_NONE;
	// Below the game thread so a queued read never preempts simulation, but
	// high enough to keep the disc busy while the frame runs.
	LWP_CreateThread(&ioThread, CdStreamThread, nil, nil, 16*1024, 80);
}

int32
CdStreamRead(int32 channel, void *buffer, uint32 offset, uint32 size)
{
	if(channel < 0 || channel >= channelCount || buffer == nil){
		if(channel >= 0 && channel < channelCount){
			channelStatus[channel] = STREAM_ERROR;
			requests[channel].status = STREAM_ERROR;
		}
		return STREAM_ERROR;
	}

	CdRequest *r = &requests[channel];
	r->buffer = buffer;
	r->offset = offset;
	r->size = size;

	// No worker: do it inline rather than queue work nobody will service.
	// Stuttery, but it always makes progress.
	if(ioThread == LWP_THREAD_NULL){
		int32 st = CdStreamDoRead(channel);
		r->status = st;
		channelStatus[channel] = st;
		return st == STREAM_ERROR ? STREAM_ERROR : STREAM_SUCCESS;
	}

	// Queue it and return. The game keeps running; CdStreamSync blocks only
	// when it genuinely needs the bytes.
	r->status = STREAM_READING;
	channelStatus[channel] = STREAM_READING;

	LWP_MutexLock(ioLock);
	ioQueue[ioTail] = channel;
	ioTail = (ioTail + 1) % (MAX_CDCHANNELS+1);
	LWP_MutexUnlock(ioLock);
	LWP_SemPost(ioPending);
	return STREAM_SUCCESS;
}

int32
CdStreamGetStatus(int32 channel)
{
	if(channel < 0 || channel >= channelCount)
		return STREAM_ERROR;
	return channelStatus[channel];
}

int32
CdStreamGetLastPosn(void)
{
	return lastPosition;
}

int32
CdStreamSync(int32 channel)
{
	if(channel < 0 || channel >= channelCount)
		return STREAM_ERROR;
	// Spin on the status the worker publishes, yielding so it gets scheduled.
	// A per-request semaphore was wrong here: its count is not reset per
	// request, so a read that completed without anyone calling Sync left the
	// count pending and the next Sync blocked forever — the loading screen
	// hang. Status is written once by the worker and read here; there is no
	// count to get out of step.
	while(requests[channel].status == STREAM_READING)
		LWP_YieldThread();
	int32 st = requests[channel].status;
	requests[channel].status = STREAM_NONE;
	channelStatus[channel] = STREAM_NONE;
	return st == STREAM_ERROR ? STREAM_ERROR : STREAM_NONE;
}

void
CdStreamShutdown(void)
{
	if(ioThread != LWP_THREAD_NULL){
		ioQuit = true;
		LWP_SemPost(ioPending);
		LWP_JoinThread(ioThread, nil);
		ioThread = LWP_THREAD_NULL;
	}
	CdStreamRemoveImages();
}

void
AddToQueue(Queue *queue, int32 item)
{
	queue->items[queue->tail] = item;
	queue->tail = (queue->tail + 1) % queue->size;
}

int32
GetFirstInQueue(Queue *queue)
{
	return queue->head == queue->tail ? -1 : queue->items[queue->head];
}

void
RemoveFirstInQueue(Queue *queue)
{
	if(queue->head != queue->tail)
		queue->head = (queue->head + 1) % queue->size;
}

uint32
GetGTA3ImgSize(void)
{
	return imageFiles[0] != nil && imageBytes[0] <= UINT32_MAX ? (uint32)imageBytes[0] : 0;
}

bool
CdStreamAddImage(char const *path)
{
	if(path == nil || imageCount >= MAX_CDIMAGES)
		return false;

	FILE *file = fcaseopen(path, "rb");
	if(file == nil)
		return false;
	if(strlen(path) >= sizeof(imageNames[0]) || fseeko(file, 0, SEEK_END) != 0){
		fclose(file);
		return false;
	}
	off_t size = ftello(file);
	if(size <= 0 || (uint64)size % CDSTREAM_SECTOR_SIZE != 0 ||
	   (uint64)size / CDSTREAM_SECTOR_SIZE > 0x1000000 ||
	   fseeko(file, 0, SEEK_SET) != 0){
		fclose(file);
		return false;
	}

	imageFiles[imageCount] = file;
	strcpy(imageNames[imageCount], path);
	imageBytes[imageCount] = (uint64)size;
	imageSectors[imageCount] = (uint32)((uint64)size / CDSTREAM_SECTOR_SIZE);
	imageCount++;
	return true;
}

char *
CdStreamGetImageName(int32 image)
{
	return image >= 0 && image < imageCount ? imageNames[image] : nil;
}

void
CdStreamRemoveImages(void)
{
	for(int32 i = 0; i < imageCount; i++) {
		fclose(imageFiles[i]);
		imageFiles[i] = nil;
		imageBytes[i] = 0;
		imageSectors[i] = 0;
		imageNames[i][0] = '\0';
	}
	imageCount = 0;
}

int32
CdStreamGetNumImages(void)
{
	return imageCount;
}

uint32
CdStreamGetImageSectorCount(int32 image)
{
	return image >= 0 && image < imageCount ? imageSectors[image] : 0;
}
