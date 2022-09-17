#include <cassert>
#include <cinttypes>
#include <cstring>

#include "audio/audio.hpp"
#include "engine/engine.hpp"

#include "avi-file.hpp"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#ifdef PROFILER
#include "engine/profiler.hpp"

extern blit::ProfilerProbe *profilerVidReadProbe;
extern blit::ProfilerProbe *profilerVidDecProbe;
extern blit::ProfilerProbe *profilerAudReadProbe;
#endif

static Chunk readChunk(blit::File &file, uint32_t offset)
{
    Chunk ret{};
    file.read(offset, 8, reinterpret_cast<char *>(&ret));
    return ret;
}

static bool checkId(char *id, const char *exId)
{
    if(memcmp(id, exId, 4) != 0)
    {
        printf("Expected %s, got %c%c%c%c\n", exId, id[0], id[1], id[2], id[3]);
        return false;
    }

    return true;
}

bool AVIFile::load(std::string filename)
{
    frameDataOffset = 0;
    playing = false;
    streams.clear();
    audioFormat = AudioFormat::None;
    currentSample = nullptr;

    file.open(filename);
    if(!file.is_open())
        return false;

    auto headChunk = readChunk(file, 0);

    if(!checkId(headChunk.id, "RIFF"))
        return false;

    char buf[4];
    file.read(8, 4, buf);

    if(!checkId(buf, "AVI "))
        return false;

    uint32_t offset = 12;

    while(offset < headChunk.len - 12)
    {
        auto chunk = readChunk(file, offset);
        std::string idStr(chunk.id, 4);

        if(idStr == "LIST")
        {
            file.read(offset + 8, 4, buf);
            std::string listIdStr(buf, 4);
            if(listIdStr == "hdrl")
            {
                if(!parseHeaders(offset + 12, chunk.len - 12))
                    return false;
            }
            else if(listIdStr == "movi")
            {
                // stream data
                frameDataOffset = offset + 8;
            }
        }
        else if(idStr == "idx1")
        {
            // reserve vectors
            for(auto &stream : streams)
                stream.frameOffsets.reserve(stream.length);

            std::vector<uint32_t> streamOffsets;
            streamOffsets.resize(streams.size());

            uint32_t idxOff = offset + 8;
            auto end = offset + 8 + chunk.len;
            while(idxOff < end)
            {
                uint32_t data[3]; // flags,offset, size
                file.read(idxOff, 4, buf);
                file.read(idxOff + 4, 12, reinterpret_cast<char *>(data));

                int streamNum = (buf[0] - '0') * 10 + (buf[1] - '0');

                auto &frameOffsets = streams[streamNum].frameOffsets;

                auto relOff = data[1] - streamOffsets[streamNum];
                assert((relOff & 1) == 0); // aligned
                assert(relOff < 0x20000);
                frameOffsets.push_back(relOff / 2);

                streamOffsets[streamNum] = data[1];

                idxOff += 16;
            }

            for(auto &stream : streams)
                stream.curOffset = frameDataOffset + stream.frameOffsets[0] * 2;
        }

        offset += 8 + chunk.len;
        if(chunk.len & 1)
            offset++;
    }

    if(audioFormat != AudioFormat::None)
    {
        for(int i = 0; i < numAudioBufs; i++)
            dataSize[i] = 0;

        if(audioFormat == AudioFormat::MP3)
            mp3dec_init(&mp3dec);
    }

    return frameDataOffset != 0;
}

void AVIFile::play(int audioChannel)
{
    if(!frameDataOffset)
        return;

    startTime = blit::now();
    channel = audioChannel;
    playing = true;
    decodedFirstFrame = false;
    bufferedSamples = 0;

    update(startTime); // decode first frame

    if(audioFormat == AudioFormat::None)
        return;

    blit::channels[channel].waveforms = blit::Waveform::WAVE;
    blit::channels[channel].user_data = this;
    blit::channels[channel].wave_buffer_callback = staticAudioCallback;
}

void AVIFile::stop()
{
    playing = false;

    if(channel != -1 && audioFormat != AudioFormat::None)
    {
        blit::channels[0].off();
        currentSample = nullptr;
    }
}

