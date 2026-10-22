/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 120 -*- */
#include <config.h>

#include <test/lokassert.hpp>

#include <kit/StegoNoise.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include "stego/stb_image.h"

#include <cppunit/extensions/HelperMacros.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace stego;

namespace
{
constexpr int PixelsPerCell = TileSize / GridSize;
constexpr double MinMarkerPeakScore = 15.0;
constexpr int MaxIncorrectBytes = 8;
constexpr const char* UUID1 = "f47ac10b-58cc-4372-a567-0e02b2c3d479";
constexpr const char* UUID2 = "77732632-7084-4992-a0eb-340fc5952e40";

using Payload = std::array<uint8_t, MessageSize>;

Payload makePayloadFromUuids()
{
    return ReedSolomonEncode<32, 32>(concatArrays<16, 16>(uuidToBytes<16>(UUID1), uuidToBytes<16>(UUID2)));
}

std::string bytesToUuid(std::span<const uint8_t, 16> bytes)
{
    constexpr char HexDigits[] = "0123456789abcdef";
    std::string uuid;
    uuid.reserve(36);

    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
        {
            uuid += '-';
        }

        uuid += HexDigits[bytes[i] >> 4];
        uuid += HexDigits[bytes[i] & 0x0F];
    }

    return uuid;
}

std::array<std::string, 2> payloadToUuids(const Payload& payload)
{
    const auto decoded = ReedSolomonDecode<32, 32>(payload);
    if (!decoded)
    {
        return {"<decode-failed>", "<decode-failed>"};
    }

    const auto& message = decoded.value();

    return {
        bytesToUuid(std::span<const uint8_t, 16>(message.data(), 16)),
        bytesToUuid(std::span<const uint8_t, 16>(message.data() + 16, 16))
    };
}

int countIncorrectBytes(const Payload& actualPayload, const Payload& expectedPayload)
{
    int incorrectByteCount = 0;
    for (std::size_t byteIndex = 0; byteIndex < actualPayload.size(); ++byteIndex)
    {
        incorrectByteCount += actualPayload[byteIndex] != expectedPayload[byteIndex] ? 1 : 0;
    }
    return incorrectByteCount;
}

bool isSupportedImageFile(const std::filesystem::path& imagePath)
{
    std::string extension = imagePath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp";
}

std::vector<unsigned char> loadRgbaImage(const std::string& imagePath, int& imageWidth, int& imageHeight)
{
    int sourceChannels = 0;
    unsigned char* imageData = stbi_load(imagePath.c_str(), &imageWidth, &imageHeight, &sourceChannels, 4);
    if (!imageData)
    {
        imageWidth = imageHeight = 0;
        return {};
    }

    std::vector<unsigned char> rgbaImage(imageData, imageData + static_cast<std::size_t>(imageWidth) * imageHeight * 4);
    stbi_image_free(imageData);
    return rgbaImage;
}

struct DecodeResult
{
    Payload payload{};
    double markerPeakScore = 0; // how much the best shift stands out from the rest; below MinMarkerPeakScore = no mark
    float imageScale = 1;
    int tileShiftX = 0;
    int tileShiftY = 0;
    std::array<double, BitCount> bitConfidence{}; // |sum| per bit, for future Reed-Solomon erasures
};

/// Pixel brightness minus local average: removes page content and keeps the fine-grained noise.
std::vector<float> makeHighPassImage(const std::vector<unsigned char>& rgbaImage, int imageWidth, int imageHeight,
                                     int windowRadius)
{
    const int integralImageWidth = imageWidth + 1;
    std::vector<unsigned char> grayPixels(static_cast<std::size_t>(imageWidth) * imageHeight);
    for (std::size_t pixelIndex = 0; pixelIndex < grayPixels.size(); ++pixelIndex)
    {
        grayPixels[pixelIndex] = static_cast<unsigned char>(
            (rgbaImage[4 * pixelIndex] + rgbaImage[4 * pixelIndex + 1] + rgbaImage[4 * pixelIndex + 2]) / 3);
    }

    std::vector<double> integralImage(static_cast<std::size_t>(integralImageWidth) * (imageHeight + 1), 0.0);
    for (int y = 0; y < imageHeight; ++y)
    {
        double rowSum = 0;
        for (int x = 0; x < imageWidth; ++x)
        {
            rowSum += grayPixels[static_cast<std::size_t>(y) * imageWidth + x];
            integralImage[static_cast<std::size_t>(y + 1) * integralImageWidth + x + 1]
                = integralImage[static_cast<std::size_t>(y) * integralImageWidth + x + 1] + rowSum;
        }
    }

    std::vector<float> highPassPixels(static_cast<std::size_t>(imageWidth) * imageHeight);
    for (int y = 0; y < imageHeight; ++y)
    {
        const int top = std::max(y - windowRadius, 0);
        const int bottom = std::min(y + windowRadius, imageHeight - 1) + 1;
        for (int x = 0; x < imageWidth; ++x)
        {
            const int left = std::max(x - windowRadius, 0);
            const int right = std::min(x + windowRadius, imageWidth - 1) + 1;
            const double windowSum = integralImage[static_cast<std::size_t>(bottom) * integralImageWidth + right]
                                     - integralImage[static_cast<std::size_t>(top) * integralImageWidth + right]
                                     - integralImage[static_cast<std::size_t>(bottom) * integralImageWidth + left]
                                     + integralImage[static_cast<std::size_t>(top) * integralImageWidth + left];
            const double localAverage = windowSum / ((right - left) * (bottom - top));
            highPassPixels[static_cast<std::size_t>(y) * imageWidth + x]
                = static_cast<float>(grayPixels[static_cast<std::size_t>(y) * imageWidth + x] - localAverage);
        }
    }
    return highPassPixels;
}

/// Fold the whole image into one TileSize x TileSize tile, assuming the given zoom.
std::vector<float> foldImageIntoTile(const std::vector<float>& highPassPixels, int imageWidth, int imageHeight,
                                     float candidateScale)
{
    std::vector<float> foldedSums(TileSize * TileSize, 0.f);
    std::vector<float> foldedCounts(TileSize * TileSize, 0.f);
    for (int y = 0; y < imageHeight; ++y)
    {
        const int tileY = static_cast<int>(y / candidateScale) & (TileSize - 1);
        for (int x = 0; x < imageWidth; ++x)
        {
            const float highPassValue = highPassPixels[static_cast<std::size_t>(y) * imageWidth + x];
            if (std::fabs(highPassValue) >= 12.f) // letters, window edges
            {
                continue;
            }
            const int tileX = static_cast<int>(x / candidateScale) & (TileSize - 1);
            const int tilePixelIndex = tileY * TileSize + tileX;
            foldedSums[tilePixelIndex] += highPassValue;
            foldedCounts[tilePixelIndex] += 1.f;
        }
    }

    std::vector<float> foldedImage(TileSize * TileSize);
    for (int pixelIndex = 0; pixelIndex < TileSize * TileSize; ++pixelIndex)
    {
        foldedImage[pixelIndex] = foldedCounts[pixelIndex] > 0
                                      ? foldedSums[pixelIndex] / foldedCounts[pixelIndex]
                                      : 0.f;
    }
    return foldedImage;
}

struct Marker
{
    int pixelX;
    int pixelY;
    float expectedSign;
};

