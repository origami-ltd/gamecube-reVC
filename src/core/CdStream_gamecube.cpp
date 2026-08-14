#include "common.h"
#include "CdStream.h"
#include "crossplatform.h"

#include <stdio.h>

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
}

void
CdStreamInitThread(void)
{
}

int32
CdStreamRead(int32 channel, void *buffer, uint32 offset, uint32 size)
{
	if(channel < 0 || channel >= channelCount || buffer == nil){
		if(channel >= 0 && channel < channelCount)
			channelStatus[channel] = STREAM_ERROR;
		return STREAM_ERROR;
	}

	uint32 image = _GET_INDEX(offset);
	uint32 sector = _GET_OFFSET(offset);
	if(image >= (uint32)imageCount || imageFiles[image] == nil || size == 0 ||
	   size > UINT32_MAX - offset || sector > imageSectors[image] ||
	   size > imageSectors[image] - sector){
		channelStatus[channel] = STREAM_ERROR;
		return STREAM_ERROR;
	}

	FILE *file = imageFiles[image];
	uint64 byteOffset = (uint64)sector * CDSTREAM_SECTOR_SIZE;
	uint64 byteCount64 = (uint64)size * CDSTREAM_SECTOR_SIZE;
	if(byteOffset > imageBytes[image] || byteCount64 > imageBytes[image] - byteOffset ||
	   (uint64)(off_t)byteOffset != byteOffset || byteCount64 > SIZE_MAX ||
	   fseeko(file, (off_t)byteOffset, SEEK_SET) != 0){
		channelStatus[channel] = STREAM_ERROR;
		return STREAM_ERROR;
	}
	size_t byteCount = (size_t)byteCount64;
	if(
	   fread(buffer, 1, byteCount, file) != byteCount) {
		channelStatus[channel] = STREAM_ERROR;
		return STREAM_ERROR;
	}

	lastPosition = offset + size;
	channelStatus[channel] = STREAM_NONE;
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
	return CdStreamGetStatus(channel);
}

void
CdStreamShutdown(void)
{
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
