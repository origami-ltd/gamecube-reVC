#include "common.h"

#include "General.h"
#include "FileMgr.h"
#include "Directory.h"

#include <new>

CDirectory::CDirectory(int32 maxEntries)
 : numEntries(0), maxEntries(maxEntries)
{
	entries = maxEntries > 0 ? new(std::nothrow) DirectoryInfo[maxEntries] : nil;
	if(entries == nil)
		this->maxEntries = 0;
}

CDirectory::~CDirectory(void)
{
	delete[] entries;
}

int32
CDirectory::ReadEntry(int fd, DirectoryInfo &dirinfo)
{
	uint8 data[32];
	size_t size = CFileMgr::Read(fd, (char*)data, sizeof(data));
	if(size == 0)
		return 0;
	if(size != sizeof(data) || memchr(&data[8], '\0', sizeof(dirinfo.name)) == nil)
		return -1;
	dirinfo.offset = ReadLE32(&data[0]);
	dirinfo.size = ReadLE32(&data[4]);
	memcpy(dirinfo.name, &data[8], sizeof(dirinfo.name));
	return 1;
}

bool
CDirectory::WriteEntry(int fd, const DirectoryInfo &dirinfo)
{
	uint8 data[32];
	if(memchr(dirinfo.name, '\0', sizeof(dirinfo.name)) == nil)
		return false;
	WriteLE32(&data[0], dirinfo.offset);
	WriteLE32(&data[4], dirinfo.size);
	memcpy(&data[8], dirinfo.name, sizeof(dirinfo.name));
	return CFileMgr::Write(fd, (char*)data, sizeof(data)) == sizeof(data);
}

bool
CDirectory::ReadDirFile(const char *filename)
{
	int fd;
	int32 firstEntry, status;
	DirectoryInfo dirinfo;

	fd = CFileMgr::OpenFile(filename, "rb");
	if(fd <= 0)
		return false;
	firstEntry = numEntries;
	while((status = ReadEntry(fd, dirinfo)) > 0)
		if(!AddItem(dirinfo)){
			status = -1;
			break;
		}
	if(status < 0){
		numEntries = firstEntry;
		CFileMgr::CloseFile(fd);
		return false;
	}
	CFileMgr::CloseFile(fd);
	return true;
}

bool
CDirectory::WriteDirFile(const char *filename)
{
	int fd;
	fd = CFileMgr::OpenFileForWriting(filename);
	if(fd <= 0)
		return false;
	for(int32 i = 0; i < numEntries; i++)
		if(!WriteEntry(fd, entries[i])){
			CFileMgr::CloseFile(fd);
			return false;
		}
	CFileMgr::CloseFile(fd);
	return true;
}

bool
CDirectory::AddItem(const DirectoryInfo &dirinfo)
{
	if(entries == nil || numEntries >= maxEntries ||
	   memchr(dirinfo.name, '\0', sizeof(dirinfo.name)) == nil)
		return false;
#ifdef FIX_BUGS
	// don't add if already exists
	uint32 offset, size;
	if(FindItem(dirinfo.name, offset, size))
		return true;
#endif
	entries[numEntries++] = dirinfo;
	return true;
}

bool
CDirectory::AddItem(const DirectoryInfo &dirinfo, int32 imgId)
{
	if(imgId < 0 || imgId >= MAX_CDIMAGES || dirinfo.offset >= 0x1000000)
		return false;
	DirectoryInfo di = dirinfo;
	di.offset |= (uint32)imgId<<24;
	return AddItem(di);
}

bool
CDirectory::FindItem(const char *name, uint32 &offset, uint32 &size)
{
	int i;

	for(i = 0; i < numEntries; i++)
		if(!CGeneral::faststricmp(entries[i].name, name)){
			offset = entries[i].offset;
			size = entries[i].size;
			return true;
		}
	return false;
}
