/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "StegoApi.h"

extern "C"
{
StegoHandle* stegoCreate(const char*, const char*, int)
{
    return nullptr;
}

void stegoDestroy(StegoHandle*) {}

void stegoApply(StegoHandle*, unsigned char*, int, int, int, int, int, int) {}

int stegoDecode(const unsigned char*, int, int, char* uuid1, char* uuid2, double* markerPeakScore)
{
    if (uuid1)
    {
        uuid1[0] = 0;
    }
    if (uuid2)
    {
        uuid2[0] = 0;
    }
    if (markerPeakScore)
    {
        *markerPeakScore = 0;
    }
    return -1;
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */