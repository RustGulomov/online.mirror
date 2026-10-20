/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#pragma once

#include <stego/StegoApi.h>

#include <string>

class StegoNoise final
{
public:
    explicit StegoNoise(const std::string& uuid1, const std::string& uuid2, int amplitude = 3)
        : _handle(stegoCreate(uuid1.c_str(), uuid2.c_str(), amplitude))
    {
    }

    ~StegoNoise() { stegoDestroy(_handle); }

    StegoNoise(const StegoNoise&) = delete;
    StegoNoise& operator=(const StegoNoise&) = delete;

    void apply(unsigned char* canvas, int canvasWidth, int canvasHeight, int offsetX, int offsetY,
               int width, int height)
    {
        stegoApply(_handle, canvas, canvasWidth, canvasHeight, offsetX, offsetY, width, height);
    }

private:
    StegoHandle* _handle;
};

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */