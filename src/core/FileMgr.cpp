#define _CRT_SECURE_NO_WARNINGS
#include <fcntl.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include "common.h"
#include "crossplatform.h"

#include "FileMgr.h"
#include "CdStream.h"

const char *_psGetUserFilesFolder();

/*
 * Windows FILE is BROKEN for GTA.
 *
 * We need to support mapping between LF and CRLF for text files
 * but we do NOT want to end the file at the first sight of a SUB character.
 * So here is a simple implementation of a FILE interface that works like GTA expects.
 */

struct myFILE
{
	bool isText;
	FILE *file;
#ifdef GTA_OGC
	// Write buffering. SaveSettings alone issues about forty tiny writes in a
	// row, and on a FAT volume each one is a read-modify-write of a sector
	// through libfat — the settings file is 200-odd bytes arriving as forty
	// separate sector transactions. Buffer the whole file and commit it in one
	// call on close.
	//
	// Buffering here rather than in SaveSettings fixes every writer at once,
	// including saves, and leaves the call sites alone.
	char *wbuf;
	size_t wlen, wcap;
#endif
};

#define NUMFILES 20
static myFILE myfiles[NUMFILES];


#if !defined(_WIN32)
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#define _getcwd getcwd

// Case-insensitivity on linux (from https://github.com/OneSadCookie/fcaseopen)
void mychdir(char const *path)
{
#if defined(ANDROID)
	if(!path) {
        return;
    }
#endif
	char* r = casepath(path, false);
    if (r) {
#if defined(ANDROID)
		char path[MAX_PATH];
		strcpy(path, CFileMgr::GetRootDirName());
		strcat(path, r);
        chdir(path);
#else
        chdir(r);
#endif
		free(r);
    } else {
        errno = ENOENT;
    }
}
#else
#define mychdir chdir
#endif

/* Force file to open as binary but remember if it was text mode */
// Every game file operation funnels through these five, and libfat is shared
// with the streaming worker, which reads the archive on its own thread. Guard
// here rather than at the callers: it is one place instead of fifty, and a
// caller that forgets is exactly the bug this is for. Closing the pause menu
// writes gta_vc.set through this path while the streamer is busy, which is
// where the game stops.
static int
myfopen(const char *filename, const char *mode)
{
#ifdef GTA_OGC
	DVD_FS_GUARD;
#endif
	int fd;
	char realmode[10], *p;

	for(fd = 1; fd < NUMFILES; fd++)
		if(myfiles[fd].file == nil)
			goto found;
	return 0;	// no free fd
found:
	myfiles[fd].isText = strchr(mode, 'b') == nil;
	p = realmode;
	while(*mode)
		if(*mode != 't' && *mode != 'b')
			*p++ = *mode++;
		else
			mode++;
	*p++ = 'b';
	*p = '\0';
	
	myfiles[fd].file = fcaseopen(filename, realmode);
	if(myfiles[fd].file == nil)
		return 0;
#ifdef GTA_OGC
	// Buffer writers, not readers. A file opened for writing collects its
	// whole contents in memory and commits once on close; reads are untouched
	// because they are already one bulk transfer through CdStream.
	myfiles[fd].wbuf = nil;
	myfiles[fd].wlen = myfiles[fd].wcap = 0;
	if(strchr(realmode, 'w') || strchr(realmode, 'a') || strchr(realmode, '+')){
		myfiles[fd].wcap = 4096;
		myfiles[fd].wbuf = (char*)malloc(myfiles[fd].wcap);
		if(myfiles[fd].wbuf == nil)
			myfiles[fd].wcap = 0;   // fall back to unbuffered
	}
#endif
	return fd;
}

static int
myfclose(int fd)
{
#ifdef GTA_OGC
	DVD_FS_GUARD;
#endif
	int ret;
	assert(fd < NUMFILES);
	if(myfiles[fd].file){
#ifdef GTA_OGC
		// One transaction for the whole file.
		if(myfiles[fd].wbuf){
			if(myfiles[fd].wlen)
				fwrite(myfiles[fd].wbuf, 1, myfiles[fd].wlen, myfiles[fd].file);
			free(myfiles[fd].wbuf);
			myfiles[fd].wbuf = nil;
			myfiles[fd].wlen = myfiles[fd].wcap = 0;
		}
#endif
		ret = fclose(myfiles[fd].file);
		myfiles[fd].file = nil;
		return ret;
	}
	return EOF;
}

static int
myfgetc(int fd)
{
	int c;
	c = fgetc(myfiles[fd].file);
	if(myfiles[fd].isText && c == 015){
		/* translate CRLF to LF */
		c = fgetc(myfiles[fd].file);
		if(c == 012)
			return c;
		ungetc(c, myfiles[fd].file);
		return 015;
	}
	return c;
}

static int
myfputc(int c, int fd)
{
	/* translate LF to CRLF */
	if(myfiles[fd].isText && c == 012)
		fputc(015, myfiles[fd].file);
	return fputc(c, myfiles[fd].file);
}

static char*
myfgets(char *buf, int len, int fd)
{
	int c;
	char *p;

	p = buf;
	len--;	// NUL byte
	while(len--){
		c = myfgetc(fd);
		if(c == EOF){
			if(p == buf)
				return nil;
			break;
		}
		*p++ = c;
		if(c == '\n')
			break;
	}
	*p = '\0';
	return buf;
}

static size_t
myfread(void *buf, size_t elt, size_t n, int fd)
{
#ifdef GTA_OGC
	DVD_FS_GUARD;
#endif
#ifdef GTA_OGC
	if(myfiles[fd].wbuf){
		size_t bytes = elt*n;
		if(myfiles[fd].wlen + bytes > myfiles[fd].wcap){
			size_t cap = myfiles[fd].wcap ? myfiles[fd].wcap*2 : 4096;
			while(cap < myfiles[fd].wlen + bytes)
				cap *= 2;
			char *nb = (char*)realloc(myfiles[fd].wbuf, cap);
			if(nb == nil)
				return 0;      // caller sees a short write, as it would anyway
			myfiles[fd].wbuf = nb;
			myfiles[fd].wcap = cap;
		}
		memcpy(myfiles[fd].wbuf + myfiles[fd].wlen, buf, bytes);
		myfiles[fd].wlen += bytes;
		return n;
	}
#endif
	if(myfiles[fd].isText){
		unsigned char *p;
		size_t i;
		int c;

		n *= elt;
		p = (unsigned char*)buf;
		for(i = 0; i < n; i++){
			c = myfgetc(fd);
			if(c == EOF)
				break;
			*p++ = (unsigned char)c;
		}
		return i / elt;
	}
	return fread(buf, elt, n, myfiles[fd].file);
}

static size_t
myfwrite(void *buf, size_t elt, size_t n, int fd)
{
#ifdef GTA_OGC
	DVD_FS_GUARD;
#endif
	if(myfiles[fd].isText){
		unsigned char *p;
		size_t i;
		int c;

		n *= elt;
		p = (unsigned char*)buf;
		for(i = 0; i < n; i++){
			c = *p++;
			myfputc(c, fd);
			if(feof(myfiles[fd].file))	// is this right?
				break;
		}
		return i / elt;
	}
	return fwrite(buf, elt, n, myfiles[fd].file);
}

static int
myfseek(int fd, long offset, int whence)
{
#ifdef GTA_OGC
	// A seek on a buffered writer would need the buffer to model file
	// position; flush and fall back instead. Nothing on the write paths seeks,
	// so this is a safety valve rather than a case that happens.
	if(myfiles[fd].wbuf){
		if(myfiles[fd].wlen)
			fwrite(myfiles[fd].wbuf, 1, myfiles[fd].wlen, myfiles[fd].file);
		free(myfiles[fd].wbuf);
		myfiles[fd].wbuf = nil;
		myfiles[fd].wlen = myfiles[fd].wcap = 0;
	}
#endif
#ifdef GTA_OGC
	DVD_FS_GUARD;
#endif
	return fseek(myfiles[fd].file, offset, whence);
}

static long
myftell(int fd)
{
	return ftell(myfiles[fd].file);
}

static int
myfeof(int fd)
{
	return feof(myfiles[fd].file);
//	return ferror(myfiles[fd].file);
}


char CFileMgr::ms_rootDirName[128] = {'\0'};
char CFileMgr::ms_dirName[128];

void
CFileMgr::Initialise(void)
{
#if defined(ANDROID)
	if(getenv("STORAGE_ROOT") != NULL) {
		strcpy(ms_rootDirName, getenv("STORAGE_ROOT"));
		strcat(ms_rootDirName, "/");
        debug("Android: Root Dir: %s\n", ms_rootDirName);
	}
#else
    _getcwd(ms_rootDirName, 128);
	strcat(ms_rootDirName, "\\");
#endif
}

void
CFileMgr::ChangeDir(const char *dir)
{
	if(*dir == '\\'){
		strcpy(ms_dirName, ms_rootDirName);
		dir++;
	}
	if(*dir != '\0'){
		strcat(ms_dirName, dir);
#ifndef ANDROID
        // BUG in the game it seems, it's off by one
		if(dir[strlen(dir)-1] != '\\')
			strcat(ms_dirName, "\\");
#endif
	}
	debug("CFileMgr::ChangeDir: %s", ms_dirName);
	mychdir(ms_dirName);
}

void
CFileMgr::SetDir(const char *dir)
{
	strcpy(ms_dirName, ms_rootDirName);
	if(*dir != '\0'){
		strcat(ms_dirName, dir);
#ifndef ANDROID
        // BUG in the game it seems, it's off by one
		if(dir[strlen(dir)-1] != '\\')
			strcat(ms_dirName, "\\");
#endif
	}
	debug("CFileMgr::SetDir: %s", ms_dirName);
	mychdir(ms_dirName);
}

void
CFileMgr::SetDirMyDocuments(void)
{
	SetDir("");	// better start at the root if user directory is relative
	mychdir(_psGetUserFilesFolder());
}

ssize_t
CFileMgr::LoadFile(const char *file, uint8 *buf, int maxlen, const char *mode)
{
	int fd;
	ssize_t n, len;

	fd = myfopen(file, mode);
	if(fd == 0)
		return -1;
	len = 0;
	do{
		n = myfread(buf + len, 1, 0x4000, fd);
#ifndef FIX_BUGS
		if (n < 0)
			return -1;
#endif
		len += n;
		assert(len < maxlen);
	}while(n == 0x4000);
	buf[len] = 0;
	myfclose(fd);
	return len;
}

int
CFileMgr::OpenFile(const char *file, const char *mode)
{
	debug("CFileMgr::OpenFile: %s", file);
	return myfopen(file, mode);
}

int
CFileMgr::OpenFileForWriting(const char *file)
{
	debug("CFileMgr::OpenFileForWriting: %s", file);
	return OpenFile(file, "wb");
}

size_t
CFileMgr::Read(int fd, char *buf, ssize_t len)
{
	return myfread((void*)buf, 1, len, fd);
}

size_t
CFileMgr::Write(int fd, const char *buf, ssize_t len)
{
	return myfwrite((void*)buf, 1, len, fd);
}

bool
CFileMgr::Seek(int fd, int offset, int whence)
{
	return myfseek(fd, offset, whence) == 0;
}

bool
CFileMgr::GetFileSize(int fd, size_t *size)
{
	if(fd <= 0 || fd >= NUMFILES || myfiles[fd].file == nil || size == nil)
		return false;
	long position = myftell(fd);
	if(position < 0)
		return false;
	if(myfseek(fd, 0, SEEK_END) != 0){
		myfseek(fd, position, SEEK_SET);
		return false;
	}
	long end = myftell(fd);
	bool restored = myfseek(fd, position, SEEK_SET) == 0;
	if(!restored || end < 0 || (uint64)end > SIZE_MAX)
		return false;
	*size = (size_t)end;
	return true;
}

bool
CFileMgr::ReadLine(int fd, char *buf, int len)
{
	return myfgets(buf, len, fd) != nil;
}

int
CFileMgr::CloseFile(int fd)
{
	return myfclose(fd);
}

int
CFileMgr::GetErrorReadWrite(int fd)
{
	return myfeof(fd);
}
