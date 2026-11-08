// 独立验证直读引擎：生成 440Hz 正弦 WAV → 用与 renderAudio 完全相同的解码+插值逻辑回放 → 输出峰值
#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>
#include <functional>

// MFAudioFormat.h 依赖的诊断回调（测试程序不需要输出）
std::function<void (const juce::String&)> mfDebugLog;

#include "../Source/MFAudioFormat.h"

int main (int argc, char** argv)
{
    juce::AudioFormatManager fm;
#if JUCE_WINDOWS
    fm.registerFormat (new MFAudioFormat(), false);   // 与插件一致的优先级
#endif
    fm.registerBasicFormats();
    fm.registerFormat (new juce::MP3AudioFormat(), false);

    // 若命令行传入了文件，则直接测试该文件（MP3/FLAC/OGG/WAV 皆可）
    juce::File testFile;
    if (argc > 1 && argv[1] != nullptr)
    {
        testFile = juce::File (juce::String::fromUTF8 (argv[1]));
        if (! testFile.existsAsFile()) { std::cout << "FAIL: file not found: " << testFile.getFullPathName() << "\n"; return 1; }
    }
    else
    {
        // 生成 2 秒 440Hz 正弦 WAV（44100Hz / 16bit / 单声道）
        testFile = juce::File ("D:/Xiangmu/S-Rack/testtone.wav");
        testFile.deleteFile();
    {
        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (
            new juce::FileOutputStream (testFile), 44100.0, 1, 16, {}, 0));
        if (w == nullptr) { std::cout << "FAIL: writer null\n"; return 1; }

        juce::AudioBuffer<float> buf (1, 4410);
        for (int blk = 0; blk < 20; ++blk)
        {
            for (int i = 0; i < 4410; ++i)
            {
                const double t = (double) (blk * 4410 + i) / 44100.0;
                buf.setSample (0, i, (float) (0.8 * std::sin (2.0 * juce::double_Pi * 440.0 * t)));
            }
            w->writeFromAudioSampleBuffer (buf, 0, 4410);
        }
        w.reset();
    }
    std::cout << "wav generated: " << testFile.getFullPathName() << " size=" << testFile.getSize() << "\n";
    }

    std::cout << "test file: " << testFile.getFullPathName() << "\n";

    // 2) 打开 reader
    auto* reader = fm.createReaderFor (testFile);
    if (reader == nullptr) { std::cout << "FAIL: reader null\n"; return 1; }
    std::cout << "reader: sr=" << reader->sampleRate << " ch=" << reader->numChannels
              << " len=" << reader->lengthInSamples
              << " (" << (reader->sampleRate > 0 ? juce::String (reader->lengthInSamples / reader->sampleRate, 1) : "0") << "s)"
              << " usesFloatingPoint=" << (int) reader->usesFloatingPointData << "\n";
    if (reader->lengthInSamples <= 0)
        std::cout << "WARN: lengthInSamples<=0 (未知时长，顺序解码到 EOF)\n";

    // 3) 与 PluginProcessor::renderAudio 逐行一致的回放（宿主采样率 48000，块 512）
    //    可选 argv[2] = 起始秒数（默认 0，用于跳过 MP3 编码器延迟/淡入）
    double startSeconds = 0.0;
    if (argc > 2 && argv[2] != nullptr)
        startSeconds = juce::String (argv[2]).getDoubleValue();

    const int n = 512;
    const double devRate = 48000.0;
    const double fileRate = reader->sampleRate;
    const double ratio = fileRate / devRate;
    const juce::int64 fileLen = reader->lengthInSamples;
    const bool unknownLen = fileLen <= 0;
    const juce::int64 readLimit = unknownLen ? (juce::int64) (fileRate * 600) : fileLen;   // 未知时长最多测 10 分钟

    juce::AudioBuffer<float> scratch ((int) reader->numChannels, 0);
    juce::AudioBuffer<float> out (2, n);
    double readerPos = startSeconds * fileRate;
    double peakAll = 0.0;
    int blocks = 0;

    for (int blk = 0; blk < 2000 && readerPos < (double) readLimit - 2.0; ++blk)
    {
        const int need = (int) std::ceil ((double) n * ratio) + 3;
        if (scratch.getNumSamples() < need)
            scratch.setSize ((int) reader->numChannels, need, false, false, true);

        auto startPos = (juce::int64) readerPos;
        auto readable = unknownLen ? (juce::int64) need
                                   : juce::jlimit<juce::int64> (0, (juce::int64) need, fileLen - startPos);
        scratch.clear();
        if (readable > 0)
            reader->read (scratch.getArrayOfWritePointers(), scratch.getNumChannels(),
                          startPos, (int) readable);

        const float frac0 = (float) (readerPos - (double) startPos);
        out.clear();

        for (int ch = 0; ch < 2; ++ch)
        {
            const int src = (reader->numChannels == 1) ? 0
                                                       : juce::jmin (ch, (int) reader->numChannels - 1);
            const float* s = scratch.getReadPointer (src);
            float* dst = out.getWritePointer (ch);

            for (int i = 0; i < n; ++i)
            {
                const double p = (double) frac0 + (double) i * ratio;
                const int idx = (int) p;
                const float fr = (float) (p - (double) idx);
                dst[i] += (s[idx] + (s[idx + 1] - s[idx]) * fr) * 0.8f;
            }
        }

        double peak = 0.0;
        double sum = 0.0;
        double minv = 1e9, maxv = -1e9;
        for (int i = 0; i < n; ++i)
        {
            const double v = out.getSample (0, i);
            peak = juce::jmax (peak, std::abs (v));
            sum += v;
            minv = juce::jmin (minv, v);
            maxv = juce::jmax (maxv, v);
        }
        if (blk < 3 || blk == 1000)
            std::cout << "blk" << blk << " peak=" << peak << " min=" << minv
                      << " max=" << maxv << " mean=" << sum / n << "\n";
        peakAll = juce::jmax (peakAll, peak);
        readerPos += (double) n * ratio;
        ++blocks;
    }

    std::cout << "blocks=" << blocks << " TOTAL PEAK=" << peakAll << "\n";
    std::cout << (peakAll > 0.3 ? "ENGINE OK" : "ENGINE SILENT - BUG CONFIRMED") << "\n";
    return 0;
}
