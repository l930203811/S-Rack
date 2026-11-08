#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "kiss_fft.h"
#include "kiss_fftr.h"
}

#include "MFAudioFormat.h"

//==============================================================================
// HanceEngine —— 流式人声估计引擎（沿用原 Hance 流式包装，但改用确定性 DSP）。
//
// 【2026-11-08 16:40 起已退出实时播放路径】
//   实测 quality: corr=0.963 / vocal/mix=0.871：中置提取对「中置的鼓、贝斯、
//   主旋律」无效，输出仍然几乎等于原混音。中性播放 (mix-v)+v 代数抵消掩盖了
//   缺陷，Solo 时直接暴露 → 独奏人声仍听到伴奏。实时分离现统一使用 VocalCache
//   （AI 分离页导出的真人声 WAV）或 SeparationEngine 的 MDX 结果，本类保留为
//   工具/实验用途，MediaPlayAudioProcessor 不再启动它。
//
// 为什么弃用 Hance 复刻网络（2026-11-08）：
//   从 VocalsSeparation.hance 提取的权重虽完整，但激活函数、bin→62 频带映射、
//   PreNormLayer、瓶颈注意力、concat 顺序均未标定（见 Tools/hance_format_notes.md）。
//   实测它输出的掩码近似「原混音 × 时变增益」，于是：
//     中性播放 (mix-v)+v = mix 精确还原 → 听不出问题；
//     Solo 人声直接输出 v → 伴奏整段保留、音量忽大忽小；
//     Mute 人声输出 mix-v → 人声残留。
//   混音逻辑怎么改都修不好，因为坏的是人声缓冲本身。
//
// 现行算法（零延迟、无模型、完全确定性）：
//   44100Hz STFT → M=(L+R)/2（中置，人声为主）/ S=(L-R)/2（两侧，伴奏为主）
//   → pan = |M|²/(|M|²+|S|²) 平方锐化 + 逐频带 EMA 时间平滑 + 人声频带权重
//   → 人声谱 = M × mask → iSTFT OLA（左右同相，人声居中）
//   → 逐段发布，伴奏总线仍为位精确的 mix - vocal。
//   该算法只适合做「中置人声预览」，不能作为 Solo / Mute 的真分离来源。
//==============================================================================

namespace hance_cfg
{
    constexpr int    kFft          = 2048;   // STFT 窗长（标定项）
    constexpr int    kHop          = 512;    // STFT hop（标定项）
    constexpr int    kFrames       = 448;    // 模型时间维（固定）
    constexpr int    kBands        = 62;     // 模型频率维（固定）
    constexpr int    kModelSr      = 44100;
    // 激活函数：0=ELU，1=LeakyReLU（标定项）
    constexpr int    kActSel       = 0;
    constexpr float  kLeakySlope   = 0.2f;
    constexpr bool   kBottleneckAttn = true; // 瓶颈 [28,28,128] 自注意力开关
    constexpr bool   kPrenormStd   = true;   // 逐频带标准化（PreNormLayer 近似）
    constexpr float  kLogScale     = 1.0f;   // mag → log1p(mag*kLogScale)
    constexpr bool   kSkipFirst    = true;   // concat 顺序：[skip, 上采样]（标定项）
    constexpr int    kMaxUnknownSeconds = 20 * 60;  // 未知时长文件的解码上限
} // namespace hance_cfg

//==============================================================================
// HanceNet —— 权重加载 + 前向推理（单声道 [448,62] → 掩码 [448,62]）
//==============================================================================
class HanceNet
{
public:
    bool load (const juce::File& binFile)
    {
        if (loaded) return true;
        juce::FileInputStream in (binFile);
        if (in.failedToOpen())
        {
            lastError = "无法打开 " + binFile.getFullPathName();
            return false;
        }
        const juce::int64 fsize = in.getTotalLength();

        auto readF32 = [&] (long long off, size_t count, std::vector<float>& dst) -> bool
        {
            if (off < 0 || off + (long long) (count * 4) > fsize) return false;
            in.setPosition (off);
            dst.resize (count);
            return in.read (dst.data(), (int) (count * 4));
        };

        struct Def { const char* name; int d0, d1, d2, d3; long long off; };
        // 偏移 = vocals_weights.bin 内的紧凑打包偏移（字节）。
        // bin 由 parse_hance7.js 按 35 张量顺序无间隔打包（json 的 offset 字段即打包
        // 偏移，单位 f32）；注意不是原始 .hance 模型里的文件偏移。
        static const Def defs[] =
        {
            { "dconv0",     3,  4,   1,  16, 32 },
            { "datt0.pw",  16, 16,   0,   0, 800 },
            { "datt0.fir",448, 16,   0,   0, 1824 },
            { "dconv1",     3,  3,  16,  32, 30496 },
            { "datt1.pw",  32, 32,   0,   0, 48928 },
            { "datt1.fir",224, 32,   0,   0, 53024 },
            { "dconv2",     5,  3,  32,  64, 81696 },
            { "datt2.pw",  64, 64,   0,   0, 204576 },
            { "datt2.fir",112, 64,   0,   0, 220960 },
            { "dconv3",     2,  3,  64, 128, 249632 },
            { "datt3.pw", 128,128,   0,   0, 446240 },
            { "datt3.fir", 56,128,   0,   0, 511776 },
            { "dconv4",     2,  3, 128, 128, 540448 },
            { "datt4.pw", 128,128,   0,   0, 933664 },
            { "datt4.fir", 28,128,   0,   0, 999200 },
            { "dconv5",     8,  1, 128, 128, 1013536 },
            { "dact5.attn",28, 28, 128,   0, 1537824 },
            { "dact5.gate",28,128,   0,   0, 1939232 },
            { "uconv5",     1,  1, 128, 128, 1953568 },
            { "uatt4.pw", 256,256,   0,   0, 2019104 },
            { "uatt4.fir", 28,256,   0,   0, 2281248 },
            { "uconv4",     2,  3, 128, 256, 2309920 },
            { "uatt3.pw", 256,256,   0,   0, 3096352 },
            { "uatt3.fir", 56,256,   0,   0, 3358496 },
            { "uconv3",     2,  3,  64, 256, 3415840 },
            { "uatt2.pw", 128,128,   0,   0, 3809056 },
            { "uatt2.fir",112,128,   0,   0, 3874592 },
            { "uconv2",     3,  3,  32, 128, 3931936 },
            { "uatt1.pw",  64, 64,   0,   0, 4079392 },
            { "uatt1.fir",224, 64,   0,   0, 4095776 },
            { "uconv1",     3,  3,  16,  64, 4153120 },
            { "uatt0.pw",  32, 32,   0,   0, 4189984 },
            { "uatt0.fir",448, 32,   0,   0, 4194080 },
            { "uconv0",     3,  4,   1,  32, 4251424 },
        };
        for (const auto& d : defs)
        {
            const size_t n = (size_t) juce::jmax (1, d.d0) * juce::jmax (1, d.d1)
                           * juce::jmax (1, d.d2) * juce::jmax (1, d.d3);
            std::vector<float> v;
            if (! readF32 (d.off, n, v))
            {
                lastError = juce::String ("权重读取失败: ") + d.name;
                return false;
            }
            w[d.name] = std::move (v);
        }
        loaded = true;
        return true;
    }

