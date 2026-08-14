#include "common.h"

#include "FileMgr.h"
#include "Script.h"

#include <errno.h>

#ifdef USE_DEBUG_SCRIPT_LOADER
int CTheScripts::ScriptToLoad = 0;
#endif

static const char *
GetScriptFilename(int selection)
{
#ifdef USE_DEBUG_SCRIPT_LOADER
	switch(selection){
	case 1: return "data\\freeroam_miami.scm";
	case 2: return "data\\main_d.scm";
	}
#else
	(void)selection;
#endif
	return "data\\main.scm";
}

static int
OpenScriptFile(int selection)
{
	CFileMgr::ChangeDir("\\");
	errno = 0;
	return CFileMgr::OpenFile(GetScriptFilename(selection), "rb");
}

int
CTheScripts::OpenScript()
{
#ifdef USE_DEBUG_SCRIPT_LOADER
	return OpenScriptFile(ScriptToLoad);
#else
	return OpenScriptFile(0);
#endif
}

bool
CTheScripts::LoadScriptFile(uint8 *data, uint32 &fileSize, int &selection)
{
	if(data == nil)
		return false;
	int selected = selection;
	int handle = OpenScriptFile(selected);
#ifdef USE_DEBUG_SCRIPT_LOADER
	if(handle <= 0 && selected == 2 && errno == ENOENT){
		selected = 0;
		handle = OpenScriptFile(selected);
	}
#endif
	size_t size = 0;
	bool loaded = handle > 0 && CFileMgr::GetFileSize(handle, &size) &&
	              size >= SIZE_MAIN_SCRIPT && size <= INT32_MAX &&
	              CFileMgr::Read(handle, (char*)data, SIZE_MAIN_SCRIPT) == SIZE_MAIN_SCRIPT;
	if(handle > 0 && CFileMgr::CloseFile(handle) != 0)
		loaded = false;
	CFileMgr::SetDir("");
	if(!loaded || !ValidateScriptFile(data, SIZE_MAIN_SCRIPT, (uint32)size))
		return false;
	fileSize = (uint32)size;
	selection = selected;
	return true;
}

static bool
IsScriptRangeValid(uint32 offset, uint32 length, uint32 size)
{
	return offset <= size && length <= size - offset;
}

bool
CTheScripts::ValidateScriptFile(const uint8 *data, uint32 dataSize, uint32 fileSize)
{
	if(data == nil || dataSize != SIZE_MAIN_SCRIPT || fileSize < dataSize || fileSize > INT32_MAX ||
	   !IsScriptRangeValid(3, sizeof(uint32), dataSize))
		return false;

	uint32 variableSpaceSize = ReadLE32(data + 3);
	if(variableSpaceSize < 8 || (variableSpaceSize - 8) % sizeof(int32) != 0 ||
	   !IsScriptRangeValid(variableSpaceSize, 12, dataSize))
		return false;

	uint16 objectCount = ReadLE16(data + variableSpaceSize + 8);
	if(objectCount > MAX_NUM_USED_OBJECTS)
		return false;
	uint32 objectNamesSize = (uint32)objectCount * USED_OBJECT_NAME_LENGTH;
	if(objectNamesSize > UINT32_MAX - variableSpaceSize - 12)
		return false;
	uint32 expectedObjectEnd = variableSpaceSize + 12 + objectNamesSize;
	uint32 objectEnd = ReadLE32(data + variableSpaceSize + 3);
	if(objectEnd != expectedObjectEnd || !IsScriptRangeValid(objectEnd, 20, dataSize))
		return false;
	for(uint16 i = 0; i < objectCount; i++)
		if(memchr(data + variableSpaceSize + 12 + i * USED_OBJECT_NAME_LENGTH,
		          '\0', USED_OBJECT_NAME_LENGTH) == nil)
			return false;

	uint32 mainScriptSize = ReadLE32(data + objectEnd + 8);
	uint32 largestMissionSize = ReadLE32(data + objectEnd + 12);
	uint16 missionCount = ReadLE16(data + objectEnd + 16);
	uint16 exclusiveMissionCount = ReadLE16(data + objectEnd + 18);
	if(missionCount > MAX_NUM_MISSION_SCRIPTS || exclusiveMissionCount > missionCount)
		return false;
	uint32 missionTableSize = (uint32)missionCount * sizeof(uint32);
	uint32 missionTableOffset = objectEnd + 20;
	if(!IsScriptRangeValid(missionTableOffset, missionTableSize, dataSize))
		return false;
	uint32 missionTableEnd = missionTableOffset + missionTableSize;
	if(mainScriptSize < missionTableEnd || mainScriptSize > dataSize ||
	   mainScriptSize > fileSize || mainScriptSize < 2 ||
	   largestMissionSize > SIZE_MISSION_SCRIPT)
		return false;
	if(missionCount == 0)
		return largestMissionSize == 0;

	uint32 largestSpan = 0;
	for(uint16 i = 0; i < missionCount; i++){
		uint32 start = ReadLE32(data + missionTableOffset + i * sizeof(uint32));
		uint32 end = i + 1 < missionCount ?
			ReadLE32(data + missionTableOffset + (i + 1) * sizeof(uint32)) : fileSize;
		if((i == 0 && start != mainScriptSize) || start < mainScriptSize ||
		   start >= end || end > fileSize || end - start > SIZE_MISSION_SCRIPT ||
		   end - start < 2)
			return false;
		if(end - start > largestSpan)
			largestSpan = end - start;
	}
	return largestMissionSize == largestSpan;
}

void
CTheScripts::NormalizeScriptGlobals(uint8 *data)
{
	uint32 variableSpaceSize = ReadLE32(data + 3);
	for(uint32 offset = 8; offset < variableSpaceSize; offset += sizeof(uint32)){
		uint32 value = ReadLE32(data + offset);
		memcpy(data + offset, &value, sizeof(value));
	}
}

bool
CTheScripts::InstallScriptFile(const uint8 *data, uint32 fileSize)
{
	if(!ValidateScriptFile(data, SIZE_MAIN_SCRIPT, fileSize))
		return false;
	memset(ScriptSpace, 0, sizeof(ScriptSpace));
	memcpy(ScriptSpace, data, SIZE_MAIN_SCRIPT);
	NormalizeScriptGlobals(ScriptSpace);
	ScriptFileSize = fileSize;
	CurrentMissionScriptSize = 0;
	memset(MultiScriptArray, 0, sizeof(MultiScriptArray));
	NumberOfExclusiveMissionScripts = 0;
	NumberOfMissionScripts = 0;
	LargestMissionScriptSize = 0;
	MainScriptSize = 0;
	ReadMultiScriptFileOffsetsFromScript();
	return true;
}

void
CTheScripts::ReadMultiScriptFileOffsetsFromScript()
{
	int32 varSpace = GetSizeOfVariableSpace();
	uint32 ip = varSpace + 3;
	int32 objectSize = Read4BytesFromScript(&ip);
	ip = objectSize + 8;
	MainScriptSize = Read4BytesFromScript(&ip);
	LargestMissionScriptSize = Read4BytesFromScript(&ip);
	NumberOfMissionScripts = Read2BytesFromScript(&ip);
	NumberOfExclusiveMissionScripts = Read2BytesFromScript(&ip);
	for(int i = 0; i < NumberOfMissionScripts; i++)
		MultiScriptArray[i] = Read4BytesFromScript(&ip);
}

bool
CTheScripts::LoadMissionScript(uint32 mission)
{
	if(mission >= NumberOfMissionScripts || ScriptFileSize > INT32_MAX ||
	   MainScriptSize > ScriptFileSize)
		return false;
	int32 startValue = MultiScriptArray[mission];
	int32 endValue = mission + 1 < NumberOfMissionScripts ?
		MultiScriptArray[mission + 1] : (int32)ScriptFileSize;
	if(startValue < 0 || endValue < 0 || (uint32)startValue < MainScriptSize ||
	   endValue <= startValue || (uint32)endValue > ScriptFileSize ||
	   (uint32)(endValue - startValue) < sizeof(uint16) ||
	   (uint32)(endValue - startValue) > SIZE_MISSION_SCRIPT)
		return false;
	uint32 missionSize = (uint32)(endValue - startValue);
	uint8 *missionData = (uint8*)RwMalloc(missionSize);
	if(missionData == nil)
		return false;

	int handle = OpenScript();
	size_t fileSize = 0;
	bool loaded = handle > 0 && CFileMgr::GetFileSize(handle, &fileSize) &&
	              fileSize == ScriptFileSize && CFileMgr::Seek(handle, startValue, SEEK_SET) &&
	              CFileMgr::Read(handle, (char*)missionData, missionSize) == missionSize;
	if(handle > 0 && CFileMgr::CloseFile(handle) != 0)
		loaded = false;
	CFileMgr::SetDir("");
	if(loaded){
		memset(&ScriptSpace[SIZE_MAIN_SCRIPT], 0, SIZE_MISSION_SCRIPT);
		memcpy(&ScriptSpace[SIZE_MAIN_SCRIPT], missionData, missionSize);
		CurrentMissionScriptSize = missionSize;
	}
	RwFree(missionData);
	return loaded;
}
