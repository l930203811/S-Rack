#include <juce_audio_utils/juce_audio_utils.h>

#include "../Source/MFAudioFormat.h"

// MFAudioFormat.h 的诊断回调（AudioDump 无需输出）
std::function<void (const juce::String&)> mfDebugLog;

// windows.h 的 min/max 宏污染
#undef min
#undef max

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include <objbase.h>

namespace
{
constexpr int kTargetSampleRate = 48000;

int usage()
{
    std::fprintf(stderr, "Usage: AudioDump.exe <input audio file> <output raw file>\n");
    return 1;
}
} // namespace

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // --threadtest <file>：在 worker 线程复刻 SeparationEngine 的解码路径
    if (argc == 3 && std::string (argv[1]) == "--threadtest")
    {
        mfDebugLog = [] (const juce::String& s) { std::printf ("[mf] %s\n", s.toRawUTF8()); std::fflush (stdout); };
        const juce::File f (juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[2])));
        std::atomic<bool> done { false };
        std::thread worker ([&]()
        {
            std::printf ("[t] thread start\n"); std::fflush (stdout);
            HRESULT chr = ::CoInitializeEx (nullptr, COINIT_MULTITHREADED);
            std::printf ("[t] com init hr=0x%08lX\n", (unsigned long) chr); std::fflush (stdout);
            juce::AudioFormatManager mgr;
            std::printf ("[t] mgr ok\n"); std::fflush (stdout);
            mgr.registerBasicFormats();
            std::printf ("[t] basic formats ok\n"); std::fflush (stdout);
            mgr.registerFormat (new MFAudioFormat(), true);
            std::printf ("[t] mf format ok\n"); std::fflush (stdout);
            std::printf ("[t] existsAsFile=%d\n", (int) f.existsAsFile()); std::fflush (stdout);
            auto ins = f.createInputStream();
            std::printf ("[t] createInputStream=%p size=%lld status failed=%d\n",
                         (void*) ins.get(), (long long) (ins ? ins->getTotalLength() : -1),
                         (int) (ins && ins->getStatus().failed())); std::fflush (stdout);
            ins.reset();
            std::unique_ptr<juce::AudioFormatReader> rd (mgr.createReaderFor (f));
            std::printf ("[t] createReaderFor returned %p\n", (void*) rd.get()); std::fflush (stdout);
            std::printf ("[t] reader ok, sr=%.0f len=%lld\n", rd->sampleRate, rd->lengthInSamples); std::fflush (stdout);

            const int inSr = (int) rd->sampleRate;
            const int numCh = (int) rd->numChannels;
            std::vector<float> decL, decR;
            const int blockN = inSr;
            std::vector<float> tmpL ((size_t) blockN), tmpR ((size_t) blockN);
            juce::int64 pos = 0;
            while (true)
            {
                float* chans[2] = { tmpL.data(), tmpR.data() };
                if (! rd->read (chans, juce::jmin (2, numCh), pos, blockN))
                {
                    std::printf ("[t] block read failed at pos=%lld, small retry\n", pos); std::fflush (stdout);
                    const int smallN = 2048;
                    std::vector<float> sL ((size_t) smallN), sR ((size_t) smallN);
                    while (true)
                    {
                        float* sc[2] = { sL.data(), sR.data() };
                        if (! rd->read (sc, juce::jmin (2, numCh), pos, smallN)) break;
                        decL.insert (decL.end(), sL.begin(), sL.end());
                        if (numCh > 1) decR.insert (decR.end(), sR.begin(), sR.end());
                        else           decR.insert (decR.end(), sL.begin(), sL.end());
                        pos += smallN;
                    }
                    break;
                }
                decL.insert (decL.end(), tmpL.begin(), tmpL.end());
                if (numCh > 1) decR.insert (decR.end(), tmpR.begin(), tmpR.end());
                else           decR.insert (decR.end(), tmpL.begin(), tmpL.end());
                pos += blockN;
                if ((pos / blockN) % 30 == 0)
                    { std::printf ("[t] decoded %.1fs\n", (double) pos / inSr); std::fflush (stdout); }
            }
            std::printf ("[t] decode done: %.1fs\n", (double) decL.size() / inSr); std::fflush (stdout);

            rd.reset();
            std::printf ("[t] reader destroyed\n"); std::fflush (stdout);

            const double ratio = 44100.0 / (double) inSr;
            std::vector<float> a, b;
            a.reserve (decL.size()); b.reserve (decL.size());
            std::printf ("[t] resampling...\n"); std::fflush (stdout);
            for (size_t i = 0; i < decL.size(); ++i) a.push_back (decL[i] * (float) ratio);
            for (size_t i = 0; i < decR.size(); ++i) b.push_back (decR[i] * (float) ratio);
            std::printf ("[t] resample done %zu\n", a.size()); std::fflush (stdout);
            done.store (true);
        });
        const auto t0 = std::chrono::steady_clock::now();
        while (! done.load())
        {
            if (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() > 120.0)
            {
                std::printf ("[t] TIMEOUT 120s, abandoning\n"); std::fflush (stdout);
                std::terminate();
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (200));
        }
        worker.join();
        std::printf ("[t] thread test OK\n");
        ::CoUninitialize();
        return 0;
    }

    if (argc != 3)
        return usage();

    const juce::File inputFile (juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[1])));
    const juce::File outputFile (juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[2])));

    if (! inputFile.existsAsFile())
    {
        std::fprintf(stderr, "Error: input file not found: %s\n", argv[1]);
        return 1;
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    formatManager.registerFormat (new juce::MP3AudioFormat(), true);
    formatManager.registerFormat (new MFAudioFormat(), true);   // MP4/M4A/AAC/WMA（Media Foundation）

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (inputFile));
    if (reader == nullptr)
    {
        std::fprintf(stderr, "Error: cannot create reader for: %s\n", argv[1]);
        return 1;
    }

    const double sourceSampleRate = reader->sampleRate;
    const int numSourceFrames = (int) reader->lengthInSamples;
    const int numSourceChannels = (int) reader->numChannels;

    if (numSourceFrames <= 0)
    {
        std::fprintf(stderr, "Error: empty audio stream\n");
        return 1;
    }

    // 读入整个文件
    juce::AudioBuffer<float> buffer (numSourceChannels, numSourceFrames);
    if (! reader->read (&buffer, 0, numSourceFrames, 0, true, true))
    {
        std::fprintf(stderr, "Error: failed to read audio data\n");
        return 1;
    }
    reader.reset();

    const int numInputChannels = buffer.getNumChannels();

    // 单声道时两声道都取 channel 0；多声道时取前两声道
    auto sampleAt = [&buffer, numInputChannels] (int ch, int index) -> float
    {
        return buffer.getSample (std::min (ch, numInputChannels - 1), index);
    };

    // 重采样后帧数
    const int numTargetFrames = (sourceSampleRate == (double) kTargetSampleRate)
        ? numSourceFrames
        : (int) std::llround ((double) numSourceFrames * (double) kTargetSampleRate / sourceSampleRate);

    // 线性插值重采样到 48kHz，输出立体声
    std::vector<float> left ((size_t) numTargetFrames, 0.0f);
    std::vector<float> right ((size_t) numTargetFrames, 0.0f);

    for (int i = 0; i < numTargetFrames; ++i)
    {
        const double srcPos = (double) i * sourceSampleRate / (double) kTargetSampleRate;
        const int idx = juce::jlimit (0, numSourceFrames - 1, (int) srcPos);
        const int nextIdx = juce::jmin (idx + 1, numSourceFrames - 1);
        const float frac = (float) (srcPos - (double) idx);

        left[(size_t) i] = juce::jlimit (-1.0f, 1.0f,
            sampleAt (0, idx) + (sampleAt (0, nextIdx) - sampleAt (0, idx)) * frac);
        right[(size_t) i] = juce::jlimit (-1.0f, 1.0f,
            sampleAt (1, idx) + (sampleAt (1, nextIdx) - sampleAt (1, idx)) * frac);
    }

    // 交错写 float32 小端原始文件
    juce::FileOutputStream stream (outputFile);
    if (! stream.openedOk())
    {
        std::fprintf(stderr, "Error: cannot open output file: %s\n", argv[2]);
        return 1;
    }

    for (int i = 0; i < numTargetFrames; ++i)
    {
        stream.writeFloat (left[(size_t) i]);
        stream.writeFloat (right[(size_t) i]);
    }

    stream.flush();
    if (stream.getStatus().failed())
    {
        std::fprintf(stderr, "Error: failed writing output file\n");
        return 1;
    }

    std::printf("SR=%d CH=2 FRAMES=%d\n", kTargetSampleRate, numTargetFrames);
    return 0;
}
