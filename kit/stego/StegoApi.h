/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#define STEGO_EXPORT __attribute__((visibility("default")))

typedef struct StegoHandle StegoHandle;

/// Creates the noise stamp for the given pair of UUIDs. Returns NULL on failure.
STEGO_EXPORT StegoHandle* stegoCreate(const char* uuid1, const char* uuid2, int amplitude);

/// Accepts NULL.
STEGO_EXPORT void stegoDestroy(StegoHandle* handle);

/// Darkens the pixels of the given tile in place. Accepts a NULL handle.
STEGO_EXPORT void stegoApply(StegoHandle* handle, unsigned char* canvas, int canvasWidth,
                             int canvasHeight, int offsetX, int offsetY, int width, int height);

/// Extracts the UUIDs from an RGBA screenshot. uuid1 and uuid2 must have room for 37 bytes.
/// Returns 0 on success.
STEGO_EXPORT int stegoDecode(const unsigned char* rgba, int width, int height, char* uuid1,
                             char* uuid2, double* markerPeakScore);

#ifdef __cplusplus
}
#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */