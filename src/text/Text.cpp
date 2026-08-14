#include "common.h"

#include "FileMgr.h"
#ifdef MORE_LANGUAGES
#include "Game.h"
#endif
#include "Frontend.h"
#include "Messages.h"
#include "Text.h"
#include "Timer.h"

#include <limits.h>
#include <new>

wchar WideErrorString[25];

CText TheText;

enum
{
	GXT_KEY_ENTRY_SIZE = 12,
	GXT_TABLE_ENTRY_SIZE = 12,
	GXT_NAME_SIZE = 8
};

struct GxtSection
{
	const uint8 *keys;
	size_t keySize;
	const uint8 *text;
	size_t textSize;
};

static bool
HasTerminator(const char *text, size_t size)
{
	return memchr(text, '\0', size) != nil;
}

static bool
ValidateGxtKeyData(const GxtSection &section)
{
	if(section.keySize % GXT_KEY_ENTRY_SIZE != 0 || section.textSize % sizeof(wchar) != 0)
		return false;
	size_t keyCount = section.keySize / GXT_KEY_ENTRY_SIZE;
	if(keyCount > INT16_MAX || section.textSize / sizeof(wchar) > INT_MAX)
		return false;

	size_t lastTerminator = SIZE_MAX;
	for(size_t offset = 0; offset < section.textSize; offset += sizeof(wchar))
		if(ReadLE16(section.text + offset) == 0)
			lastTerminator = offset;
	if(keyCount != 0 && lastTerminator == SIZE_MAX)
		return false;

	const char *previousKey = nil;
	for(size_t i = 0; i < keyCount; i++){
		const uint8 *entry = section.keys + i * GXT_KEY_ENTRY_SIZE;
		uint32 valueOffset = ReadLE32(entry);
		const char *key = (const char*)entry + sizeof(uint32);
		if(!HasTerminator(key, GXT_NAME_SIZE) || (valueOffset & 1) != 0 ||
		   valueOffset > lastTerminator || valueOffset + sizeof(wchar) > section.textSize)
			return false;
		if(previousKey != nil && strcmp(previousKey, key) >= 0)
			return false;
		previousKey = key;
	}
	return true;
}

static bool
ReadGxtSection(const uint8 *fileData, size_t start, size_t end,
	const char *expectedName, GxtSection &section)
{
	if(start > end)
		return false;
	size_t cursor = start;
	if(expectedName != nil){
		if(end - cursor < GXT_NAME_SIZE ||
		   memcmp(fileData + cursor, expectedName, GXT_NAME_SIZE) != 0)
			return false;
		cursor += GXT_NAME_SIZE;
	}

	memset(&section, 0, sizeof(section));
	bool hasKeys = false;
	bool hasText = false;
	while(cursor < end){
		size_t remaining = end - cursor;
		if(remaining < 8){
			if(remaining != 2 || (cursor & 3) != 2 || (end & 3) != 0)
				return false;
			while(cursor < end)
				if(fileData[cursor++] != 0)
					return false;
			break;
		}

		const uint8 *header = fileData + cursor;
		uint32 chunkSize = ReadLE32(header + 4);
		cursor += 8;
		if(chunkSize > end - cursor)
			return false;
		if(memcmp(header, "TKEY", 4) == 0){
			if(hasKeys)
				return false;
			hasKeys = true;
			section.keys = fileData + cursor;
			section.keySize = chunkSize;
		}else if(memcmp(header, "TDAT", 4) == 0){
			if(hasText)
				return false;
			hasText = true;
			section.text = fileData + cursor;
			section.textSize = chunkSize;
		}else{
			return false;
		}
		cursor += chunkSize;
	}
	return hasKeys && hasText && ValidateGxtKeyData(section);
}