void AVIFile::update(uint32_t time)
{
    if(!file.is_open() || !playing)
        return;

    if(time < startTime)
        return; // time-travel!

    // use audio playback as timer if possible
    if(audioFormat != AudioFormat::None)
        time = (uint64_t(bufferedSamples + blit::channels[channel].wave_buf_pos) * 1000) / 22050;
    else
        time -= startTime;

    for(auto &stream : streams)
    {
        if(stream.type == StreamType::Video)
        {
            if(stream.curFrame + 1 == stream.length)
                continue;

            auto nextFrameTime = ((stream.curFrame + 1) * mainHead.usPerFrame) / 1000;

            // not ready to show next frame
            if(nextFrameTime > time && decodedFirstFrame)
                continue;

            // skip frames
            while(nextFrameTime <= time && stream.curFrame < stream.length)
            {
                nextFrame(stream);
                nextFrameTime = ((stream.curFrame + 1) * mainHead.usPerFrame) / 1000;
            }

            auto chunk = readChunk(file, stream.curOffset);

            if(chunk.len == 0)
                continue;

            auto buf = new uint8_t[chunk.len];

#ifdef PROFILER
            profilerVidReadProbe->start();
#endif
            file.read(stream.curOffset + 8, chunk.len, (char *)buf);

#ifdef PROFILER
            profilerVidReadProbe->store_elapsed_us();
#endif

#ifdef PROFILER
            profilerVidDecProbe->start();
#endif

            if(jpeg.data)
                delete[] jpeg.data;
            jpeg = blit::decode_jpeg_buffer(buf, chunk.len);

#ifdef PROFILER
            profilerVidDecProbe->store_elapsed_us();
#endif

            delete[] buf;
            decodedFirstFrame = true;
        }
        else if(stream.type == StreamType::Audio)
        {
            // assumes 22050Hz mono
            for(int i = 0; i < numAudioBufs; i++)
            {
                if(dataSize[i])
                    continue;

                if(stream.curFrame >= stream.frameOffsets.size())
                {
                    dataSize[i] = -1;
                    continue;
                }

#ifdef PROFILER
                profilerAudReadProbe->start();
#endif

                auto chunk = readChunk(file, stream.curOffset);

                int read = 0;

                if(audioFormat == AudioFormat::PCM)
                {
                    // raw data
                    while(read + chunk.len / 2 < audioBufSize)
                    {
                        file.read(stream.curOffset + 8, chunk.len, reinterpret_cast<char *>(audioBuf[i] + read));
                        read += chunk.len / 2;
                        if(!nextFrame(stream))
                            break;

                        chunk = readChunk(file, stream.curOffset);
                    }
                }
                else if(audioFormat == AudioFormat::MP3)
                {
                    // guess a bit how much data we can decode
                    while(read + MINIMP3_MAX_SAMPLES_PER_FRAME / 2 < audioBufSize)
                    {
                        auto buf = new uint8_t[chunk.len];
                        file.read(stream.curOffset + 8, chunk.len, reinterpret_cast<char *>(buf));
                        mp3dec_frame_info_t info;
                        read += mp3dec_decode_frame(&mp3dec, buf, chunk.len, audioBuf[i] + read, &info);

                        delete[] buf;

                        if(!nextFrame(stream))
                            break;

                        chunk = readChunk(file, stream.curOffset);
                    }
                }

                dataSize[i] = read;

                //printf("refilled %i %i %i %i\n", i, dataSize[i], stream.curFrame, stream.length);
#ifdef PROFILER
                profilerAudReadProbe->store_elapsed_us();
#endif
                if(!currentSample && dataSize[i])
                {
                    // start of stream
                    curAudioBuf = i;
                    endSample = audioBuf[i] + dataSize[i];
                    currentSample = audioBuf[i];
                    blit::channels[channel].adsr = 0xFFFF00;
                    blit::channels[channel].trigger_sustain();
                }
            }
        }
    }
}

void AVIFile::render()
{
    if(!jpeg.data)
        return;

    auto xOff = (blit::screen.bounds.w - jpeg.size.w) / 2;
    auto yOff = (blit::screen.bounds.h - jpeg.size.h) / 2;

    for(int y = 0; y < jpeg.size.h; y++)
    {
        auto p = blit::screen.ptr(xOff, y + yOff);
        memcpy(p, jpeg.data + y * jpeg.size.w * 3, jpeg.size.w * 3);
    }
}

bool AVIFile::parseHeaders(uint32_t offset, uint32_t len)
{
    auto chunk = readChunk(file, offset);

    if(!checkId(chunk.id, "avih"))
        return false;

    if(file.read(offset + 8, sizeof(AVIHChunk), reinterpret_cast<char *>(&mainHead)) != sizeof(AVIHChunk))
        return false;

    if(mainHead.width > static_cast<uint32_t>(blit::screen.bounds.w) || mainHead.height > static_cast<uint32_t>(blit::screen.bounds.h))
    {
        printf("file size %" PRIu32 "x%" PRIu32 " is bigger than the screen...\n", mainHead.width, mainHead.height);
        return false;
    }

    //printf("us/f %u maxbps %u align %u flags %x frames %u initframes %u streams %u sugbufsize %u w %u h %u\n", mainHead.usPerFrame, mainHead.maxBytesPerSec, mainHead.alignment, mainHead.flags, mainHead.numFrames, mainHead.initialFrames, mainHead.numStreams, mainHead. suggestedBufferSize, mainHead.width, mainHead.height);

    offset += chunk.len + 8;
    if(chunk.len & 1)
        offset++;

    // read strl lists
    for(unsigned int i = 0; i < mainHead.numStreams; i++)
    {
        chunk = readChunk(file, offset);
        if(!checkId(chunk.id, "LIST"))
            return false;

        auto listEnd = offset + 8 + chunk.len;

        char buf[4];
        file.read(offset + 8, 4, buf);
        if(!checkId(buf, "strl"))
            return false;

        offset += 12;

        // strh
        auto strhChunk = readChunk(file, offset);
        if(!checkId(strhChunk.id, "strh"))
            return false;

        STRHChunk streamHeader;

        if(file.read(offset + 8, sizeof(STRHChunk), reinterpret_cast<char *>(&streamHeader)) != sizeof(STRHChunk))
            return false;

        offset += strhChunk.len + 8;
        if(strhChunk.len & 1)
            offset++;

        std::string streamType(streamHeader.type, 4);
        std::string streamHandler(streamHeader.handler, 4);

        /*
        printf("stream type %s handler %s flags %x priority %u lang %u initframes %u scale %u rate %u start %u length %u sugbufsize %u quality %u samplesize %u frame %i %i %i %i\n",
            streamType.c_str(), streamHandler.c_str(), streamHeader.flags, streamHeader.priority, streamHeader.language, streamHeader.initialFrames,
            streamHeader.scale, streamHeader.rate, streamHeader.start, streamHeader.length, streamHeader.suggestedBufferSize, streamHeader.quality,
            streamHeader.sampleSize, streamHeader.frameLeft, streamHeader.frameTop, streamHeader.frameRight, streamHeader.frameBottom);
        */

        if(streamType == "vids" && streamHandler != "MJPG")
        {
            printf("Unsupported video hander: %s\n", streamHandler.c_str());
            return false;
        }

        // strf
        auto strfChunk = readChunk(file, offset);
        if(!checkId(strfChunk.id, "strf"))
            return false;

        // read strf...
        if(streamType == "auds")
        {
            // WAVEFORMATEX
            uint16_t format, channels;
            uint32_t sampleRate;
            file.read(offset + 8, 2, reinterpret_cast<char *>(&format));
            file.read(offset + 10, 2, reinterpret_cast<char *>(&channels));
            file.read(offset + 12, 4, reinterpret_cast<char *>(&sampleRate));

            if(format == 1)
                audioFormat = AudioFormat::PCM;
            else if(format == 0x55)
                audioFormat = AudioFormat::MP3;
            else
            {
                printf("Unsupported audio format: %x\n", format);
                audioFormat = AudioFormat::None;
            }

            if(channels != 1 || sampleRate != 22050)
            {
                printf("Unsupported audio channels/sample rate: %i %" PRIu32 "Hz\n", channels, sampleRate);
                audioFormat = AudioFormat::None;
            }
        }

        offset += strfChunk.len + 8;
        if(strfChunk.len & 1)
            offset++;

        Stream stream;
        stream.length = streamHeader.length;

        if(streamType == "vids")
            stream.type = StreamType::Video;
        else if(streamType == "auds")
            stream.type = StreamType::Audio;
        else 
            stream.type = StreamType::Other;

        // read any remaining chunks
        while(offset < listEnd)
        {
            auto strChunk = readChunk(file, offset);
            std::string idStr(strChunk.id, 4);

            puts(idStr.c_str());
            
            offset += strChunk.len + 8;
        }

        streams.push_back(stream);
    }

    return true;
}

bool AVIFile::nextFrame(Stream &stream)
{
    if(stream.curFrame == stream.length)
        return false;

    stream.curFrame++;

    if(stream.curFrame == stream.length)
        return false;

    stream.curOffset += stream.frameOffsets[stream.curFrame] * 2;

    return true;
}

void AVIFile::staticAudioCallback(blit::AudioChannel &channel)
{
    reinterpret_cast<AVIFile *>(channel.user_data)->audioCallback(channel);
}

void AVIFile::audioCallback(blit::AudioChannel &channel)
{
    if(!currentSample)
    {
        channel.off();
        return;
    }

    // there was no buffer last time
    if(currentSample == endSample)
    {
        if(dataSize[curAudioBuf] > 0)
        {
            // recover from underrun
            endSample = audioBuf[curAudioBuf] + dataSize[curAudioBuf];
        }
        else
        {
            if(dataSize[curAudioBuf] == -1) // EOF
                channel.off();

            memset(channel.wave_buffer, 0, 64 * sizeof(int16_t));
            return;
        }
    }

    auto out = channel.wave_buffer;

    int i = 0;
    for(; i < 64 && currentSample != endSample; i++)
        *(out++) = *(currentSample++);


    // swap buffers
    if(currentSample == endSample)
    {
        dataSize[curAudioBuf] = 0;
        curAudioBuf = ++curAudioBuf % numAudioBufs;

        if(dataSize[curAudioBuf] == -1) // EOF
            currentSample = endSample = nullptr;
        else
        {
            currentSample = audioBuf[curAudioBuf];
            endSample = currentSample + dataSize[curAudioBuf];

            // if(currentSample == endSample)
            // no more samples available - underrun
        }
    }

    for(; i < 64 && currentSample != endSample; i++)
        *(out++) = *(currentSample++);

    bufferedSamples += 64;
}
