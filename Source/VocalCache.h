#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <vector>

//==============================================================================
// VocalCache —— 实时分离页使用的「真人声」来源。
//
// 背景：之前实时路径跑的是 HanceEngine 的 center-pan DSP 估计，日志实测
//   [HANCE] quality: corr=0.963 vocal/mix=0.871
// 即所谓「人声」几乎就是原混音（中置的鼓/贝斯/主旋律全部保留）。中性播放时
// 输出 (mix - v) + v 会代数抵消，听感正常；一旦 Solo 人声就直接把这个
// 「伪人声」送到扬声器，于是「独奏仍听到伴奏、音量忽大忽小」。
//
// 本项目里真正可用的分离来自 MDX-Net（SeparationEngine / AiExporter），
// 点「分离音频」会把它导出到 <曲目目录>/AI分离输出/<曲名>_人声.wav
// （同时写出 <曲名>_伴奏.wav 与 导出结果.txt）。
// VocalCache 把这份结果读回来，按文件采样率对齐，供音频线程按绝对文件位置
// 插值读取；于是 人声 = 纯模型人声、伴奏 = mix - 人声（精确伴奏）。
//
// 三种命中方式：
//   1. 当前播放的文件本身就是 *_人声.wav（用户直接试听导出结果）→ 用它自己；
//   2. 同目录 AI分离输出/<base>_人声.wav；
//   3. 同目录 <base>_人声.wav。
// 命中前会校验时长（±3%），避免把别的曲目的分离结果错配上来。
//==============================================================================
class VocalCache
{
public:
    struct Data
    {
        std::vector<float> L, R;
    };

    struct Snapshot
    {
        std::shared_ptr<const Data> buf;
        int length = 0;
        const float* L() const noexcept { return buf ? buf->L.data() : nullptr; }
        const float* R() const noexcept { return buf ? buf->R.data() : nullptr; }
    };

    VocalCache() = default;

    // 不依赖 Processor（本头文件被 PluginProcessor.h 在类定义之前 include）
    static juce::String z (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

    void release()
    {
        length.store (0, std::memory_order_release);
        std::atomic_store_explicit (&pub, std::shared_ptr<const Data> (),
                                    std::memory_order_release);
    }

    Snapshot snapshot() const
    {
        Snapshot s;
        s.buf = std::atomic_load_explicit (&pub, std::memory_order_acquire);
        if (s.buf != nullptr)
        {
            const int cap = (int) juce::jmin (s.buf->L.size(), s.buf->R.size());
            // 先取缓冲再取长度：宁可少读，绝不越界
            s.length = juce::jmax (0, juce::jmin (length.load (std::memory_order_acquire), cap));
        }
        return s;
    }

    bool isLoaded() const { return length.load (std::memory_order_acquire) > 0; }

    // 消息线程调用（切歌时）。info 返回可直接显示的状态说明。
    bool loadFor (const juce::File& track, double targetRate,
                  juce::int64 expectedLen, juce::String& info)
    {
        release();
        info = juce::String();

        if (track == juce::File() || targetRate <= 0.0 || expectedLen <= 0)
        {
            info = z ("未加载曲目");
            return false;
        }

        const juce::File vf = findVocalFile (track);
        if (vf == juce::File())
        {
            info = z ("本曲尚无分离结果，请点「分离音频」");
            return false;
        }

        juce::AudioFormatManager mgr;
        mgr.registerFormat (new juce::WavAudioFormat(), false);
        std::unique_ptr<juce::AudioFormatReader> rd (mgr.createReaderFor (vf));
        if (rd == nullptr || rd->lengthInSamples <= 0 || rd->sampleRate <= 0.0)
        {
            info = z ("人声缓存无法打开：") + vf.getFileName();
            return false;
        }

        const double srcRate = rd->sampleRate;
        const juce::int64 nIn = rd->lengthInSamples;
        const double srcSec = (double) nIn / srcRate;
        const double trkSec = (double) expectedLen / targetRate;
        if (trkSec > 0.0 && std::abs (srcSec - trkSec) > 0.03 * juce::jmax (1.0, trkSec))
        {
            info = z ("人声缓存与当前曲目时长不符，已忽略");
            return false;
        }
        // 上限 30 分钟，避免异常文件把内存吃满
        if (nIn > (juce::int64) 30 * 60 * 192000)
        {
            info = z ("人声缓存过大，已忽略");
            return false;
        }

        const int numCh = (int) rd->numChannels;
        std::vector<float> srcL ((size_t) nIn), srcR ((size_t) nIn);
        {
            constexpr int blockN = 48000;
            std::vector<float> tL ((size_t) blockN), tR ((size_t) blockN);
            juce::int64 pos = 0;
            while (pos < nIn)
            {
                const int want = (int) juce::jmin<juce::int64> (blockN, nIn - pos);
                float* chans[2] = { tL.data(), tR.data() };
                rd->read (chans, juce::jmin (2, numCh), pos, want);
                std::copy (tL.begin(), tL.begin() + want, srcL.begin() + pos);
                if (numCh > 1)
                    std::copy (tR.begin(), tR.begin() + want, srcR.begin() + pos);
                else
                    std::copy (tL.begin(), tL.begin() + want, srcR.begin() + pos);
                pos += want;
            }
        }
        rd.reset();

        auto d = std::make_shared<Data>();
        const double ratio = targetRate / srcRate;   // 输出样本数 / 输入样本数
        if (std::abs (ratio - 1.0) < 1.0e-5)
        {
            d->L = std::move (srcL);
            d->R = std::move (srcR);
        }
        else
        {
            const int outN = (int) std::floor ((double) nIn * ratio);
            if (outN <= 0) return false;
            d->L.resize ((size_t) outN);
            d->R.resize ((size_t) outN);
            const double inv = 1.0 / ratio;          // 输出 i → 输入位置 i*inv
            for (int i = 0; i < outN; ++i)
            {
                const double p = (double) i * inv;
                const int idx = (int) p;
                const float fr = (float) (p - (double) idx);
                const int i1 = juce::jmin (idx + 1, (int) nIn - 1);
                d->L[(size_t) i] = srcL[(size_t) idx] + (srcL[(size_t) i1] - srcL[(size_t) idx]) * fr;
                d->R[(size_t) i] = srcR[(size_t) idx] + (srcR[(size_t) i1] - srcR[(size_t) idx]) * fr;
            }
        }

        const int nOut = (int) juce::jmin (d->L.size(), d->R.size());
        std::atomic_store_explicit (&pub, std::shared_ptr<const Data> (d),
                                    std::memory_order_release);
        length.store (nOut, std::memory_order_release);
        info = z ("已载入真人声：") + vf.getFileName();
        return nOut > 0;
    }

    static juce::File findVocalFile (const juce::File& track)
    {
        const juce::String name = track.getFileName();
        // 直接试听导出的人声：它本身就是分离结果
        if (name.endsWithIgnoreCase (z ("_人声.wav")))
            return track.existsAsFile() ? track : juce::File();

        const juce::String base = track.getFileNameWithoutExtension() + z ("_人声.wav");
        const juce::File dir = track.getParentDirectory();
        const juce::File a = dir.getChildFile (z ("AI分离输出")).getChildFile (base);
        if (a.existsAsFile()) return a;
        const juce::File b = dir.getChildFile (base);
        if (b.existsAsFile()) return b;
        return juce::File();
    }

private:
    std::shared_ptr<const Data> pub;
    std::atomic<int> length { 0 };
};