static bool
ValidateGxtFile(const uint8 *fileData, size_t fileSize,
	CMissionTextOffsets &missionOffsets, GxtSection &mainSection)
{
	if(fileData == nil || fileSize < 8 || memcmp(fileData, "TABL", 4) != 0)
		return false;
	uint32 tableSize = ReadLE32(fileData + 4);
	if(tableSize % GXT_TABLE_ENTRY_SIZE != 0 || tableSize > fileSize - 8)
		return false;
	size_t entryCount = tableSize / GXT_TABLE_ENTRY_SIZE;
	if(entryCount == 0 || entryCount - 1 > CMissionTextOffsets::MAX_MISSION_TEXTS)
		return false;

	CMissionTextOffsets stagedOffsets;
	stagedOffsets.size = (uint16)entryCount;
	for(size_t i = 0; i < entryCount; i++){
		const uint8 *entry = fileData + 8 + i * GXT_TABLE_ENTRY_SIZE;
		memcpy(stagedOffsets.data[i].szMissionName, entry, GXT_NAME_SIZE);
		stagedOffsets.data[i].offset = ReadLE32(entry + GXT_NAME_SIZE);
		if(!HasTerminator(stagedOffsets.data[i].szMissionName, GXT_NAME_SIZE))
			return false;
		if(i == 0){
			if(stagedOffsets.data[i].offset != 8 + tableSize)
				return false;
		}else{
			if((i > 1 && strcmp(stagedOffsets.data[i - 1].szMissionName,
			                    stagedOffsets.data[i].szMissionName) >= 0) ||
			   stagedOffsets.data[i].offset <= stagedOffsets.data[i - 1].offset)
				return false;
		}
		if(stagedOffsets.data[i].offset >= fileSize)
			return false;
	}

	for(size_t i = 0; i < entryCount; i++){
		size_t start = stagedOffsets.data[i].offset;
		size_t end = i + 1 < entryCount ? stagedOffsets.data[i + 1].offset : fileSize;
		GxtSection section;
		const char *name = i == 0 ? nil : stagedOffsets.data[i].szMissionName;
		if(!ReadGxtSection(fileData, start, end, name, section))
			return false;
		if(i == 0)
			mainSection = section;
	}

	missionOffsets = stagedOffsets;
	return true;
}

static bool
DecodeGxtSection(const GxtSection &section, CKeyArray &keys, CData &text)
{
	CKeyArray stagedKeys;
	CData stagedText;
	int keyCount = (int)(section.keySize / GXT_KEY_ENTRY_SIZE);
	int charCount = (int)(section.textSize / sizeof(wchar));

	if(keyCount != 0){
		stagedKeys.entries = new(std::nothrow) CKeyEntry[keyCount];
		if(stagedKeys.entries == nil)
			return false;
	}
	stagedKeys.numEntries = keyCount;
	for(int i = 0; i < keyCount; i++){
		const uint8 *entry = section.keys + i * GXT_KEY_ENTRY_SIZE;
#if defined(FIX_BUGS) || defined(FIX_BUGS_64)
		stagedKeys.entries[i].valueOffset = ReadLE32(entry);
#else
		stagedKeys.entries[i].value = (wchar*)(uintptr)ReadLE32(entry);
#endif
		memcpy(stagedKeys.entries[i].key, entry + sizeof(uint32), GXT_NAME_SIZE);
	}

	if(charCount != 0){
		stagedText.chars = new(std::nothrow) wchar[charCount];
		if(stagedText.chars == nil)
			return false;
	}
	stagedText.numChars = charCount;
	for(int i = 0; i < charCount; i++)
		stagedText.chars[i] = ReadLE16(section.text + i * sizeof(wchar));
	stagedKeys.Update(stagedText.chars);

	keys.Swap(stagedKeys);
	text.Swap(stagedText);
	return true;
}

bool
DecodeGxtFile(const uint8 *fileData, size_t fileSize, CKeyArray &keys, CData &text,
	CMissionTextOffsets &missionOffsets)
{
	CMissionTextOffsets stagedOffsets;
	GxtSection mainSection;
	if(!ValidateGxtFile(fileData, fileSize, stagedOffsets, mainSection))
		return false;

	CKeyArray stagedKeys;
	CData stagedText;
	if(!DecodeGxtSection(mainSection, stagedKeys, stagedText))
		return false;
	keys.Swap(stagedKeys);
	text.Swap(stagedText);
	missionOffsets = stagedOffsets;
	return true;
}

static bool
SameMissionOffsets(const CMissionTextOffsets &left, const CMissionTextOffsets &right)
{
	if(left.size > CMissionTextOffsets::MAX_MISSION_TEXTS + 1 || left.size != right.size)
		return false;
	for(uint16 i = 0; i < left.size; i++)
		if(left.data[i].offset != right.data[i].offset ||
		   memcmp(left.data[i].szMissionName, right.data[i].szMissionName, GXT_NAME_SIZE) != 0)
			return false;
	return true;
}

