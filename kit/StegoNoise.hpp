/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#pragma once
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace stego
{
constexpr int TileSize = 256;
constexpr int GridSize = TileSize / 2; // 2x2 pixels grid from 256x256 pixels tile is 128x128 cells
constexpr int CellCount = GridSize * GridSize;
constexpr int MarkerCount = 4096; // Marker cells
constexpr int MessageSize = 48;
constexpr int BitCount = MessageSize * 8;
constexpr uint64_t Seed = 0x1234ABCD5678EF01ULL;

inline uint64_t next(uint64_t& seed)
{
    uint64_t z = (seed += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct Layout
{
    std::array<int16_t, CellCount> ownerBit; // -1 = marker cell, or 0..BitCount
    std::bitset<CellCount> pattern;
};

inline const Layout& layout()
{
    static const Layout L = []
    {
        Layout l;
        uint64_t seed = Seed;

        std::array<int, CellCount> cellsOrder{};
        std::iota(cellsOrder.begin(), cellsOrder.end(), 0);

        for (int i = CellCount - 1; i > 0; --i)
        {
            std::swap(cellsOrder[i], cellsOrder[next(seed) % (i + 1)]);
        }
        for (int i = 0; i < CellCount; ++i)
        {
            const int cellIdx = cellsOrder[i];
            l.ownerBit[cellIdx] = (i < MarkerCount) ? -1 : (i - MarkerCount) % BitCount;
            l.pattern[cellIdx] = (next(seed) & 1) ? 1 : 0;
        }
        return l;
    }();
    return L;
}
}

class StegoNoise final
{
public:
    explicit StegoNoise(const std::array<uint8_t, stego::MessageSize>& payload, int amplitude = 3)
        : _amplitude(amplitude)
    {
        const stego::Layout& L = stego::layout();
        for (int cell = 0; cell < stego::CellCount; ++cell)
        {
            bool cellPattern = L.pattern[cell];
            const int ownerBit = L.ownerBit[cell];
            if (ownerBit >= 0)
            {
                const bool bit = (payload[ownerBit / 8] >> (ownerBit % 8)) & 1;
                cellPattern = bit ? cellPattern : !cellPattern;
            }
            _encodedPattern[cell] = cellPattern;
        }
    }

    void apply(unsigned char* canvas, int canvasWidth, int canvasHeight, int offsetX, int offsetY, int width, int height)
    {
        if (!canvas || width <= 0 || height <= 0)
        {
            return;
        }

        const std::vector<int8_t>& pixelAdjustments = getPixelAdjustments(width, height);

        for (int y = 0; y < height && offsetY + y < canvasHeight; ++y)
        {
            const size_t rowOffset = y * width;

            unsigned char* px = canvas + 4 * ((offsetY + y) * canvasWidth + offsetX);

            for (int x = 0; x < width && offsetX + x < canvasWidth; ++x, px += 4)
            {
                const int8_t adjustment = pixelAdjustments[rowOffset + x];
                if (adjustment == 0 || px[3] == 0)
                {
                    continue;
                }
                px[0] = static_cast<unsigned char>(std::max(px[0] + adjustment, 0));
                px[1] = static_cast<unsigned char>(std::max(px[1] + adjustment, 0));
                px[2] = static_cast<unsigned char>(std::max(px[2] + adjustment, 0));
            }
        }
    }

private:
    const std::vector<int8_t>& getPixelAdjustments(int width, int height)
    {
        const uint64_t key = (static_cast<uint64_t>(width) << 32) | static_cast<uint32_t>(height);
        auto [it, inserted] = _pixelAdjustmentsEachTile.try_emplace(key);
        std::vector<int8_t>& pixelAdjustments = it->second;
        if (!inserted)
        {
            return pixelAdjustments;
        }

        pixelAdjustments.resize(width * height);
        for (int y = 0; y < height; ++y)
        {
            const int cellY = y * stego::GridSize / height;
            for (int x = 0; x < width; ++x)
            {
                const int cellX = x * stego::GridSize / width;
                pixelAdjustments[y * width + x] = _encodedPattern[cellY * stego::GridSize + cellX] ? -1 * _amplitude : 0;
            }
        }
        return pixelAdjustments;
    }

    const int _amplitude;
    std::bitset<stego::CellCount> _encodedPattern;
    std::unordered_map<uint64_t, std::vector<int8_t>> _pixelAdjustmentsEachTile;
};

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */