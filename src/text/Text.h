#ifndef __GTA_TEXT_H__
#define __GTA_TEXT_H__

char *UnicodeToAscii(wchar *src);
char *UnicodeToAsciiForSaveLoad(wchar *src);
char *UnicodeToAsciiForMemoryCard(wchar *src);
void TextCopy(wchar *dst, const wchar *src);

struct CKeyEntry
{
#if defined(FIX_BUGS) || defined(FIX_BUGS_64)
	uint32 valueOffset;
#else
	wchar *value;
#endif
	char key[8];
};

// If this fails, CKeyArray::Load will have to be fixed
VALIDATE_SIZE(CKeyEntry, 12);

class CKeyArray
{
public:
	CKeyEntry *entries;
	int numEntries;	// You can make this size_t if you want to exceed 32-bit boundaries, everything else should be ready.

	CKeyArray(void) : entries(nil), numEntries(0) {}
	~CKeyArray(void) { Unload(); }
	void Unload(void);
	void Swap(CKeyArray &other);
	void Update(wchar *chars);
	CKeyEntry *BinarySearch(const char *key, CKeyEntry *entries, int16 low, int16 high);
#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
	wchar *Search(const char *key, wchar *data, uint8 *result);
#else
	wchar *Search(const char *key, uint8* result);
#endif
};

class CData
{
public:
	wchar *chars;
	int numChars; // You can make this size_t if you want to exceed 32-bit boundaries, everything else should be ready.

	CData(void) : chars(nil), numChars(0) {}
	~CData(void) { Unload(); }
	void Unload(void);
	void Swap(CData &other);
};

class CMissionTextOffsets
{
public:
	struct Entry
	{
		char szMissionName[8];
		uint32 offset;
	};

	enum {MAX_MISSION_TEXTS = 90}; // beware that LCS has more

	Entry data[MAX_MISSION_TEXTS + 1];
	uint16 size; // You can make this size_t if you want to exceed 32-bit boundaries, everything else should be ready.

	CMissionTextOffsets(void) : size(0) {}
};

bool DecodeGxtFile(const uint8 *fileData, size_t fileSize, CKeyArray &keys, CData &text,
	CMissionTextOffsets &missionOffsets);
bool DecodeGxtMission(const uint8 *fileData, size_t fileSize,
	const CMissionTextOffsets &expectedOffsets, const char missionName[8],
	CKeyArray &keys, CData &text, char loadedName[8]);

class CText
{
	CKeyArray keyArray;
	CData data;
	CKeyArray mission_keyArray;
	CData mission_data;
	char encoding;
	int32 loadedLanguage;
	bool bHasMissionTextOffsets;
	bool bIsMissionTextLoaded;
	char szMissionTableName[8];
	CMissionTextOffsets MissionTextOffsets;
	bool HandleLoadFailure(void);
public:
	CText(void);
	bool Load(void);
	void Unload(void);
	wchar *Get(const char *key);
	wchar GetUpperCase(wchar c);
	void UpperCase(wchar *s);
	void GetNameOfLoadedMissionText(char *outName);
	bool LoadMissionText(const char *MissionTableName);
};

extern CText TheText;

#endif // __GTA_TEXT_H__
