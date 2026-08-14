#ifndef GAMECUBEPATH_H
#define GAMECUBEPATH_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline bool
IsGameCubePathSeparator(char value)
{
	return value == '/' || value == '\\';
}

static inline bool
IsGameCubeDvdPath(const char *path)
{
	return path != NULL &&
	       (path[0] == 'd' || path[0] == 'D') &&
	       (path[1] == 'v' || path[1] == 'V') &&
	       (path[2] == 'd' || path[2] == 'D') &&
	       path[3] == ':' && IsGameCubePathSeparator(path[4]);
}

static inline char *
NormalizeGameCubePath(const char *path, const char *currentDirectory)
{
	if(path == NULL)
		return NULL;

	bool absolute = IsGameCubeDvdPath(path) || IsGameCubePathSeparator(path[0]);
	if(strchr(path, ':') != NULL && !IsGameCubeDvdPath(path))
		return NULL;
	if(!absolute && !IsGameCubeDvdPath(currentDirectory))
		return NULL;

	const char *directory = absolute ? "" : currentDirectory + 5;
	const char *suffix = IsGameCubeDvdPath(path) ? path + 5 : path;
	size_t directoryLength = strlen(directory);
	size_t suffixLength = strlen(suffix);
	if(directoryLength > SIZE_MAX - suffixLength ||
	   directoryLength + suffixLength > SIZE_MAX - 7)
		return NULL;
	size_t capacity = 7 + directoryLength + suffixLength;
	char *normalized = (char*)malloc(capacity);
	if(normalized == NULL)
		return NULL;

	memcpy(normalized, "dvd:/", 5);
	size_t length = 5;
	const char *parts[] = { directory, suffix };
	for(size_t part = 0; part < 2; part++){
		const char *source = parts[part];
		if(*source != '\0' && length > 5 && normalized[length - 1] != '/')
			normalized[length++] = '/';
		while(*source != '\0'){
			char value = *source++;
			if(IsGameCubePathSeparator(value))
				value = '/';
			if(value != '/' || normalized[length - 1] != '/')
				normalized[length++] = value;
		}
	}
	normalized[length] = '\0';
	return normalized;
}

#endif
