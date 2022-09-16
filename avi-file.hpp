#pragma once

#include <cstdint>
#include <string>
#include <vector>

#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#include "audio/audio.hpp"
#include "engine/file.hpp"
#include "graphics/jpeg.hpp"

struct Chunk
{
    char id[4];
    uint32_t len;
};

struct AVIHChunk
{
    uint32_t usPerFrame;
    uint32_t maxBytesPerSec;
    uint32_t alignment;
    uint32_t flags;
    uint32_t numFrames;
    uint32_t initialFrames;
    uint32_t numStreams;
    uint32_t suggestedBufferSize;
    uint32_t width;
    uint32_t height;
    // 4x dword reserved
};

struct STRHChunk
{
    char type[4];
    char handler[4];
    uint32_t flags;
    uint16_t priority;
    uint16_t language;
    uint32_t initialFrames;
    uint32_t scale;
    uint32_t rate;
    uint32_t start;
    uint32_t length;
    uint32_t suggestedBufferSize;
    uint32_t quality;
    uint32_t sampleSize;
    int16_t frameLeft;
    int16_t frameTop;
    int16_t frameRight;
    int16_t frameBottom;
};

static_assert(sizeof(Chunk) == 8);
static_assert(sizeof(AVIHChunk) == 40);
static_assert(sizeof(STRHChunk) == 56);

enum class StreamType
{
    Video,
    Audio,
    Other // ignored
};

struct Stream
{
    StreamType type;
    // more
    uint32_t length;

    uint32_t curFrame = 0;
    uint32_t curOffset = 0;
    std::vector<uint32_t> frameOffsets;
};

enum class AudioFormat
{
    None,
    PCM,
    MP3
};

class AVIFile
{
public:
    bool load(std::string filename);

    void play(int audioChannel);
    void stop();

    void update(uint32_t time);
    void render();

    bool getPlaying() const {return playing;}

private:
    bool parseHeaders(uint32_t offset, uint32_t len);

    bool nextFrame(Stream &stream);

    static void staticAudioCallback(blit::AudioChannel &channel);
    void audioCallback(blit::AudioChannel &channel);

    bool playing = false;
    bool decodedFirstFrame = false;

    blit::JPEGImage jpeg = {};

    blit::File file;
    uint32_t frameDataOffset;

    AVIHChunk mainHead;
    std::vector<Stream> streams;
    uint32_t startTime = 0;
    AudioFormat audioFormat = AudioFormat::None;

    // audio bits
    int channel = -1;

    static const int numAudioBufs = 2;
    static const int audioBufSize = 4096;
    int16_t audioBuf[numAudioBufs][audioBufSize];
    int16_t *currentSample = nullptr, *endSample = nullptr;
    int dataSize[numAudioBufs]{};
    int curAudioBuf = 0;

    uint32_t bufferedSamples = 0;

    mp3dec_t mp3dec;
};
