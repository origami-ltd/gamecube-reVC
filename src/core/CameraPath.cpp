#include "common.h"

#include <ctype.h>
#include <stdlib.h>

#include "CameraPath.h"
#include "FileMgr.h"

static bool
ParseCameraPathValue(char *token, float &value)
{
	char *end;
	value = strtof(token, &end);
	if(end == token || !isfinite(value))
		return false;
	if(*end == 'f' || *end == 'F')
		end++;
	while(*end && isspace((unsigned char)*end))
		end++;
	return *end == '\0';
}

static bool
IsCameraPathTokenEmpty(const char *token)
{
	while(*token && isspace((unsigned char)*token))
		token++;
	return *token == '\0';
}

static bool
ValidateCameraPathSpline(float *spline, uint32 numValues, uint32 capacity, uint32 stride)
{
	float countValue = spline[0];
	if(countValue < 2.0f || countValue > (capacity-1)/stride)
		return false;
	uint32 declaredCount = (uint32)countValue;
	if(countValue != declaredCount || (numValues-1)%stride != 0)
		return false;
	uint32 recordCount = (numValues-1)/stride;
	if(recordCount != declaredCount && recordCount != declaredCount+1)
		return false;
	for(uint32 i = 0; i < recordCount; i++){
		float time = spline[1 + i*stride];
		if(time < 0.0f || (double)time*1000.0 > UINT32_MAX ||
		   (i > 0 && time < spline[1 + (i-1)*stride]))
			return false;
	}
	float declaredFinalDelta = spline[1 + (declaredCount-1)*stride] -
	                           spline[1 + (declaredCount-2)*stride];
	if((uint32)(declaredFinalDelta*1000.0f) == 0){
		if(recordCount != declaredCount+1)
			return false;
		float extraDelta = spline[1 + declaredCount*stride] -
		                   spline[1 + (declaredCount-1)*stride];
		if((uint32)(extraDelta*1000.0f) == 0)
			return false;
		spline[0] = (float)recordCount;
	}
	return true;
}

bool
DecodeCameraPathSplines(int file, uint32 length, float *pathData[4],
                        uint32 capacity, uint32 valueCounts[4])
{
	enum { NUM_PATH_SPLINES = 4 };
	bool reading = true;
	char c, token[32] = { 0 };
	uint32 consumed = 0;
	uint32 i, j, n;

	if(pathData == nil || capacity == 0 || valueCounts == nil)
		return false;
	for(i = 0; i < NUM_PATH_SPLINES; i++){
		valueCounts[i] = 0;
		if(pathData[i] == nil)
			return false;
	}

	i = 0;
	j = 0;
	n = 0;
	while(reading && consumed < length){
		if(CFileMgr::Read(file, &c, 1) != 1)
			goto fail;
		consumed++;
		switch(c){
		case '\0':
			reading = false;
			break;

		case '+': case '-': case '.':
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
		case 'e': case 'E': case 'f': case 'F':
		case ' ': case '\t': case '\r': case '\n':
			if(n >= sizeof(token)-1)
				goto fail;
			token[n++] = c;
			break;

		case ',':
			if(i >= NUM_PATH_SPLINES || j >= capacity ||
			   !ParseCameraPathValue(token, pathData[i][j]))
				goto fail;
			j++;
			memset(token, 0, sizeof(token));
			n = 0;
			break;

		case ';':
			if(i >= NUM_PATH_SPLINES)
				goto fail;
			if(!IsCameraPathTokenEmpty(token)){
				if(j >= capacity || !ParseCameraPathValue(token, pathData[i][j]))
					goto fail;
				j++;
			}else if(j == 0){
				goto fail;
			}
			if(!ValidateCameraPathSpline(pathData[i], j, capacity, i < 2 ? 4 : 10))
				goto fail;
			valueCounts[i] = j;
			i++;
			j = 0;
			if(i == NUM_PATH_SPLINES)
				reading = false;
			memset(token, 0, sizeof(token));
			n = 0;
			break;

		default:
			goto fail;
		}
	}
	if(reading || i != NUM_PATH_SPLINES)
		goto fail;
	return true;

fail:
	for(i = 0; i < NUM_PATH_SPLINES; i++)
		valueCounts[i] = 0;
	return false;
}
