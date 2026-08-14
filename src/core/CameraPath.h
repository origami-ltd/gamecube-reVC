#ifndef __GTA_CAMERAPATH_H__
#define __GTA_CAMERAPATH_H__

#include <stdint.h>

bool DecodeCameraPathSplines(int file, uint32_t length, float *pathData[4],
                            uint32_t capacity, uint32_t valueCounts[4]);

#endif // __GTA_CAMERAPATH_H__