    const juce::String& error() const { return lastError; }
    bool isLoaded() const { return loaded; }

    // in: [448*62] 频带特征；out: [448*62] sigmoid 掩码
    void forward (const float* in, float* outMask)
    {
        using namespace hance_cfg;
        const int T = kFrames, F = kBands;

        // ---- 编码器 ----
        // 注意维度：dconv0 时间步幅=1（448→448，datt0 核 [448,16] 实证），
        // dconv1..4 才是时间步幅 2（448→224→112→56→28）
        convSame (in, T, F, 1, w["dconv0"].data(), 3, 4, 16, 1, 2, e0.data());   // [448,31,16]
        actInPlace (e0.data(), e0.size());
        separable (e0.data(), 448, 31, 16, w["datt0.pw"].data(), w["datt0.fir"].data(), 16, e0a.data());

        convSame (e0a.data(), 448, 31, 16, w["dconv1"].data(), 3, 3, 32, 2, 2, e1.data()); // [224,16,32]
        actInPlace (e1.data(), e1.size());
        separable (e1.data(), 224, 16, 32, w["datt1.pw"].data(), w["datt1.fir"].data(), 32, e1a.data());

        convSame (e1a.data(), 224, 16, 32, w["dconv2"].data(), 5, 3, 64, 2, 2, e2.data()); // [112,8,64]
        actInPlace (e2.data(), e2.size());
        separable (e2.data(), 112, 8, 64, w["datt2.pw"].data(), w["datt2.fir"].data(), 64, e2a.data());

        convSame (e2a.data(), 112, 8, 64, w["dconv3"].data(), 2, 3, 128, 2, 2, e3.data()); // [56,4,128]
        actInPlace (e3.data(), e3.size());
        separable (e3.data(), 56, 4, 128, w["datt3.pw"].data(), w["datt3.fir"].data(), 128, e3a.data());

        convSame (e3a.data(), 56, 4, 128, w["dconv4"].data(), 2, 3, 128, 2, 2, e4.data()); // [28,2,128]
        actInPlace (e4.data(), e4.size());
        separable (e4.data(), 28, 2, 128, w["datt4.pw"].data(), w["datt4.fir"].data(), 128, e4a.data());

        convSame (e4a.data(), 28, 2, 128, w["dconv5"].data(), 8, 1, 128, 1, 1, b4.data()); // [28,2,128]

        // ---- 瓶颈（bndense→bnact 近似）：激活 → 自注意力(dact5 [28,28,128]) + 门控 → 逐点 uconv5 ----
        actInPlace (b4.data(), b4.size());
        if (kBottleneckAttn)
        {
            const float* A = w["dact5.attn"].data();
            const float* G = w["dact5.gate"].data();
            const int Ta = 28, C = 128;
            std::vector<float> wts (Ta);
            for (int f = 0; f < 2; ++f)
                for (int c = 0; c < C; ++c)
                    for (int t0 = 0; t0 < Ta; ++t0)
                    {
                        // A[t0,·,c] 作为注意力 logits（softmax over t1）
                        float mx = -1.0e30f;
                        for (int t1 = 0; t1 < Ta; ++t1)
                        {
                            const float a = A[((size_t) t0 * Ta + t1) * C + c];
                            wts[(size_t) t1] = a;
                            mx = juce::jmax (mx, a);
                        }
                        float Z = 0.0f;
                        for (int t1 = 0; t1 < Ta; ++t1) { wts[(size_t) t1] = std::exp (wts[(size_t) t1] - mx); Z += wts[(size_t) t1]; }
                        float acc = 0.0f;
                        for (int t1 = 0; t1 < Ta; ++t1)
                            acc += (wts[(size_t) t1] / Z) * b4[((size_t) t1 * 2 + f) * C + c];
                        const float g = 1.0f / (1.0f + std::exp (-G[(size_t) t0 * C + c]));
                        b5[((size_t) t0 * 2 + f) * C + c] = acc * g;
                    }
        }
        else
        {
            b5 = b4;
        }

        // ---- 解码器 ----
        pointwise (b5.data(), 28 * 2, 128, w["uconv5"].data(), 128, u5.data());
        concat (e4a.data(), u5.data(), 28 * 2, 128, c4.data());                 // [28,2,256]
        separable (c4.data(), 28, 2, 256, w["uatt4.pw"].data(), w["uatt4.fir"].data(), 256, a4d.data());
        deconv (a4d.data(), 28, 2, 256, w["uconv4"].data(), 2, 3, 128, 2, 2, u4.data());   // [56,4,128]

        concat (e3a.data(), u4.data(), 56 * 4, 128, c3.data());                 // [56,4,256]
        separable (c3.data(), 56, 4, 256, w["uatt3.pw"].data(), w["uatt3.fir"].data(), 256, a3d.data());
        deconv (a3d.data(), 56, 4, 256, w["uconv3"].data(), 2, 3, 64, 2, 2, u3.data());    // [112,8,64]

        concat (e2a.data(), u3.data(), 112 * 8, 64, c2.data());                 // [112,8,128]
        separable (c2.data(), 112, 8, 128, w["uatt2.pw"].data(), w["uatt2.fir"].data(), 128, a2d.data());
        deconv (a2d.data(), 112, 8, 128, w["uconv2"].data(), 3, 3, 32, 2, 2, u2.data());   // [224,16,32]

        concat (e1a.data(), u2.data(), 224 * 16, 32, c1.data());                // [224,16,64]
        separable (c1.data(), 224, 16, 64, w["uatt1.pw"].data(), w["uatt1.fir"].data(), 64, a1d.data());
        deconv (a1d.data(), 224, 16, 64, w["uconv1"].data(), 3, 3, 16, 2, 2, u1.data());   // [448,31,16]

        concat (e0a.data(), u1.data(), 448 * 31, 16, c0.data());                // [448,31,32]
        separable (c0.data(), 448, 31, 32, w["uatt0.pw"].data(), w["uatt0.fir"].data(), 32, a0d.data());
        deconv (a0d.data(), 448, 31, 32, w["uconv0"].data(), 3, 4, 1, 1, 2, outMask);      // [448,62]

        constexpr size_t maskN = (size_t) hance_cfg::kFrames * hance_cfg::kBands;
        for (size_t i = 0; i < maskN; ++i)
            outMask[i] = 1.0f / (1.0f + std::exp (-outMask[i]));
    }

    void alloc()
    {
        using namespace hance_cfg;
        e0.assign ((size_t) 448 * 31 * 16, 0.0f);  e0a = e0;
        e1.assign ((size_t) 224 * 16 * 32, 0.0f);  e1a = e1;
        e2.assign ((size_t) 112 * 8 * 64, 0.0f);   e2a = e2;
        e3.assign ((size_t) 56 * 4 * 128, 0.0f);   e3a = e3;
        e4.assign ((size_t) 28 * 2 * 128, 0.0f);   e4a = e4;
        b4.assign ((size_t) 28 * 2 * 128, 0.0f);   b5 = b4;
        u5.assign ((size_t) 28 * 2 * 128, 0.0f);
        c4.assign ((size_t) 28 * 2 * 256, 0.0f);   a4d = c4;  u4.assign ((size_t) 56 * 4 * 128, 0.0f);
        c3.assign ((size_t) 56 * 4 * 256, 0.0f);   a3d = c3;  u3.assign ((size_t) 112 * 8 * 64, 0.0f);
        c2.assign ((size_t) 112 * 8 * 128, 0.0f);  a2d = c2;  u2.assign ((size_t) 224 * 16 * 32, 0.0f);
        c1.assign ((size_t) 224 * 16 * 64, 0.0f);  a1d = c1;  u1.assign ((size_t) 448 * 31 * 16, 0.0f);
        c0.assign ((size_t) 448 * 31 * 32, 0.0f);  a0d = c0;
        yfull.assign ((size_t) 449 * 33 * 16, 0.0f);   // deconv 全尺寸草稿（最大 uconv1）
    }

private:
    static float act (float x)
    {
        using namespace hance_cfg;
        if constexpr (kActSel == 0) return (x > 0.0f) ? x : std::expm1 (x);
        else                        return (x > 0.0f) ? x : kLeakySlope * x;
    }
    static void actInPlace (float* p, size_t n) { for (size_t i = 0; i < n; ++i) p[i] = act (p[i]); }

    // 'same' 步幅卷积：w[kT][kF][Cin][Cout]，pad = k-1 非对称（PyTorch 风格）
    static void convSame (const float* x, int T, int F, int Cin,
                          const float* w, int kT, int kF, int Cout, int sT, int sF, float* y)
    {
        const int padTB = (kT - 1) / 2, padTA = kT - 1 - padTB;
        const int padFB = (kF - 1) / 2, padFA = kF - 1 - padFB;
        const int oT = (T + padTB + padTA - kT) / sT + 1;
        const int oF = (F + padFB + padFA - kF) / sF + 1;
        std::fill (y, y + (size_t) oT * oF * Cout, 0.0f);
        for (int t = 0; t < oT; ++t)
            for (int f = 0; f < oF; ++f)
            {
                float* yo = y + ((size_t) t * oF + f) * Cout;
                for (int kt = 0; kt < kT; ++kt)
                {
                    const int ti = t * sT - padTB + kt;
                    if (ti < 0 || ti >= T) continue;
                    for (int kf = 0; kf < kF; ++kf)
                    {
                        const int fi = f * sF - padFB + kf;
                        if (fi < 0 || fi >= F) continue;
                        const float* xw = x + ((size_t) ti * F + fi) * Cin;
                        const float* ww = w + (size_t) (kt * kF + kf) * Cin * Cout;
                        for (int ci = 0; ci < Cin; ++ci)
                        {
                            const float xv = xw[ci];
                            const float* wr = ww + (size_t) ci * Cout;
                            for (int co = 0; co < Cout; ++co) yo[co] += xv * wr[co];
                        }
                    }
                }
            }
    }

    // 转置卷积：w[kT][kF][Cout][Cin]，输出 = Ti*sT（TF 'same'），多余裁剪对半分
    void deconv (const float* x, int Ti, int Fi, int Cin,
                 const float* w, int kT, int kF, int Cout, int sT, int sF, float* y)
    {
        const int LT = (Ti - 1) * sT + kT, LF = (Fi - 1) * sF + kF;
        const int oT = Ti * sT, oF = Fi * sF;
        const int cropTB = (LT - oT) / 2, cropFB = (LF - oF) / 2;
        std::fill (yfull.begin(), yfull.begin() + (size_t) LT * LF * Cout, 0.0f);
        for (int ti = 0; ti < Ti; ++ti)
            for (int fj = 0; fj < Fi; ++fj)
            {
                const float* xw = x + ((size_t) ti * Fi + fj) * Cin;
                for (int kt = 0; kt < kT; ++kt)
                    for (int kf = 0; kf < kF; ++kf)
                    {
                        float* dst = yfull.data() + (((size_t) (ti * sT + kt) * LF) + (fj * sF + kf)) * Cout;
                        const float* ww = w + (size_t) (kt * kF + kf) * Cout * Cin;
                        for (int co = 0; co < Cout; ++co)
                        {
                            const float* wr = ww + (size_t) co * Cin;
                            float s = 0.0f;
                            for (int ci = 0; ci < Cin; ++ci) s += xw[ci] * wr[ci];
                            dst[co] += s;
                        }
                    }
            }
        for (int t = 0; t < oT; ++t)
            std::memcpy (y + (size_t) t * oF * Cout,
                         yfull.data() + ((size_t) (t + cropTB) * LF + cropFB) * Cout,
                         sizeof (float) * (size_t) oF * Cout);
    }