/// Try all 256x256 shifts and return how far the best one stands out from the rest.
double findBestTileShift(const std::vector<float>& foldedImage, const std::vector<Marker>& markerCells,
                         int& bestShiftX, int& bestShiftY)
{
    std::vector<float> shiftScores(TileSize * TileSize);
    for (int candidateShiftY = 0; candidateShiftY < TileSize; ++candidateShiftY)
    {
        for (int candidateShiftX = 0; candidateShiftX < TileSize; ++candidateShiftX)
        {
            float correlationScore = 0;
            for (const Marker& marker : markerCells)
            {
                correlationScore += marker.expectedSign
                                    * foldedImage[((marker.pixelY + candidateShiftY) & (TileSize - 1)) * TileSize
                                                  + ((marker.pixelX + candidateShiftX) & (TileSize - 1))];
            }
            shiftScores[candidateShiftY * TileSize + candidateShiftX] = correlationScore;
        }
    }

    double meanScore = 0;
    double scoreVariance = 0;
    int bestScoreIndex = 0;
    for (int scoreIndex = 0; scoreIndex < TileSize * TileSize; ++scoreIndex)
    {
        meanScore += shiftScores[scoreIndex];
        if (shiftScores[scoreIndex] > shiftScores[bestScoreIndex])
        {
            bestScoreIndex = scoreIndex;
        }
    }
    meanScore /= TileSize * TileSize;
    for (int scoreIndex = 0; scoreIndex < TileSize * TileSize; ++scoreIndex)
    {
        const double distanceFromMean = shiftScores[scoreIndex] - meanScore;
        scoreVariance += distanceFromMean * distanceFromMean;
    }

    const double scoreStdDev = std::sqrt(scoreVariance / (TileSize * TileSize)) + 1e-9;
    bestShiftY = bestScoreIndex / TileSize;
    bestShiftX = bestScoreIndex % TileSize;
    return (shiftScores[bestScoreIndex] - meanScore) / scoreStdDev;
}

DecodeResult decodeScreenshot(const std::vector<unsigned char>& rgbaImage, int imageWidth, int imageHeight,
                              const std::vector<float>& candidateScales)
{
    const stego::Layout& tileLayout = stego::layout();
    // +1: cell left as is (pattern 0); -1: cell darkened (pattern 1).
    auto expectedSignForCell = [&tileLayout](int cellIndex) { return tileLayout.pattern[cellIndex] ? -1.f : 1.f; };

    std::vector<Marker> markerCells;
    for (int cellIndex = 0; cellIndex < stego::CellCount; ++cellIndex)
    {
        if (tileLayout.ownerBit[cellIndex] < 0)
        {
            markerCells.push_back({(cellIndex % stego::GridSize) * PixelsPerCell,
                                   (cellIndex / stego::GridSize) * PixelsPerCell,
                                   expectedSignForCell(cellIndex)});
        }
    }

    const std::vector<float> highPassPixels = makeHighPassImage(rgbaImage, imageWidth, imageHeight, 6);

    DecodeResult result;
    result.markerPeakScore = -1;
    std::vector<float> bestFoldedImage;
    for (float candidateScale : candidateScales)
    {
        std::vector<float> foldedImage = foldImageIntoTile(highPassPixels, imageWidth, imageHeight, candidateScale);
        int bestShiftX = 0;
        int bestShiftY = 0;
        const double markerPeakScore = findBestTileShift(foldedImage, markerCells, bestShiftX, bestShiftY);
        if (markerPeakScore > result.markerPeakScore)
        {
            result.markerPeakScore = markerPeakScore;
            result.imageScale = candidateScale;
            result.tileShiftX = bestShiftX;
            result.tileShiftY = bestShiftY;
            bestFoldedImage = std::move(foldedImage);
        }
    }

    std::array<double, stego::BitCount> bitScores{};
    for (int cellIndex = 0; cellIndex < stego::CellCount; ++cellIndex)
    {
        const int bitIndex = tileLayout.ownerBit[cellIndex];
        if (bitIndex < 0)
        {
            continue;
        }

        const int cellPixelX = (cellIndex % stego::GridSize) * PixelsPerCell;
        const int cellPixelY = (cellIndex / stego::GridSize) * PixelsPerCell;
        float cellScoreSum = 0;
        for (int pixelOffsetY = 0; pixelOffsetY < PixelsPerCell; ++pixelOffsetY)
        {
            for (int pixelOffsetX = 0; pixelOffsetX < PixelsPerCell; ++pixelOffsetX)
            {
                const int tilePixelY = (cellPixelY + pixelOffsetY + result.tileShiftY) & (TileSize - 1);
                const int tilePixelX = (cellPixelX + pixelOffsetX + result.tileShiftX) & (TileSize - 1);
                cellScoreSum += bestFoldedImage[tilePixelY * TileSize + tilePixelX];
            }
        }
        const float averageCellScore = cellScoreSum / (PixelsPerCell * PixelsPerCell);
        bitScores[bitIndex] += expectedSignForCell(cellIndex) * averageCellScore;
    }

    for (int bitIndex = 0; bitIndex < stego::BitCount; ++bitIndex)
    {
        if (bitScores[bitIndex] > 0)
        {
            result.payload[bitIndex / 8] |= static_cast<uint8_t>(1u << (bitIndex % 8));
        }
        result.bitConfidence[bitIndex] = std::fabs(bitScores[bitIndex]);
    }
    return result;
}