bool
DecodeGxtMission(const uint8 *fileData, size_t fileSize,
	const CMissionTextOffsets &expectedOffsets, const char missionName[8],
	CKeyArray &keys, CData &text, char loadedName[8])
{
	CMissionTextOffsets offsets;
	GxtSection mainSection;
	if(missionName == nil || loadedName == nil ||
	   !ValidateGxtFile(fileData, fileSize, offsets, mainSection) ||
	   !SameMissionOffsets(offsets, expectedOffsets))
		return false;

	for(uint16 i = 1; i < offsets.size; i++){
		if(memcmp(offsets.data[i].szMissionName, missionName, GXT_NAME_SIZE) != 0)
			continue;
		size_t end = i + 1 < offsets.size ? offsets.data[i + 1].offset : fileSize;
		GxtSection section;
		if(!ReadGxtSection(fileData, offsets.data[i].offset, end,
		                   offsets.data[i].szMissionName, section))
			return false;
		CKeyArray stagedKeys;
		CData stagedText;
		if(!DecodeGxtSection(section, stagedKeys, stagedText))
			return false;
		keys.Swap(stagedKeys);
		text.Swap(stagedText);
		memcpy(loadedName, offsets.data[i].szMissionName, GXT_NAME_SIZE);
		return true;
	}
	return false;
}

static bool
GetTextFilename(char filename[32])
{
	const char *name = nil;
	switch(FrontEndMenuManager.m_PrefsLanguage){
	case CMenuManager::LANGUAGE_AMERICAN:
#if defined(GTA_PS2) && defined(GTA_PAL)
		name = "ENGLISH.GXT";
#else
		name = "AMERICAN.GXT";
#endif
		break;
	case CMenuManager::LANGUAGE_FRENCH: name = "FRENCH.GXT"; break;
	case CMenuManager::LANGUAGE_GERMAN: name = "GERMAN.GXT"; break;
	case CMenuManager::LANGUAGE_ITALIAN: name = "ITALIAN.GXT"; break;
	case CMenuManager::LANGUAGE_SPANISH: name = "SPANISH.GXT"; break;
#ifdef MORE_LANGUAGES
	case CMenuManager::LANGUAGE_POLISH: name = "POLISH.GXT"; break;
	case CMenuManager::LANGUAGE_RUSSIAN: name = "RUSSIAN.GXT"; break;
	case CMenuManager::LANGUAGE_JAPANESE: name = "JAPANESE.GXT"; break;
	case CMenuManager::LANGUAGE_PORTUGUESE: name = "PORTUGUESE.GXT"; break;
#endif
	default: return false;
	}
	strcpy(filename, name);
	return true;
}

static bool
ReadTextFile(const char *filename, uint8 *&fileData, size_t &fileSize)
{
	fileData = nil;
	fileSize = 0;
	CFileMgr::SetDir("TEXT");
	int file = CFileMgr::OpenFile(filename, "rb");
	bool success = file > 0 && CFileMgr::GetFileSize(file, &fileSize) &&
	               fileSize != 0 && fileSize <= INT_MAX;
	if(success){
		fileData = new(std::nothrow) uint8[fileSize];
		success = fileData != nil && CFileMgr::Read(file, (char*)fileData, fileSize) == fileSize;
	}
	if(file > 0 && CFileMgr::CloseFile(file) != 0)
		success = false;
	CFileMgr::SetDir("");
	if(!success){
		delete[] fileData;
		fileData = nil;
		fileSize = 0;
	}
	return success;
}

CText::CText(void)
{
	encoding = 'e';
	loadedLanguage = -1;
	bHasMissionTextOffsets = false;
	bIsMissionTextLoaded = false;
	memset(szMissionTableName, 0, sizeof(szMissionTableName));
	memset(WideErrorString, 0, sizeof(WideErrorString));
}

bool
CText::HandleLoadFailure(void)
{
	if(loadedLanguage >= 0)
		FrontEndMenuManager.m_PrefsLanguage = loadedLanguage;
	return false;
}

