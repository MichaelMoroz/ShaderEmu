// Sound out for rvc_harness (docs/sound.md). The sound card draws a ring of samples from a
// cursor on; SoundOut keeps the newest of them and an XAudio2 voice pulls from it, so the
// audio thread's position is the clock. SoundCapture writes the same samples to a WAV file.
#pragma once

#include "common.h"

#include <xaudio2.h>

#include <atomic>
#include <cmath>
#include <string>
#include <vector>

const uint32_t kSoundRing = 16384;   // samples in a mix: SND_RING in sound.h
const uint32_t kSoundRate = 48000;
// Samples before a mix's cursor that keep what an earlier mix put there: they may be playing.
const uint32_t kSoundGuard = 4096;

inline int16_t soundSample(float v) {
    long s = lrintf(v * 32768.0f);
    return (int16_t)(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
}

class SoundOut : public IXAudio2VoiceCallback {
public:
    bool open(std::string& err) {
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kSoundRate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 4;
        wf.nAvgBytesPerSec = kSoundRate * 4;
        HRESULT hr = XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (SUCCEEDED(hr)) hr = engine_->CreateMasteringVoice(&master_);
        if (SUCCEEDED(hr)) hr = engine_->CreateSourceVoice(&voice_, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, this);
        if (FAILED(hr)) {
            err = "XAudio2: " + hrToString(hr);
            close();
            return false;
        }
        ring_.assign((size_t)kSoundRing * 2, 0);
        for (int b = 0; b < kBuffers; ++b) queue(b);
        return SUCCEEDED(voice_->Start());
    }
    void close() {
        if (voice_) voice_->DestroyVoice();
        if (master_) master_->DestroyVoice();
        voice_ = nullptr;
        master_ = nullptr;
        engine_.Reset();
    }
    // The next sample the audio thread will take.
    uint32_t position() const { return pos_.load(); }
    // Carries on from sample `at`, with nothing to play until a mix arrives.
    void restart(uint32_t at) {
        valid_.store(at);
        pos_.store(at);
    }
    void silence() { valid_.store(pos_.load()); }
    void setVolume(float v) {
        if (voice_) voice_->SetVolume(v);
    }
    // A mix: kSoundRing pairs of floats, pair p being the sample congruent to p from `cursor` on.
    void submit(uint32_t cursor, const float* pairs) {
        for (uint32_t k = 0; k < kSoundRing - kSoundGuard; ++k) {
            uint32_t p = (cursor + k) & (kSoundRing - 1);
            ring_[2 * p] = soundSample(pairs[2 * p]);
            ring_[2 * p + 1] = soundSample(pairs[2 * p + 1]);
        }
        valid_.store(cursor + kSoundRing - kSoundGuard);
    }

    void STDMETHODCALLTYPE OnBufferEnd(void* context) override { queue((int)(intptr_t)context); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}

private:
    static const int kBuffers = 3, kChunk = 480;   // 10 ms each

    // Fills buffer b from the ring at the play position and hands it to the voice. Past what
    // the last mix covers there is silence: the ring there holds sound from a turn ago.
    void queue(int b) {
        uint32_t at = pos_.load(), valid = valid_.load();
        for (uint32_t i = 0; i < (uint32_t)kChunk; ++i) {
            uint32_t n = at + i, p = n & (kSoundRing - 1);
            bool have = (int32_t)(valid - n) > 0;
            chunk_[b][2 * i] = have ? ring_[2 * p] : 0;
            chunk_[b][2 * i + 1] = have ? ring_[2 * p + 1] : 0;
        }
        pos_.compare_exchange_strong(at, at + kChunk);   // unless restart() moved it meanwhile
        XAUDIO2_BUFFER xb{};
        xb.AudioBytes = kChunk * 4;
        xb.pAudioData = (const BYTE*)chunk_[b];
        xb.pContext = (void*)(intptr_t)b;
        voice_->SubmitSourceBuffer(&xb);
    }

    ComPtr<IXAudio2> engine_;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* voice_ = nullptr;
    std::vector<int16_t> ring_;
    int16_t chunk_[kBuffers][kChunk * 2] = {};
    std::atomic<uint32_t> pos_{0}, valid_{0};
};

// The samples as a listener at the cursor hears them: each mix gives those up to the next
// mix's cursor. With them, per mix, its cursor and the control row, for tools/sound_reference.py.
class SoundCapture {
public:
    void add(uint32_t cursor, const float* pairs, const uint32_t* controlRow, size_t controlWords) {
        if (have_) {
            // a minute at most: a cursor that jumped must not fill memory with silence
            uint32_t n = (std::min)(cursor - cursor_, kSoundRate * 60);
            for (uint32_t k = 0; k < n; ++k) {
                uint32_t p = (cursor_ + k) & (kSoundRing - 1);
                bool drawn = k < kSoundRing;
                pcm_.push_back(drawn ? soundSample(last_[2 * p]) : 0);
                pcm_.push_back(drawn ? soundSample(last_[2 * p + 1]) : 0);
            }
        }
        last_.assign(pairs, pairs + (size_t)kSoundRing * 2);
        cursor_ = cursor;
        have_ = true;
        frames_.push_back(cursor);
        frames_.insert(frames_.end(), controlRow, controlRow + controlWords);
    }
    // FILE as a 16-bit stereo WAV, and FILE.frames: per mix its cursor, then the control row's words.
    bool write(const std::string& path) const {
        std::vector<uint8_t> wav(44);
        uint32_t bytes = (uint32_t)(pcm_.size() * 2);
        auto put = [&](size_t at, uint32_t v, int n) {
            for (int i = 0; i < n; ++i) wav[at + i] = (uint8_t)(v >> (8 * i));
        };
        memcpy(wav.data(), "RIFF", 4);
        put(4, 36 + bytes, 4);
        memcpy(wav.data() + 8, "WAVEfmt ", 8);
        put(16, 16, 4);
        put(20, 1, 2);
        put(22, 2, 2);
        put(24, kSoundRate, 4);
        put(28, kSoundRate * 4, 4);
        put(32, 4, 2);
        put(34, 16, 2);
        memcpy(wav.data() + 36, "data", 4);
        put(40, bytes, 4);
        wav.insert(wav.end(), (const uint8_t*)pcm_.data(), (const uint8_t*)pcm_.data() + bytes);
        return writeFileBinary(path, wav.data(), wav.size()) &&
               writeFileBinary(path + ".frames", (const uint8_t*)frames_.data(), frames_.size() * 4);
    }

private:
    std::vector<int16_t> pcm_;
    std::vector<float> last_;
    std::vector<uint32_t> frames_;
    uint32_t cursor_ = 0;
    bool have_ = false;
};