    // 分离卷积：数据 [T,F,C]（扁平 T*F 行）；逐点 [Cout,Cin] 按位置 →
    // 逐输出通道时间核 [T_l,Cout]（核长 = T_l，'same' 中心对齐，沿时间轴卷积）
    static void separable (const float* x, int T, int F, int Cin,
                           const float* pw, const float* fir, int Cout, float* y)
    {
        const int N = T * F;
        static thread_local std::vector<float> tmp;
        tmp.resize ((size_t) N * Cout);
        for (int i = 0; i < N; ++i)
        {
            const float* xw = x + (size_t) i * Cin;
            float* tw = &tmp[(size_t) i * Cout];
            for (int co = 0; co < Cout; ++co)
            {
                const float* wr = pw + (size_t) co * Cin;
                float s = 0.0f;
                for (int ci = 0; ci < Cin; ++ci) s += xw[ci] * wr[ci];
                tw[co] = s;
            }
        }
        const int K = T;                       // 核长 = 时间维长度（权重表 [T_l,C] 实证）
        const int padB = (K - 1) / 2;
        for (int t = 0; t < T; ++t)
            for (int f = 0; f < F; ++f)
            {
                float* yw = y + ((size_t) t * F + f) * Cout;
                for (int co = 0; co < Cout; ++co) yw[co] = 0.0f;
                for (int tau = 0; tau < K; ++tau)
                {
                    const int ti = t - padB + tau;
                    if (ti < 0 || ti >= T) continue;
                    const float* tw = &tmp[((size_t) ti * F + f) * Cout];
                    const float* kw = fir + (size_t) tau * Cout;
                    for (int co = 0; co < Cout; ++co) yw[co] += tw[co] * kw[co];
                }
            }
    }

    static void pointwise (const float* x, int N, int Cin, const float* w, int Cout, float* y)
    {
        for (int i = 0; i < N; ++i)
        {
            const float* xw = x + (size_t) i * Cin;
            float* yw = y + (size_t) i * Cout;
            for (int co = 0; co < Cout; ++co)
            {
                const float* wr = w + (size_t) co * Cin;
                float s = 0.0f;
                for (int ci = 0; ci < Cin; ++ci) s += xw[ci] * wr[ci];
                yw[co] = s;
            }
        }
    }

    // concat：扁平 [N,C]；顺序 [skip, up]（标定项 kSkipFirst）
    static void concat (const float* skip, const float* up, int N, int C, float* y)
    {
        using namespace hance_cfg;
        for (int i = 0; i < N; ++i)
            for (int c = 0; c < C; ++c)
            {
                const size_t o = (size_t) i * 2 * C;
                if (kSkipFirst) { y[o + c] = skip[(size_t) i * C + c]; y[o + C + c] = up[(size_t) i * C + c]; }
                else            { y[o + c] = up[(size_t) i * C + c];   y[o + C + c] = skip[(size_t) i * C + c]; }
            }
    }

    bool loaded = false;
    juce::String lastError;
    std::map<std::string, std::vector<float>> w;

    std::vector<float> e0, e0a, e1, e1a, e2, e2a, e3, e3a, e4, e4a;
    std::vector<float> b4, b5, u5, c4, a4d, u4, c3, a3d, u3, c2, a2d, u2, c1, a1d, u1, c0, a0d;
    std::vector<float> yfull;
};

