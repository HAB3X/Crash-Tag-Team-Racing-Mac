// The SPU2's output (ps2/sound.h): samples made as they're played, 48 kHz stereo, through an SDL3 audio stream whose callback
// (on SDL's audio thread) makes what the device asks for; with no device (--headless, --mute, or none found) a thread of its own
// makes them at the wall clock's pace. The IOP's hardware timer runs in the SPU2's time between samples, so a driver's thread
// sees its ticks at the same points of the sound however the host schedules the making. --audio-dump writes everything made to
// a WAV file. Nothing here waits for the game's thread or makes it wait, except for the sound lock while samples are made
// (a few hundred at a time)
#include "../runtime.h"
#include "ps2/sound.h"
#include "spu2.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace Ps2
{
namespace
{
constexpr u32 SampleRate = 48000;
// Samples made a step (the most the lock is held for), and how far ahead of the wall clock the thread keeps without a device
constexpr u32 StepFrames = 256;
constexpr u32 LeadFrames = 1600;

std::recursive_mutex g_lock;
SDL_AudioStream* g_stream = nullptr;
std::thread g_thread;
std::atomic<bool> g_running = false;

// The timer: its period and function, and the samples left until it runs
u32 g_timerPeriod = 0;
u32 g_timerLeft = 0;
std::function<void()> g_timer;

// A 16 bit stereo WAV file; its header's sizes are rewritten every half second so it's readable however the run ends
FILE* g_wav = nullptr;
u32 g_wavBytes = 0;
u32 g_wavSinceHeader = 0;

void WavHeader()
{
    auto put32 = [](u32 v) { std::fwrite(&v, 4, 1, g_wav); };
    auto put16 = [](u16 v) { std::fwrite(&v, 2, 1, g_wav); };
    std::fseek(g_wav, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, g_wav);
    put32(36 + g_wavBytes);
    std::fwrite("WAVEfmt ", 1, 8, g_wav);
    put32(16);
    put16(1);
    put16(2);
    put32(SampleRate);
    put32(SampleRate * 4);
    put16(4);
    put16(16);
    std::fwrite("data", 1, 4, g_wav);
    put32(g_wavBytes);
    std::fseek(g_wav, 0, SEEK_END);
}

void WavWrite(const s16* samples, u32 frames)
{
    if (g_wav == nullptr)
    {
        return;
    }
    std::fwrite(samples, 4, frames, g_wav);
    g_wavBytes += frames * 4;
    g_wavSinceHeader += frames;
    if (g_wavSinceHeader >= SampleRate / 2)
    {
        g_wavSinceHeader = 0;
        WavHeader();
        std::fflush(g_wav);
    }
}

// Makes frames of output into out (left, right), the timer's function run in its place among them
void Make(s16* out, u32 frames)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (u32 i = 0; i < frames; i++)
    {
        if (g_timerPeriod != 0 && --g_timerLeft == 0)
        {
            g_timerLeft = g_timerPeriod;
            g_timer();
        }
        Spu2::Stereo s = Spu2::Tick();
        out[i * 2] = static_cast<s16>(std::clamp(s.left, -0x8000, 0x7FFF));
        out[i * 2 + 1] = static_cast<s16>(std::clamp(s.right, -0x8000, 0x7FFF));
    }
    WavWrite(out, frames);
}

void SDLCALL Callback(void*, SDL_AudioStream* stream, int additional, int)
{
    s16 buffer[StepFrames * 2];
    while (additional > 0)
    {
        u32 frames = std::min<u32>(StepFrames, static_cast<u32>(additional + 3) / 4);
        Make(buffer, frames);
        SDL_PutAudioStreamData(stream, buffer, static_cast<int>(frames * 4));
        additional -= static_cast<int>(frames * 4);
    }
}

// Without a device: the wall clock's samples, a little ahead
void Thread()
{
    ThreadOnPerformanceCores();
    s16 buffer[StepFrames * 2];
    auto start = std::chrono::steady_clock::now();
    u64 made = 0;
    while (g_running)
    {
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (made < static_cast<u64>(seconds * SampleRate) + LeadFrames)
        {
            Make(buffer, StepFrames);
            made += StepFrames;
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
}

std::unique_lock<std::recursive_mutex> SoundLock() { return std::unique_lock<std::recursive_mutex>(g_lock); }

void SoundSetTimer(u32 samples, std::function<void()> tick)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_timer = std::move(tick);
    g_timerPeriod = samples;
    g_timerLeft = samples;
}

void SoundInit()
{
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        Spu2::Reset();
    }
    if (!g_options.audioDump.empty())
    {
        g_wav = std::fopen(g_options.audioDump.c_str(), "wb");
        if (g_wav == nullptr)
        {
            Log("audio: can't write %s", g_options.audioDump.c_str());
        }
        else
        {
            WavHeader();
        }
    }
    if (!g_options.headless && !g_options.mute)
    {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            Log("audio: SDL's audio didn't start: %s", SDL_GetError());
        }
        else
        {
            SDL_AudioSpec spec = {SDL_AUDIO_S16LE, 2, static_cast<int>(SampleRate)};
            g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Callback, nullptr);
            if (g_stream == nullptr)
            {
                Log("audio: no audio device: %s", SDL_GetError());
            }
            else
            {
                SDL_ResumeAudioStreamDevice(g_stream);
                Log("audio: playing at %u Hz", SampleRate);
                return;
            }
        }
    }
    g_running = true;
    g_thread = std::thread(Thread);
}

void SoundStop()
{
    if (g_running)
    {
        g_running = false;
        g_thread.join();
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_wav != nullptr)
    {
        WavHeader();
        std::fclose(g_wav);
        g_wav = nullptr;
        Log("audio: %.1f s written to %s", g_wavBytes / 4.0 / SampleRate, g_options.audioDump.c_str());
    }
}
}