bool
CText::Load(void)
{
	int32 requestedLanguage = FrontEndMenuManager.m_PrefsLanguage;
	char filename[32];
	if(!GetTextFilename(filename))
		return HandleLoadFailure();
	uint8 *fileData;
	size_t fileSize;
	if(!ReadTextFile(filename, fileData, fileSize))
		return HandleLoadFailure();

	CKeyArray stagedKeys;
	CData stagedText;
	CMissionTextOffsets stagedOffsets;
	bool success = DecodeGxtFile(fileData, fileSize, stagedKeys, stagedText, stagedOffsets);
	delete[] fileData;
	if(!success)
		return HandleLoadFailure();

	CMessages::ClearAllMessagesDisplayedByGame();
	keyArray.Swap(stagedKeys);
	data.Swap(stagedText);
	mission_keyArray.Unload();
	mission_data.Unload();
	MissionTextOffsets = stagedOffsets;
	bHasMissionTextOffsets = true;
	bIsMissionTextLoaded = false;
	loadedLanguage = requestedLanguage;
	memset(szMissionTableName, 0, sizeof(szMissionTableName));
	return true;
}

void
CText::Unload(void)
{
	CMessages::ClearAllMessagesDisplayedByGame();
	keyArray.Unload();
	data.Unload();
	mission_keyArray.Unload();
	mission_data.Unload();
	loadedLanguage = -1;
	bHasMissionTextOffsets = false;
	bIsMissionTextLoaded = false;
	MissionTextOffsets.size = 0;
	memset(szMissionTableName, 0, sizeof(szMissionTableName));
}

wchar*
CText::Get(const char *key)
{
	uint8 result = false;
#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
	wchar *outstr = keyArray.Search(key, data.chars, &result);
#else
	wchar *outstr = keyArray.Search(key, &result);
#endif

	if (!result && bHasMissionTextOffsets && bIsMissionTextLoaded)
#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
		outstr = mission_keyArray.Search(key, mission_data.chars, &result);
#else
		outstr = mission_keyArray.Search(key, &result);
#endif
	return outstr;
}

wchar UpperCaseTable[128] = {
	128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138,
	139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
	150, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137,
	138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148,
	149, 173, 173, 175, 176, 177, 178, 179, 180, 181, 182,
	183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193,
	194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204,
	205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215,
	216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226,
	227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237,
	238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248,
	249, 250, 251, 252, 253, 254, 255
};

wchar FrenchUpperCaseTable[128] = {
	128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138,
	139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
	150, 65, 65, 65, 65, 132, 133, 69, 69, 69, 69, 73, 73,
	73, 73, 79, 79, 79, 79, 85, 85, 85, 85, 173, 173, 175,
	176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186,
	187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197,
	198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208,
	209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219,
	220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230,
	231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241,
	242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252,
	253, 254, 255
};

wchar
CText::GetUpperCase(wchar c)
{
	switch (encoding)
	{
	case 'e':
		if (c >= 'a' && c <= 'z')
			return c - 32;
		break;
	case 'f':
		if (c >= 'a' && c <= 'z')
			return c - 32;

		if (c >= 128 && c <= 255)
			return FrenchUpperCaseTable[c-128];
		break;
	case 'g':
	case 'i':
	case 's':
		if (c >= 'a' && c <= 'z')
			return c - 32;

		if (c >= 128 && c <= 255)
			return UpperCaseTable[c-128];
		break;
	default:
		break;
	}
	return c;
}

void
CText::UpperCase(wchar *s)
{
	while(*s){
		*s = GetUpperCase(*s);
		s++;
	}
}

void
CText::GetNameOfLoadedMissionText(char *outName)
{
	strcpy(outName, szMissionTableName);
}

bool
CText::LoadMissionText(const char *MissionTableName)
{
	if(!bHasMissionTextOffsets || MissionTableName == nil)
		return false;
	char filename[32];
	if(!GetTextFilename(filename))
		return false;

	uint8 *fileData;
	size_t fileSize;
	CTimer::Suspend();
	bool success = ReadTextFile(filename, fileData, fileSize);
	CTimer::Resume();
	if(!success)
		return false;

	CKeyArray stagedKeys;
	CData stagedText;
	char loadedName[8];
	success = DecodeGxtMission(fileData, fileSize, MissionTextOffsets,
	                           MissionTableName, stagedKeys, stagedText, loadedName);
	delete[] fileData;
	if(!success)
		return false;

	CMessages::ClearAllMessagesDisplayedByGame();
	mission_keyArray.Swap(stagedKeys);
	mission_data.Swap(stagedText);
	memcpy(szMissionTableName, loadedName, sizeof(szMissionTableName));
	bIsMissionTextLoaded = true;
	return true;
}

void
CKeyArray::Unload(void)
{
	delete[] entries;
	entries = nil;
	numEntries = 0;
}

void
CKeyArray::Swap(CKeyArray &other)
{
	CKeyEntry *savedEntries = entries;
	int savedCount = numEntries;
	entries = other.entries;
	numEntries = other.numEntries;
	other.entries = savedEntries;
	other.numEntries = savedCount;
}

void
CKeyArray::Update(wchar *chars)
{
#if !defined(FIX_BUGS) && !defined(FIX_BUGS_64)
	int i;
	for(i = 0; i < numEntries; i++)
		entries[i].value = (wchar*)((uint8*)chars + (uintptr)entries[i].value);
#endif
}

CKeyEntry*
CKeyArray::BinarySearch(const char *key, CKeyEntry *entries, int16 low, int16 high)
{
	int mid;
	int diff;

	if(low > high)
		return nil;

	mid = (low + high)/2;
	diff = strcmp(key, entries[mid].key);
	if(diff == 0)
		return &entries[mid];
	if(diff < 0)
		return BinarySearch(key, entries, low, mid-1);
	if(diff > 0)
		return BinarySearch(key, entries, mid+1, high);
	return nil;
}

wchar*
#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
CKeyArray::Search(const char *key, wchar *data, uint8 *result)
#else
CKeyArray::Search(const char *key, uint8 *result)
#endif
{
	CKeyEntry *found;
	char errstr[25];
	int i;

#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
	found = BinarySearch(key, entries, 0, numEntries-1);
	if (found) {
		*result = true;
		return (wchar*)((uint8*)data + found->valueOffset);
	}
#else
	found = BinarySearch(key, entries, 0, numEntries-1);
	if (found) {
		*result = true;
		return found->value;
	}
#endif
	*result = false;
#ifdef MASTER
	sprintf(errstr, "");
#else
	sprintf(errstr, "%s missing", key);
#endif // MASTER
	for(i = 0; i < 25; i++)
		WideErrorString[i] = errstr[i];
	return WideErrorString;
}

void
CData::Unload(void)
{
	delete[] chars;
	chars = nil;
	numChars = 0;
}

void
CData::Swap(CData &other)
{
	wchar *savedChars = chars;
	int savedCount = numChars;
	chars = other.chars;
	numChars = other.numChars;
	other.chars = savedChars;
	other.numChars = savedCount;
}

char*
UnicodeToAscii(wchar *src)
{
	static char aStr[256];
	int len;
	for(len = 0; *src != '\0' && len < 256-1; len++, src++)
#ifdef MORE_LANGUAGES
		if(*src < 128 || ((CGame::russianGame || CGame::japaneseGame) && *src < 256))
#else
		if(*src < 128)
#endif
			aStr[len] = *src;
		// convert to CP1252
		else if(*src <= 131)
			aStr[len] = *src + 64;
		else if (*src <= 141)
			aStr[len] = *src + 66;
		else if (*src <= 145)
			aStr[len] = *src + 68;
		else if (*src <= 149)
			aStr[len] = *src + 71;
		else if (*src <= 154)
			aStr[len] = *src + 73;
		else if (*src <= 164)
			aStr[len] = *src + 75;
		else if (*src <= 168)
			aStr[len] = *src + 77;
		else if (*src <= 204)
			aStr[len] = *src + 80;
		else switch (*src) {
		case 205: aStr[len] = 209; break;
		case 206: aStr[len] = 241; break;
		case 207: aStr[len] = 191; break;
		default: aStr[len] = '#'; break;
		}
	aStr[len] = '\0';
	return aStr;
}

char*
UnicodeToAsciiForSaveLoad(wchar *src)
{
	static char aStr[256];
	int len;
	for(len = 0; *src != '\0' && len < 256; len++, src++)
		if(*src < 256)
			aStr[len] = *src;
		else
			aStr[len] = '#';
	aStr[len] = '\0';
	return aStr;
}

char*
UnicodeToAsciiForMemoryCard(wchar *src)
{
	static char aStr[256];
	int len;
	for(len = 0; *src != '\0' && len < 256; len++, src++)
		if(*src < 256)
			aStr[len] = *src;
		else
			aStr[len] = '#';
	aStr[len] = '\0';
	return aStr;
}

void
TextCopy(wchar *dst, const wchar *src)
{
	while((*dst++ = *src++) != '\0');
}