//==============================================================================
// HanceCore —— 流式处理核心（STFT + 分段推理 + iSTFT OLA），引擎与测试工具共用
//==============================================================================
struct HanceCore
{
    // mix44: [n44] L/R；voc44: 输出 44.1kHz 人声（增长式）；onSeg(done44) 每段回调
    static void processStream44 (HanceNet& net,
                                 const float* mix44L, const float* mix44R, int n44,
                                 std::vector<float>& voc44L, std::vector<float>& voc44R,
                                 const std::function<void (int)>& onSeg,
                                 const std::function<bool()>& cancelled)
    {
        using namespace hance_cfg;
        const int fft = kFft, hop = kHop, bins = fft / 2 + 1;
        voc44L.clear(); voc44R.clear();
        if (n44 <= 0) return;
        voc44L.assign ((size_t) n44, 0.0f);
        voc44R.assign ((size_t) n44, 0.0f);

        // Hann 窗（periodic）+ Σw²
        std::vector<float> win ((size_t) fft);
        for (int i = 0; i < fft; ++i)
            win[(size_t) i] = 0.5f - 0.5f * (float) std::cos (2.0 * juce::MathConstants<double>::pi * i / fft);
        std::vector<float> w2sum ((size_t) n44 + (size_t) fft, 0.0f);
        {
            const int Ftot = (n44 + hop - 1) / hop;
            for (int fr = 0; fr < Ftot; ++fr)
            {
                const size_t base = (size_t) fr * hop;
                for (int i = 0; i < fft; ++i)
                    if (base + i < w2sum.size()) w2sum[base + i] += win[(size_t) i] * win[(size_t) i];
            }
        }

        kiss_fftr_cfg fwd = kiss_fftr_alloc (fft, 0, nullptr, nullptr);
        kiss_fftr_cfg inv = kiss_fftr_alloc (fft, 1, nullptr, nullptr);
        // kissfft 归一化探测（与 SeparationEngine::RealFft 相同）
        double invScale = 1.0;
        {
            std::vector<float> x ((size_t) fft), t ((size_t) fft + 2);
            std::vector<kiss_fft_cpx> Fk ((size_t) bins);
            double si = 0.0, so = 0.0;
            for (int i = 0; i < fft; ++i) { x[(size_t) i] = (float) std::sin (0.37 * i); si += x[(size_t) i] * x[(size_t) i]; }
            kiss_fftr (fwd, x.data(), Fk.data());
            kiss_fftri (inv, Fk.data(), t.data());
            for (int i = 0; i < fft; ++i) { const double v = t[(size_t) i]; so += v * v; }
            invScale = (so > 0.0) ? std::sqrt (si / so) : 1.0;
        }

        const int Ftot = (n44 + hop - 1) / hop;
        const int numSeg = (Ftot + kFrames - 1) / kFrames;

        std::vector<kiss_fft_cpx> spec ((size_t) fft / 2 + 1);
        std::vector<float> frameBuf ((size_t) fft);
        std::vector<float> segOut ((size_t) fft + 2);

        // 人声主频区权重：140Hz~3.8kHz 保留，两端平滑衰减到 0.2（留底避免梳状噪声）
        std::vector<float> bandW ((size_t) bins, 1.0f);
        for (int k = 0; k < bins; ++k)
        {
            const double hz = (double) k * (double) kModelSr / (double) fft;
            const auto smoothStep = [] (double a, double b, double x)
            {
                const double t = juce::jlimit (0.0, 1.0, (x - a) / (b - a));
                return t * t * (3.0 - 2.0 * t);
            };
            const double lo = smoothStep (60.0, 160.0, hz);
            const double hi = 1.0 - smoothStep (3800.0, 7500.0, hz);
            bandW[(size_t) k] = (float) (0.20 + 0.80 * lo * hi);
        }

        // 每帧同时保留左右声道谱，供中置 / 两侧能量比使用
        std::vector<kiss_fft_cpx> fL ((size_t) kFrames * bins);
        std::vector<kiss_fft_cpx> fR ((size_t) kFrames * bins);
        std::vector<float> panEma ((size_t) bins, 0.5f);

        for (int seg = 0; seg < numSeg; ++seg)
        {
            if (cancelled && cancelled()) break;
            const int f0 = seg * kFrames;
            const int f1 = juce::jmin (f0 + kFrames, Ftot);

            // ---- 左右声道 STFT（同一窗 / 同一 FFT 网格）----
            for (int fi = f0; fi < f1; ++fi)
            {
                const int base = fi * hop;
                for (int i = 0; i < fft; ++i)
                {
                    const int idx = base + i;
                    frameBuf[(size_t) i] = (idx >= 0 && idx < n44)
                        ? mix44L[idx] * win[(size_t) i] : 0.0f;
                }
                kiss_fftr (fwd, frameBuf.data(), spec.data());
                std::memcpy (&fL[(size_t) (fi - f0) * bins], spec.data(),
                             sizeof (kiss_fft_cpx) * (size_t) bins);
                for (int i = 0; i < fft; ++i)
                {
                    const int idx = base + i;
                    frameBuf[(size_t) i] = (idx >= 0 && idx < n44)
                        ? mix44R[idx] * win[(size_t) i] : 0.0f;
                }
                kiss_fftr (fwd, frameBuf.data(), spec.data());
                std::memcpy (&fR[(size_t) (fi - f0) * bins], spec.data(),
                             sizeof (kiss_fft_cpx) * (size_t) bins);
            }

            // ---- 中置维纳掩码：M=(L+R)/2 人声为主，S=(L-R)/2 伴奏为主 ----
            //  pan = |M|² / (|M|²+|S|²)；平方锐化 + 逐频带 EMA（0.82）时间平滑。
            //  完全确定性、无模型推理，因此不会出现「伴奏/人声忽大忽小」。
            for (int fi = f0; fi < f1; ++fi)
            {
                const int lf = fi - f0;
                for (int k = 0; k < bins; ++k)
                {
                    const auto& L = fL[(size_t) lf * bins + k];
                    const auto& R = fR[(size_t) lf * bins + k];
                    const float mr = 0.5f * (L.r + R.r), mi = 0.5f * (L.i + R.i);
                    const float sr = 0.5f * (L.r - R.r), si = 0.5f * (L.i - R.i);
                    const double m2 = (double) mr * mr + (double) mi * mi;
                    const double s2 = (double) sr * sr + (double) si * si;
                    const float pan = (float) (m2 / (m2 + s2 + 1.0e-9));
                    float& e = panEma[(size_t) k];
                    e = 0.82f * e + 0.18f * (0.12f + 0.88f * pan * pan);
                    const float mask = juce::jlimit (0.05f, 1.0f, bandW[(size_t) k] * e);
                    spec[(size_t) k].r = mr * mask;
                    spec[(size_t) k].i = mi * mask;
                }
                kiss_fftri (inv, spec.data(), segOut.data());
                const size_t base = (size_t) fi * hop;
                for (int i = 0; i < fft; ++i)
                    if (base + i < (size_t) n44)
                    {
                        const float v = segOut[(size_t) i] * (float) invScale * win[(size_t) i];
                        voc44L[base + i] += v;
                        voc44R[base + i] += v;   // 人声居中：左右一致
                    }
            }

            // 归一化发布到段尾（本段最后帧覆盖到的样本）
            const int done44 = juce::jmin (n44, (f0 + kFrames) * hop);
            for (int i = (seg == 0 ? 0 : seg * kFrames * hop); i < done44; ++i)
            {
                const float nrm = w2sum[(size_t) i];
                if (nrm > 1.0e-6f)
                {
                    voc44L[(size_t) i] = juce::jlimit (-1.0f, 1.0f, voc44L[(size_t) i] / nrm);
                    voc44R[(size_t) i] = juce::jlimit (-1.0f, 1.0f, voc44R[(size_t) i] / nrm);
                }
                else { voc44L[(size_t) i] = 0.0f; voc44R[(size_t) i] = 0.0f; }
            }
            if (onSeg) onSeg (done44);   // onSeg 内已让出调度器，避免抢占宿主音频回调
        }

        kiss_fftr_free (fwd);
        kiss_fftr_free (inv);
    }
};

//==============================================================================
// HanceEngine —— 与 SeparationEngine 同接口的后台流式分离线程
//==============================================================================
class HanceEngine : private juce::Thread
{
public:
    enum Status { Idle = 0, Working = 1, Ready = 2, Failed = 3 };

    HanceEngine() : juce::Thread ("HanceEngine") {}
    ~HanceEngine() override { shutdown(); }

    void shutdown()
    {
        cancel();
        // 同上：有限超时，避免关闭程序时卡在后台推理线程上
        stopThread (4000);
    }

    // 消息线程：暂停 / 停止 / 切歌时立即放弃当前任务
    // Hance 首段 2 秒就翻 Ready，但后台仍会逐段推理完整首歌。
    // 暂停时必须按 progress 判定「仍在跑」并取消；已完成的结果则保留复用。
    void cancelPendingWork()
    {
        const auto s = getStatus();
        if (s == Idle || s == Failed) return;
        if (s == Working || getProgress() < 0.999f)
            cancel();
    }

    void cancel()
    {
        {
            const juce::ScopedLock sl (requestLock);
            // 与 start() 串行化，避免 start 的 false 覆盖 cancel 的 true
            cancelFlag.store (true, std::memory_order_relaxed);
            hasPending = false;
        }
        status.store (Idle, std::memory_order_release);
        readyTrackId.store (-1, std::memory_order_release);
        requestedTrackId.store (-1, std::memory_order_relaxed);
        progress.store (0.0f, std::memory_order_relaxed);
        notify();
    }

    void start (const juce::File& audioFile, double fileRateIn, int trackIdIn)
    {
        {
            const juce::ScopedLock sl (requestLock);
            cancelFlag.store (false, std::memory_order_relaxed);
            pendingFile = audioFile;
            pendingRate = fileRateIn;
            pendingTrackId = trackIdIn;
            hasPending = true;
        }
        // 同步翻状态，避免切歌瞬间音频线程读到上一曲的人声缓冲
        status.store (Working, std::memory_order_release);
        readyTrackId.store (-1, std::memory_order_release);
        requestedTrackId.store (trackIdIn, std::memory_order_relaxed);
        progress.store (0.0f, std::memory_order_relaxed);
        published.store (false, std::memory_order_release);
        notify();
        // 实时播放时这个线程会连续满速跑完整首歌（手写卷积网络，单线程）。
        // normal 优先级会和宿主音频线程抢核心，表现为「播几秒卡一秒」。
        if (! isThreadRunning()) startThread (juce::Thread::Priority::low);
    }

    int    getRequestedTrackId() const { return requestedTrackId.load (std::memory_order_relaxed); }
    Status getStatus() const        { return (Status) status.load (std::memory_order_acquire); }
    float  getProgress() const      { return progress.load (std::memory_order_relaxed); }
    juce::String getError() const   { return errorMsg; }

    bool isReadyFor (int expectedTrackId) const
    {
        return status.load (std::memory_order_acquire) == Ready
            && readyTrackId.load (std::memory_order_acquire) == expectedTrackId;
    }

    // ---- 不可变结果缓冲 ----
    // 逐段发布时 worker 持续写同一块缓冲，音频线程只按 pubLen 读已发布区间；
    // 新任务开始时整体替换 shared_ptr，音频线程持有的旧快照仍有效，
    // 不会因为 vector 扩容重分配而踩到已释放的堆块（爆音 / 堆损坏）。
    struct VocalData
    {
        std::vector<float> L, R;
    };

    struct VocalSnapshot
    {
        std::shared_ptr<const VocalData> buf;
        int length = 0;
        const float* L() const noexcept { return buf ? buf->L.data() : nullptr; }
        const float* R() const noexcept { return buf ? buf->R.data() : nullptr; }
    };

    VocalSnapshot snapshot() const
    {
        VocalSnapshot s;
        s.buf = std::atomic_load_explicit (&pub, std::memory_order_acquire);
        if (s.buf != nullptr)
        {
            const int cap = (int) juce::jmin (s.buf->L.size(), s.buf->R.size());
            // 先取缓冲再取长度：长度只会偏小，绝不会拿旧缓冲配新长度越界读
            s.length = juce::jmax (0, juce::jmin (pubLen.load (std::memory_order_acquire), cap));
        }
        return s;
    }

private:
    // 任务级取消判定：显式取消，或已被更新的曲目请求取代（快速切歌）。
    bool jobCancelled (int jobTrackId) const noexcept
    {
        return cancelFlag.load (std::memory_order_acquire)
            || jobTrackId != requestedTrackId.load (std::memory_order_acquire);
    }

    static void log (const juce::String& s)
    {
        juce::File ("D:\\Xiangmu\\S-Rack\\mp_debug.log").appendText (
            juce::Time::getCurrentTime().toString (false, true) + "  [HANCE] " + s + "\n");
    }

    static juce::File findWeightsFile()
    {
        const juce::File exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        juce::Array<juce::File> candidates {
            exe.getParentDirectory().getChildFile ("vocals_weights.bin"),
            exe.getParentDirectory().getChildFile ("models/hance/vocals_weights.bin"),
            juce::File ("D:/Xiangmu/S-Rack/Tools/vocals_weights.bin"),
        };
        for (const auto& f : candidates)
            if (f.existsAsFile()) return f;
        return juce::File();
    }

    void run() override
    {
        ::CoInitializeEx (nullptr, COINIT_MULTITHREADED);

        juce::File jobFile;
        double jobRate = 48000.0;
        int jobTrackId = -1;

        while (! threadShouldExit())
        {
            // 不能在持锁时 wait(-1)：cancel()/start() 需要先拿锁再 notify，会互锁，
            // 并且 stopThread() 唤不醒它（关闭程序卡死的元凶）。
            bool haveJob = false;
            {
                const juce::ScopedLock sl (requestLock);
                if (hasPending)
                {
                    jobFile = pendingFile;
                    jobRate = pendingRate;
                    jobTrackId = pendingTrackId;
                    hasPending = false;
                    haveJob = true;
                }
            }
            if (! haveJob)
            {
                wait (200);
                continue;
            }

            // 不要重置 cancelFlag：暂停请求可能在取走 job 之后到达
            if (jobCancelled (jobTrackId))
                continue;

            // 音频线程可能还持着上一曲缓冲指针，先翻状态再让一拍
            status.store (Working, std::memory_order_release);
            readyTrackId.store (-1, std::memory_order_release);
            progress.store (0.0f, std::memory_order_relaxed);
            pubLen.store (0, std::memory_order_release);
            published.store (false, std::memory_order_release);
            Thread::sleep (250);

            try { runJob (jobFile, jobRate, jobTrackId); }
            catch (const std::exception& e)
            {
                errorMsg = juce::String ("Hance 分离异常: ") + e.what();
                log (errorMsg);
                if (! jobCancelled (jobTrackId))
                    status.store (Failed, std::memory_order_release);
            }
        }

        ::CoUninitialize();
    }

    void runJob (const juce::File& audioFile, double fileRateIn, int jobTrackId)
    {
        // 实时人声估计已改为确定性的「中置声道 + 频域维纳掩码」DSP，
        // 不再依赖未标定的 Hance 复刻网络权重（旧路径的掩码近似「原混音×时变增益」，
        // Solo 时会把伴奏整段放出来并造成忽大忽小）。权重文件缺失也照常工作。
        if (! net.isLoaded())
            log ("realtime vocal estimator: center-pan DSP (no model weights needed)");

        // ---- 解码（与 SeparationEngine 相同的 EOF 安全模式）----
        juce::AudioFormatManager mgr;
        mgr.registerFormat (new MFAudioFormat(), false);
        mgr.registerBasicFormats();
        mgr.registerFormat (new juce::MP3AudioFormat(), false);

        std::unique_ptr<juce::AudioFormatReader> rd (mgr.createReaderFor (audioFile));
        if (rd == nullptr)
        {
            errorMsg = "无法打开音频文件用于 Hance 分离";
            status.store (Failed, std::memory_order_release);
            return;
        }
        const double inSr = rd->sampleRate;
        const int numCh = (int) rd->numChannels;
        const juce::int64 total = rd->lengthInSamples;
        auto* mfRd = dynamic_cast<MFAudioFormatReader*> (rd.get());

        std::vector<float> decL, decR;
        {
            const int blockN = (int) inSr;
            std::vector<float> tmpL ((size_t) blockN), tmpR ((size_t) blockN);
            juce::int64 pos = 0;
            const juce::int64 cap = (juce::int64) hance_cfg::kMaxUnknownSeconds * (juce::int64) inSr;
            while (! jobCancelled (jobTrackId))
            {
                const bool eof = (total > 0) ? (pos >= total)
                                             : (mfRd != nullptr && mfRd->isEof());
                if (eof || (total <= 0 && pos >= cap)) break;
                const int want = (int) ((total > 0) ? juce::jmin<juce::int64> (blockN, total - pos)
                                                    : juce::jmin<juce::int64> (blockN, cap - pos));
                float* chans[2] = { tmpL.data(), tmpR.data() };
                rd->read (chans, juce::jmin (2, numCh), pos, want);
                decL.insert (decL.end(), tmpL.begin(), tmpL.begin() + want);
                if (numCh > 1) decR.insert (decR.end(), tmpR.begin(), tmpR.begin() + want);
                else           decR.insert (decR.end(), tmpL.begin(), tmpL.begin() + want);
                pos += want;
            }
        }
        rd.reset();
        if (decL.empty() || jobCancelled (jobTrackId))
        {
            status.store (jobCancelled (jobTrackId) ? Idle : Failed,
                          std::memory_order_release);
            return;
        }

        // ---- 重采样到 44100 ----
        const double ratioIn = hance_cfg::kModelSr / inSr;
        std::vector<float> mix44L = resampleCR (decL.data(), decL.size(), ratioIn);
        std::vector<float> mix44R = resampleCR (decR.data(), decR.size(), ratioIn);
        decL.clear(); decR.clear(); decL.shrink_to_fit(); decR.shrink_to_fit();
        const int n44 = (int) mix44L.size();
        log (juce::String::formatted ("job begin: %.1fs, n44=%d", (double) n44 / hance_cfg::kModelSr, n44));

        // ---- 为本轮任务分配一块全新的不可变发布缓冲 ----
        ratioOut = fileRateIn / (double) hance_cfg::kModelSr;
        if (! (ratioOut > 0.0) || ! std::isfinite (ratioOut))
        {
            errorMsg = "文件采样率无效，无法进行实时分离";
            log (errorMsg);
            status.store (Failed, std::memory_order_release);
            return;
        }
        const juce::int64 nOutMax64 = (juce::int64) std::floor ((double) juce::jmax (0, n44 - 4) * ratioOut) + 8;
        const juce::int64 capOut = (juce::int64) hance_cfg::kMaxUnknownSeconds * (juce::int64) hance_cfg::kModelSr;
        nOutCap = (int) juce::jmax (0LL, juce::jmin (nOutMax64, capOut));
        const size_t need = (size_t) juce::jmax (1, nOutCap);
        // 旧实现在 worker 里 resize/assign 共享 vector，音频线程持有的裸指针
        // 会立刻悬空。改为每轮任务整体发布一块新缓冲，旧快照由 shared_ptr 兜住。
        auto buf = std::make_shared<VocalData>();
        buf->L.resize (need);          // vector 构造即零初始化
        buf->R.resize (need);
        workerBuf = buf;
        pubLen.store (0, std::memory_order_release);
        std::atomic_store_explicit (&pub,
            std::shared_ptr<const VocalData> (buf), std::memory_order_release);
        nextQ = 0;
        published.store (false, std::memory_order_release);

        // ---- 流式处理（逐段发布）----
        auto onSeg = [&] (int done44)
        {
            publishPrefix (done44, jobTrackId);
            progress.store (juce::jmin (1.0f, (float) done44 / (float) juce::jmax (1, n44)),
                            std::memory_order_relaxed);
            // 段间主动让出调度器：避免连续占满一个核心，把宿主音频回调饿死。
            juce::Thread::sleep (1);
        };
        HanceCore::processStream44 (net, mix44L.data(), mix44R.data(), n44,
                                    voc44L, voc44R, onSeg,
                                    [this, jobTrackId] { return jobCancelled (jobTrackId); });
        if (jobCancelled (jobTrackId)) { status.store (Idle, std::memory_order_release); return; }

        // ---- 尾部冲刷 ----
        publishPrefix (n44, jobTrackId);
        progress.store (1.0f, std::memory_order_relaxed);
        if (jobCancelled (jobTrackId))
        {
            status.store (Idle, std::memory_order_release);
            return;
        }
        // 客观质量指标：人声与原混音的相关系数 / 能量比。
        // 旧复刻网络输出几乎等于原混音，corr 会接近 1.0；确定性中置估计应明显低于它。
        {
            double dot = 0.0, vv = 0.0, mm = 0.0;
            const int step = juce::jmax (1, n44 / 20000);
            for (int i = 0; i < n44; i += step)
            {
                const double v = (double) voc44L[(size_t) i];
                const double m = (double) mix44L[(size_t) i];
                dot += v * m; vv += v * v; mm += m * m;
            }
            const double corr  = (vv > 0.0 && mm > 0.0) ? dot / std::sqrt (vv * mm) : 0.0;
            const double ratio = (mm > 0.0) ? std::sqrt (vv / mm) : 0.0;
            log (juce::String::formatted ("quality: corr=%.3f vocal/mix=%.3f", corr, ratio));
        }
        readyTrackId.store (jobTrackId, std::memory_order_release);
        status.store (Ready, std::memory_order_release);
        log (juce::String::formatted ("job done: pubLen=%d (%.1fs)", pubLen.load(),
                                      (double) pubLen.load() / fileRateIn));
    }

    // 把 voc44[0, done44) 中可重采样的部分发布到 pub 缓冲（仅 worker 调用）
    void publishPrefix (int done44, int jobTrackId)
    {
        // 旧 job 在切歌 / 暂停后可能还会跑完当前段，绝不能把它的缓冲再翻成 Ready，
        // 否则音频线程会读到上一曲的人声（Solo 听到伴奏、Mute 听到人声）。
        if (jobCancelled (jobTrackId))
            return;
        if (done44 < 3 || nOutCap <= 0) return;
        const auto buf = workerBuf;
        if (buf == nullptr || buf->L.size() < (size_t) nOutCap
            || buf->R.size() < (size_t) nOutCap)
            return;
        bool changed = false;
        while (nextQ < nOutCap)
        {
            const double pos = (double) nextQ / ratioOut;
            int i1 = (int) pos;
            if (i1 + 2 > done44 - 1) break;          // 需要 voc44[i1+2]
            const double t = pos - (double) i1;
            const int i0 = (i1 == 0) ? 0 : i1 - 1;
            const int i2 = i1 + 1;
            const int i3 = (i2 + 1 < done44) ? i2 + 1 : i2;
            for (int ch = 0; ch < 2; ++ch)
            {
                const std::vector<float>& v = (ch == 0) ? voc44L : voc44R;
                const float p0 = v[(size_t) i0], p1 = v[(size_t) i1], p2 = v[(size_t) i2], p3 = v[(size_t) i3];
                const float a = 0.5f * (-p0 + 3.0f * p1 - 3.0f * p2 + p3);
                const float b = 0.5f * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3);
                const float c = 0.5f * (p2 - p0);
                const float yv = 0.5f * (2.0f * p1 + c * (float) t + b * (float) (t * t) + a * (float) (t * t * t));
                (ch == 0 ? buf->L : buf->R)[(size_t) nextQ] = juce::jlimit (-1.0f, 1.0f, yv);
            }
            ++nextQ;
            changed = true;
        }
        if (changed)
        {
            if (jobCancelled (jobTrackId))
                return;
            pubLen.store (nextQ, std::memory_order_release);
            if (! published.exchange (true, std::memory_order_relaxed))
            {
                // 首段就绪：立即可用（实时分离的核心卖点）
                readyTrackId.store (jobTrackId, std::memory_order_release);
                status.store (Ready, std::memory_order_release);
                log ("first segment READY");
            }
        }
    }

    static std::vector<float> resampleCR (const float* x, size_t nIn, double ratio)
    {
        // nIn < 4 时 (nIn - 4) 是无符号下溢，会在构造 vector 时申请天文数字内存；
        // ratio=0/inf/NaN（采样率异常）同理。必须先校验再分配。
        if (x == nullptr || nIn < 4 || ! (ratio > 0.0) || ! std::isfinite (ratio))
            return {};
        const double outD = std::floor ((double) (nIn - 4) * ratio);
        constexpr double kMaxOut = 48000.0 * 60.0 * 60.0;
        if (! (outD > 0.0) || outD > kMaxOut)
            return {};
        const size_t nOut = (size_t) outD;
        std::vector<float> y (nOut, 0.0f);
        for (size_t i = 0; i < nOut; ++i)
        {
            const double pos = (double) i / ratio;
            size_t i1 = (size_t) pos;
            double t = pos - (double) i1;
            if (i1 >= nIn - 2) { i1 = nIn - 2; t = 0.0; }
            const size_t i0 = (i1 == 0) ? 0 : i1 - 1;
            const size_t i2 = i1 + 1;
            const size_t i3 = (i2 + 1 < nIn) ? i2 + 1 : i2;
            const float p0 = x[i0], p1 = x[i1], p2 = x[i2], p3 = x[i3];
            const float a = 0.5f * (-p0 + 3.0f * p1 - 3.0f * p2 + p3);
            const float b = 0.5f * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3);
            const float c = 0.5f * (p2 - p0);
            y[i] = 0.5f * (2.0f * p1 + c * (float) t + b * (float) (t * t) + a * (float) (t * t * t));
        }
        return y;
    }

    HanceNet net;
    std::vector<float> voc44L, voc44R;      // worker 内部 44.1k 人声
    std::shared_ptr<const VocalData> pub;   // 音频线程按 shared_ptr 快照读取
    std::shared_ptr<VocalData> workerBuf;   // 仅 worker 写，整轮任务固定
    std::atomic<int>   pubLen { 0 };
    std::atomic<bool>  published { false };
    int  nextQ = 0;
    int  nOutCap = 0;
    double ratioOut = 1.0;

    juce::CriticalSection requestLock;
    juce::File pendingFile;
    double pendingRate = 48000.0;
    int pendingTrackId = -1;
    bool hasPending = false;

    std::atomic<int>   readyTrackId { -1 };
    std::atomic<int>   requestedTrackId { -1 };
    std::atomic<int>   status { Idle };
    std::atomic<float> progress { 0.0f };
    std::atomic<bool>  cancelFlag { false };
    juce::String errorMsg;
};