const std::vector<float> CandidateTileScales = {1, 1.25, 1.5, 1.75, 2, 2.25, 2.5, 2.75, 3, 3.25, 3.5};
} // namespace

class StegoNoiseTests : public CPPUNIT_NS::TestFixture
{
    CPPUNIT_TEST_SUITE(StegoNoiseTests);

    CPPUNIT_TEST(testMarkedScreenshots);

    CPPUNIT_TEST_SUITE_END();

    void testMarkedScreenshots()
    {
        constexpr std::string_view testname = __func__;
        const std::filesystem::path imageDirectory = std::filesystem::path(TDOC) / "stego";
        LOK_ASSERT_MESSAGE("no image directory: " + imageDirectory.string(), std::filesystem::is_directory(imageDirectory));

        const Payload expectedPayload = makePayloadFromUuids();
        int checkedImageCount = 0;
        for (const auto& imageEntry : std::filesystem::directory_iterator(imageDirectory))
        {
            if (!imageEntry.is_regular_file() || !isSupportedImageFile(imageEntry.path()))
            {
                continue;
            }

            int imageWidth = 0, imageHeight = 0;
            const auto rgbaImage = loadRgbaImage(imageEntry.path().string(), imageWidth, imageHeight);
            LOK_ASSERT_MESSAGE("cannot load " + imageEntry.path().string(), !rgbaImage.empty());

            const DecodeResult decodeResult = decodeScreenshot(rgbaImage, imageWidth, imageHeight, CandidateTileScales);
            const int incorrectByteCount = countIncorrectBytes(decodeResult.payload, expectedPayload);

            const auto decodedUuids = payloadToUuids(decodeResult.payload);
            std::string diagnosticMessage = imageEntry.path().filename().string()
                                            + ": scale=" + std::to_string(decodeResult.imageScale)
                                            + " shift=(" + std::to_string(decodeResult.tileShiftX) + ","
                                            + std::to_string(decodeResult.tileShiftY) + ")"
                                            + " peak=" + std::to_string(decodeResult.markerPeakScore)
                                            + " wrong=" + std::to_string(incorrectByteCount)
                                            + " decoded uuids={" + decodedUuids[0] + ", " + decodedUuids[1] + "}";

            std::cerr << diagnosticMessage << '\n';

            LOK_ASSERT_MESSAGE(diagnosticMessage, decodedUuids[0] == UUID1);
            LOK_ASSERT_MESSAGE(diagnosticMessage, decodedUuids[1] == UUID2);

            LOK_ASSERT_MESSAGE(diagnosticMessage, decodeResult.markerPeakScore > MinMarkerPeakScore);
            LOK_ASSERT_MESSAGE(diagnosticMessage, incorrectByteCount <= MaxIncorrectBytes);
            ++checkedImageCount;
        }
        LOK_ASSERT_MESSAGE("no images in " + imageDirectory.string(), checkedImageCount > 0);
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(StegoNoiseTests);
/* vim:set shiftwidth=4 softtabstop=4 expandtab: */