#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <cstring>
#include <algorithm>

//==============================================================================
// 实时 5 路音频分轨 v4.2（纯 DSP / 零外部依赖）
//
// v4.2 相对 v4.1 的改动（离线模型 build/verify_v4.js 验证，借鉴 Acon Digital
// Remix 的「时间上下文」思路——Remix 每轨一个带自注意力的独立模型，能跨帧
// 聚合上下文；我们用掩码时间中值滤波近似该效果）：
//   * 逐 bin 掩码时间中值滤波（±5 帧 = ±53ms，共 11 帧窗口）：对孤立误判帧
//     鲁棒——钢琴起音瞬间被误判的少数帧在窗口内被中值剔除。独奏鼓组的钢琴
//     泄漏 -38.0 -> -44.6 dB，独奏贝斯各项小改善，其余场景无回退。
//     实现：环形缓冲保存最近 11 帧的 IIR 平滑后 A/D 掩码（约 90KB），每帧
//     逐 bin 取中值作为实际输出掩码，E = D - A 保持重组代数一致。
//   * （实验性）每轨独立掩码模式在离线验证中为纯折衷、全面劣于 8 次幂竞争
//     掩码，未采用——Remix 的优势来自训练好的独立模型，而非「独立掩码」
//     这个形式本身。
//
// v4.1 相对 v3.1 的改动（离线模型 build/verify_v4.js 验证）：
//   * fftSize 1024 -> 2048（hop 固定 512，75% 重叠，OLA 补偿 olaComp=0.5）：
//     频率分辨率翻倍后贝斯/鼓组独奏串漏大幅下降（独奏贝斯的人声泄漏
//     -36 -> -45 dB，独奏鼓组的钢琴泄漏 -25 -> -38 dB），Mute 人声深度
//     -30.5 -> -33.1 dB；代价是计算量约 2 倍、处理延迟约 53 ms。
//   * 人声主频段下限 200 -> 160 Hz、下边沿 60 -> 30 Hz（覆盖男声基频，
//     贝斯谐波误判更少）；贝斯上边沿 28 -> 18 Hz（贝斯独奏自身电平
//     -7.6 -> -3.4 dB，Mute 人声时贝斯保真 -7.0 -> -3.1 dB）。
//   * beta 时间平滑 0.61/0.39 -> 0.45/0.55（平均掉人声×伴奏边带的旋转
//     交叉项，钢琴中置泄漏 -12.4 -> -11.4 dB）。
//
// v3.1 相对 v3 的两类改动（不改变 UI 行为与听感语义）：
//   * CPU：hop 256 -> 512（帧率 / FFT / 逐 bin 掩码计算量减半，COLA 仍为 1.0）；
//     交叉淡化从逐样本 sin/cos 改为查表；beta 限幅用平方比较避免每 bin 开方；
//     7 点局部峰度改为滑动窗口求和。
//   * 分离：鼓组掩码基底 0.25 -> 0.35，改善鼓组独奏时被过度衰减的问题。
//
// 【为什么要有 v3】旧版把「混合增益」直接乘在整段 M/S 频谱上，本质上是一条
// 随频率起伏的 EQ：人声和伴奏落在同一个频点时会被一起放行，所以 Solo 听起来
// 只是「伴奏小一点」、Mute 之后还剩一大片沉闷人声。
//
// v3 改成 Mid/Side 复数维纳消勘：
//   beta(k) = <M*conj(S)> / <|S|^2>     该频点上 M 与 S 的复数耦合（声像预测）
//   C(k)    = M - beta*S                仅「居中」成分（人声 / 贝斯 / 底鼓）
//   P(k)    = beta*S  ，以及 S 本身     「偏侧」成分（钢琴 / 镲片 / 其他）
//   a_i = 居中组竞争力份额， b_i = 偏侧组竞争力份额（8 次幂竞争，各自和为 1）
//   M_out = A*M + E*(beta*S) ,  S_out = D*S
//           A = sum(g_i*a_i), D = sum(g_i*b_i), E = D - A
//           （等价于 M_out = A*C + D*P：居中与偏侧两部分分别按各自增益重组）
//   Solo 人声 => A≈1、D≈0 => 输出只剩 C（偏侧伴奏被复数相消）
//   Mute 人声 => A≈0、D≈1 => M 被替换成 beta*S、S 原样保留（人声消失且不发闷）
//   中性（无 S/M）=> A=D=1、E=0 => 频谱逐点不变，配合 COLA 逐样本无损
//
// 竞争力还叠加了逐 bin 的频谱通量（瞬态→鼓组）与 7 点局部峰度（谐波→人声/钢琴），
// 掩码再经平方锐化与帧间平滑，避免硬切造成咔哒声和音乐噪声。
//
// 【零延迟直通】不点亮 S/M 时不跑任何 FFT：输出直接取当前输入样本（延迟 0，
// 位精确），只维护一条轻量历史/环形延迟线。点亮 / 熄灭 S 时用 fadeLen（2048）
// 样本的 equal-power 交叉淡化在「零延迟干声」与「约 2560 样本延迟的处理链」
// 之间过渡，因此纯听音乐既无色染、无延迟，也不承担 FFT 开销。
//==============================================================================
class StemSeparator
{
public:
    enum Stem { Vocals = 0, Piano, Bass, Drums, Other };
    static constexpr int numStems = 5;

    static constexpr int fftOrder = 11;
    static constexpr int fftSize  = 1 << fftOrder;   // 2048
    // v4.1：fftSize 1024→2048（频率分辨率 47→23.4 Hz/bin），hop 固定 512（75% 重叠）。
    // 更细的 bin 让居中乐器（钢琴/吉他）泛音与人声泛音的分类边界更清晰，
    // 贝斯/鼓组独奏串漏大幅下降；代价是 FFT 与逐 bin 掩码计算量约 2 倍、
    // 分析延迟 2048+512 样本 @48k 约 53 ms（媒体回放场景可接受）。
    // sqrt-Hann² 在 75% 重叠下叠加和为 2，两条 OLA 路径均乘 olaComp 补偿。
    static constexpr int hopSize  = 512;             // 帧移固定 512（帧率与 v3.1 一致）
    static constexpr int numBins  = fftSize / 2;     // 1024
    static constexpr int fadeLen  = fftSize;         // 交叉淡化 2048 样本 @48k 约 43 ms
    static constexpr float olaComp = 2.0f * (float) hopSize / (float) fftSize;  // 0.5

    // ---- UI 参数（消息线程写 / 音频线程读）----
    // 外置人声模式：人声已由 MDX-Net 模型单独分离输出，本 DSP 只处理
    // 伴奏总线上的钢琴/贝斯/鼓组/其他。人声轨的竞争力被置零（掩码竞争中
    // 永远不出现人声），isActive() 也忽略人声轨的 UI 状态；人声的增益/
    // 静音/独奏/电平表由调用方在 DSP 之外处理。
    std::atomic<bool> externalVocals { false };
    std::array<std::atomic<float>, numStems> gainDb {};        // -60 .. +10 dB
    std::array<std::atomic<float>, numStems> sensitivity {};   // -100 .. +100 %
    std::array<std::atomic<bool>,  numStems> mute {};
    std::array<std::atomic<bool>,  numStems> solo {};

    // ---- 电平表（音频线程写 / UI 只读）----
    // 0..1 对应 -60..0 dBFS；meterPeak 为慢落峰值保持
    std::array<std::atomic<float>, numStems> meterLevel {};
    std::array<std::atomic<float>, numStems> meterPeak {};

    StemSeparator()
    {
        for (int i = 0; i < numStems; ++i)
        {
            gainDb[(size_t) i].store (0.0f, std::memory_order_relaxed);
            sensitivity[(size_t) i].store (0.0f, std::memory_order_relaxed);
            mute[(size_t) i].store (false, std::memory_order_relaxed);
            solo[(size_t) i].store (false, std::memory_order_relaxed);
            meterLevel[(size_t) i].store (0.0f, std::memory_order_relaxed);
            meterPeak[(size_t) i].store (0.0f, std::memory_order_relaxed);
        }

        const double twoPi = juce::MathConstants<double>::twoPi;
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = std::sqrt (0.5f - 0.5f
                * (float) std::cos (twoPi * (double) i / (double) fftSize));

        // equal-power 交叉淡化权重表（0..fadeLen），逐样本直接查表
        for (int i = 0; i <= fadeLen; ++i)
        {
            const float x = (float) i / (float) fadeLen * halfPi;
            fadeWet[(size_t) i] = std::sin (x);
            fadeDry[(size_t) i] = std::cos (x);
        }

        buildFrequencyTables();
        reset();
    }

    // 是否进入「分离处理」。全部 S 熄灭且无 M / 推子 / 灵敏度 = 原声直通。
    bool isActive() const
    {
        const bool extVoc = externalVocals.load (std::memory_order_relaxed);
        for (int i = 0; i < numStems; ++i)
        {
            if (extVoc && i == Vocals) continue;
            if (std::abs (gainDb[(size_t) i].load (std::memory_order_relaxed)) > 0.01f) return true;
            if (std::abs (sensitivity[(size_t) i].load (std::memory_order_relaxed)) > 0.01f) return true;
            if (mute[(size_t) i].load (std::memory_order_relaxed)) return true;
            if (solo[(size_t) i].load (std::memory_order_relaxed)) return true;
        }
        return false;
    }

    void prepare (double sampleRate, int)
    {
        sr = (float) sampleRate;
        buildFrequencyTables();
        reset();
    }

    void reset()
    {
        histL.fill (0.0f);       histR.fill (0.0f);
        olaL.fill (0.0f);        olaR.fill (0.0f);
        hopBufL.fill (0.0f);     hopBufR.fill (0.0f);
        outFifoL.fill (0.0f);    outFifoR.fill (0.0f);
        fftM.fill (0.0f);        fftS.fill (0.0f);

        smoothA.fill (1.0f);     smoothD.fill (1.0f);
        for (auto& fr : medHistA) fr.fill (1.0f);
        for (auto& fr : medHistD) fr.fill (1.0f);
        medPos = 0; medFill = 0;
        centerBuf.fill (0.5f);   centerSmooth.fill (0.5f);
        magBuf.fill (0.0f);      prevMag.fill (0.0f);
        fluxSmooth.fill (0.0f);  peaky.fill (0.0f);
        powSBuf.fill (0.0f);
        betaRe.fill (0.0f);      betaIm.fill (0.0f);
        betaSRe.fill (0.0f);     betaSIm.fill (0.0f);

        hopCount = 0;
        outRead  = 0;
        outWrite = 0;
        cross = 0.0f;
        wasProcessing = false;
        meterSmooth.fill (0.0f);
        meterHold.fill (0.0f);
        for (int i = 0; i < numStems; ++i)
        {
            meterLevel[(size_t) i].store (0.0f, std::memory_order_relaxed);
            meterPeak[(size_t) i].store (0.0f, std::memory_order_relaxed);
        }

        // 预跑一帧（输入全零）把输出 FIFO 的第一个 hop 填上，使读指针恒落后
        // 写指针 1 个 hop。否则每个 hop 里只有 1 个非零，会断续静音。
        processFrame (false);
    }

    // 原地处理一对立体声通道。
    //   - 未激活：零延迟位精确直通，不执行 FFT / 掩码 / 逆变换
    //   - 已激活：约 2560 样本延迟的 STFT 处理链
    //   - 切换：fadeLen 个样本的 equal-power 交叉淡化
    void process (float* L, float* R, int n)
    {
        if (L == nullptr || R == nullptr || sr <= 0.0f) return;

        const bool target  = isActive();
        const bool needFFT = target || cross > 0.0f;   // 淡出期间也要跑，保证 FIFO 连续
        constexpr float fadeStep = 1.0f / (float) fadeLen;

        // 刚进入处理链：清掉叠接累加器与频域状态，避免上一次处理留下的旧帧叠进
        // 新信号。cross 会在 fadeLen 内 0->1，正好与 OLA 的 2 帧收敛同步。
        if (needFFT && ! wasProcessing)
        {
            // ola 累加器由 bypassFrame() 用「零 FFT 的等价 OLA」持续维护，
            // 这里绝不能清空，否则淡入瞬间会掉 1/4 幅度。只重置递推状态。
            smoothA.fill (1.0f);   smoothD.fill (1.0f);
            for (auto& fr : medHistA) fr.fill (1.0f);
            for (auto& fr : medHistD) fr.fill (1.0f);
            medPos = 0; medFill = 0;
            betaRe.fill (0.0f);    betaIm.fill (0.0f);
            prevMag.fill (0.0f);   fluxSmooth.fill (0.0f);
            wasProcessing = true;
        }
        else if (! needFFT && wasProcessing)
        {
            wasProcessing = false;
        }

        for (int i = 0; i < n; ++i)
        {
            const float xL = L[i], xR = R[i];

            hopBufL[(size_t) hopCount] = xL;
            hopBufR[(size_t) hopCount] = xR;
            if (++hopCount == hopSize)
            {
                shiftHistory();
                if (needFFT) processFrame (target);
                else         bypassFrame();
                hopCount = 0;
            }

            // outFifo 恒为「对齐到 t-(fftSize+hop)」的信号：处理链输出或零 FFT 的对齐干声
            const float wetL = outFifoL[(size_t) outRead];
            const float wetR = outFifoR[(size_t) outRead];
            outFifoL[(size_t) outRead] = 0.0f;
            outFifoR[(size_t) outRead] = 0.0f;
            outRead = (outRead + 1) % fifoSize;

            if (cross <= 0.0f && ! target)
            {
                L[i] = xL;      // 真·零延迟、位精确
                R[i] = xR;
            }
            else
            {
                // 交叉淡化权重查表：避免每个样本都调用 sin/cos（稳态时 cross 恒为
                // 0 或 1，原实现对每个样本仍各算一次三角函数）。
                const int fi = juce::jlimit (0, fadeLen, (int) (cross * (float) fadeLen));
                const float wWet = fadeWet[(size_t) fi];
                const float wDry = fadeDry[(size_t) fi];
                L[i] = wDry * xL + wWet * wetL;
                R[i] = wDry * xR + wWet * wetR;
            }

            if (target) cross = juce::jmin (1.0f, cross + fadeStep);
            else        cross = juce::jmax (0.0f, cross - fadeStep);
        }
    }

private:
    static constexpr int fifoSize  = fftSize;
    static constexpr int tableSize = numBins + 1;

    // 数值常量（与 build/verify_v4.js 的离线模型逐项对齐；v4 相对 v3 的改动：
    // 人声主频段 200→160Hz、下边沿 60→30Hz（覆盖男声基频、贝斯谐波误判更少）、
    // 贝斯上边沿 28→18Hz（贝斯独奏自身电平 -7.6→-3.4dB）、beta 时间平滑
    // 0.39→0.75（更快跟踪声像耦合，钢琴中置泄漏 -12.4→-11.1dB）。
    static constexpr float floorGain = 0.004f;     // -48 dB 残留底
    static constexpr float betaMax   = 2.0f;       // 声像预测系数上限
    static constexpr float betaMax2  = betaMax * betaMax;
    static constexpr float halfPi    = 1.5707963f;
    static constexpr int   medN      = 5;          // 掩码时间中值半窗宽（帧）
    static constexpr int   medLen    = 2 * medN + 1;   // 11 帧 ≈ ±53ms @hop512/48k

    juce::dsp::FFT fft { fftOrder };
    float sr = 48000.0f;

    std::array<float, fftSize>      window {}, histL {}, histR {}, olaL {}, olaR {};
    std::array<float, fadeLen + 1>  fadeWet {}, fadeDry {};
    std::array<float, hopSize>      hopBufL {}, hopBufR {};
    std::array<float, fifoSize>     outFifoL {}, outFifoR {};
    std::array<float, fftSize * 2>  fftM {}, fftS {};

    std::array<float, tableSize> bBass {}, bDrumLo {}, bDrumMid {}, bDrumHi {},
                                 bVoc {}, bPiano {}, bHigh {};
    std::array<float, tableSize> centerBuf {}, centerSmooth {},
                                 smoothA {}, smoothD {};
    // 掩码时间中值滤波：最近 medLen 帧的 IIR 平滑后 A/D 掩码环形历史
    std::array<std::array<float, tableSize>, medLen> medHistA {}, medHistD {};
    int medPos = 0, medFill = 0;
    std::array<float, tableSize> magBuf {}, prevMag {}, fluxSmooth {}, peaky {},
                                 powSBuf {};
    std::array<float, tableSize> betaRe {}, betaIm {}, betaSRe {}, betaSIm {};

    int hopCount = 0, outRead = 0, outWrite = 0;
    float cross = 0.0f;                  // 交叉淡化进度 0=零延迟直通 / 1=处理链
    bool  wasProcessing = false;
    std::array<float, numStems> meterSmooth {}, meterHold {};

    // exp 参数限幅，避免高频端 exp(±800) 溢出成 inf
    static float smoothBand (float f, float lo, float hi, float edge)
    {
        const float ea = juce::jlimit (-40.0f, 40.0f, -(f - lo) / edge);
        const float eb = juce::jlimit (-40.0f, 40.0f, -(hi - f) / edge);
        const float a = 1.0f / (1.0f + (float) std::exp ((double) ea));
        const float b = 1.0f / (1.0f + (float) std::exp ((double) eb));
        return a * b;
    }

    // x<=1 时的 8 次幂（三次平方，无 powf）
    static inline float pow8 (float x) noexcept
    {
        const float x2 = x * x;
        const float x4 = x2 * x2;
        return x4 * x4;
    }

    void buildFrequencyTables()
    {
        for (int k = 0; k <= numBins; ++k)
        {
            const float f = (float) k * sr / (float) fftSize;
            bBass[(size_t) k]    = smoothBand (f, 20.0f,    170.0f,    18.0f);
            bDrumLo[(size_t) k]  = smoothBand (f, 30.0f,    150.0f,    30.0f);
            bDrumMid[(size_t) k] = smoothBand (f, 150.0f,   900.0f,   180.0f);
            bDrumHi[(size_t) k]  = smoothBand (f, 2200.0f, 12000.0f, 1500.0f);
            bVoc[(size_t) k]     = 0.95f * smoothBand (f, 160.0f, 4200.0f,  30.0f)
                                 + 0.30f * smoothBand (f, 3200.0f, 9000.0f, 1200.0f);
            bPiano[(size_t) k]   = smoothBand (f, 160.0f,   5500.0f,  300.0f);
            bHigh[(size_t) k]    = smoothBand (f, 6500.0f, 20000.0f, 2000.0f);
        }
    }

    void shiftHistory()
    {
        std::memmove (histL.data(), histL.data() + hopSize, (size_t) (fftSize - hopSize) * sizeof (float));
        std::memmove (histR.data(), histR.data() + hopSize, (size_t) (fftSize - hopSize) * sizeof (float));
        std::memcpy  (histL.data() + fftSize - hopSize, hopBufL.data(), (size_t) hopSize * sizeof (float));
        std::memcpy  (histR.data() + fftSize - hopSize, hopBufR.data(), (size_t) hopSize * sizeof (float));
    }

    float computeFrameRms() const
    {
        float p = 0.0f;
        for (int i = 0; i < hopSize; ++i)
            p += hopBufL[(size_t) i] * hopBufL[(size_t) i]
               + hopBufR[(size_t) i] * hopBufR[(size_t) i];
        return std::sqrt (p / (float) (2 * hopSize));
    }

    void pushMeters (const float* share, float frameRms)
    {
        for (int i = 0; i < numStems; ++i)
        {
            const float amp    = frameRms * std::sqrt (juce::jmax (0.0f, share[i]));
            const float db     = 20.0f * std::log10 (amp + 1.0e-6f);
            const float target = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);

            const float cur  = meterSmooth[(size_t) i];
            const float next = target > cur ? cur + 0.55f * (target - cur)
                                            : cur + 0.12f * (target - cur);
            meterSmooth[(size_t) i] = next;

            float hold = meterHold[(size_t) i];
            hold = (next > hold) ? next : juce::jmax (next, hold - 0.007f);
            meterHold[(size_t) i] = hold;

            meterLevel[(size_t) i].store (next, std::memory_order_relaxed);
            meterPeak[(size_t) i].store  (hold, std::memory_order_relaxed);
        }
    }

    // 零 FFT 的一帧。中性时 IFFT(FFT(hist*w))*w*olaComp 恒等于 hist*w*w*olaComp，
    // 所以这里直接用窗平方在时域做叠接累加，与处理链的 OLA 逐样本等价。
    // 这样旁路 / 处理两条路径共用同一个连续累加器，切换不会掉幅度或爆音。
    // 叠加和 sqrt(hop) 归一：olaComp = 2*hop/fftSize（2048/512=75% 重叠时为 0.5，
    // 1024/512=50% 重叠时为 1.0），两条路径共用同一补偿。
    void bypassFrame()
    {
        for (int j = 0; j < fftSize; ++j)
        {
            const float w2 = window[(size_t) j] * window[(size_t) j];
            olaL[(size_t) j] += histL[(size_t) j] * w2 * olaComp;
            olaR[(size_t) j] += histR[(size_t) j] * w2 * olaComp;
        }

        for (int j = 0; j < hopSize; ++j)
        {
            outFifoL[(size_t) outWrite] = olaL[(size_t) j];
            outFifoR[(size_t) outWrite] = olaR[(size_t) j];
            outWrite = (outWrite + 1) % fifoSize;
        }

        std::memmove (olaL.data(), olaL.data() + hopSize, (size_t) (fftSize - hopSize) * sizeof (float));
        std::memmove (olaR.data(), olaR.data() + hopSize, (size_t) (fftSize - hopSize) * sizeof (float));
        std::fill (olaL.begin() + (fftSize - hopSize), olaL.end(), 0.0f);
        std::fill (olaR.begin() + (fftSize - hopSize), olaR.end(), 0.0f);

        const float sh[numStems] = { 0.28f, 0.24f, 0.18f, 0.18f, 0.12f };
        pushMeters (sh, computeFrameRms());
    }

    //--------------------------------------------------------------------------
    // 完整 STFT 帧。apply=false 用于淡出：不改动频谱，只把状态收敛回中性，
    // 使 wet 平滑变回干声，再与零延迟干声交叉淡化。
    void processFrame (bool apply)
    {
        // 1) 分析窗 -> Mid / Side -> 正变换
        // JUCE real-only FFT 的输入必须是数组【前 fftSize 个连续实数】；变换后
        // 这 2*fftSize 个 float 才被解释为 fftSize 个交错复数 bin，逆变换只使用
        // 前 fftSize/2+1 个（非负频率）。按交错方式写样本会降八度 / 折叠镜像。
        for (int i = 0; i < fftSize; ++i)
        {
            const float w = window[(size_t) i];
            const float l = histL[(size_t) i] * w;
            const float r = histR[(size_t) i] * w;
            fftM[(size_t) i] = (l + r) * 0.5f;
            fftS[(size_t) i] = (l - r) * 0.5f;
        }
        fft.performRealOnlyForwardTransform (fftM.data());
        fft.performRealOnlyForwardTransform (fftS.data());

        if (apply)
        {
            applySeparationMasks();
        }
        else
        {
            for (int k = 0; k <= numBins; ++k)
            {
                smoothA[(size_t) k] = 0.30f * smoothA[(size_t) k] + 0.70f;
                smoothD[(size_t) k] = 0.30f * smoothD[(size_t) k] + 0.70f;
                medHistA[(size_t) medPos][(size_t) k] = smoothA[(size_t) k];
                medHistD[(size_t) medPos][(size_t) k] = smoothD[(size_t) k];
                betaRe[(size_t) k]     *= 0.45f;
                betaIm[(size_t) k]     *= 0.45f;
                prevMag[(size_t) k]    *= 0.61f;
                fluxSmooth[(size_t) k] *= 0.61f;
            }
            medPos = (medPos + 1) % medLen;
            medFill = juce::jmin (medFill + 1, medLen);
            const float sh[numStems] = { 0.28f, 0.24f, 0.18f, 0.18f, 0.12f };
            pushMeters (sh, computeFrameRms());
        }

        // 2) 逆变换（JUCE 内部已做 1/N 归一化）-> 综合窗 -> 叠接累加
        fft.performRealOnlyInverseTransform (fftM.data());
        fft.performRealOnlyInverseTransform (fftS.data());

        for (int i = 0; i < fftSize; ++i)
        {
            const float w = window[(size_t) i];   // 75% 重叠下 sqrt-Hann² 叠加和为 2
            const float m = fftM[(size_t) i] * w * olaComp;
            const float s = fftS[(size_t) i] * w * olaComp;
            olaL[(size_t) i] += m + s;
            olaR[(size_t) i] += m - s;
        }

        for (int i = 0; i < hopSize; ++i)
        {
            outFifoL[(size_t) outWrite] = olaL[(size_t) i];
            outFifoR[(size_t) outWrite] = olaR[(size_t) i];
            outWrite = (outWrite + 1) % fifoSize;
        }

        std::memmove (olaL.data(), olaL.data() + hopSize, (size_t) (fftSize - hopSize) * sizeof (float));
        std::memmove (olaR.data(), olaR.data() + hopSize, (size_t) (fftSize - hopSize) * sizeof (float));
        std::fill (olaL.begin() + (fftSize - hopSize), olaL.end(), 0.0f);
        std::fill (olaR.begin() + (fftSize - hopSize), olaR.end(), 0.0f);
    }

    //--------------------------------------------------------------------------
    void applySeparationMasks()
    {
        // ---- pass 1：逐 bin 基础统计（功率 / 中心度 / 通量 / 声像耦合）----
        for (int k = 0; k <= numBins; ++k)
        {
            const float mr = fftM[(size_t) (2 * k)],  mi = fftM[(size_t) (2 * k + 1)];
            const float sr = fftS[(size_t) (2 * k)],  si = fftS[(size_t) (2 * k + 1)];
            const float powM = mr * mr + mi * mi;
            const float powS = sr * sr + si * si;
            powSBuf[(size_t) k] = powS;

            const float mag = std::sqrt (powM + powS);
            magBuf[(size_t) k] = mag;
            const float am = std::sqrt (powM), as = std::sqrt (powS);
            centerBuf[(size_t) k] = am / (am + as + 1.0e-6f);

            const float fl = juce::jmax (0.0f,
                (mag - prevMag[(size_t) k]) / (mag + prevMag[(size_t) k] + 1.0e-6f));
            fluxSmooth[(size_t) k] = 0.36f * fluxSmooth[(size_t) k] + 0.64f * fl;
            prevMag[(size_t) k] = mag;

            // beta = M*conj(S) / (|S|^2 + 0.05(|M|^2+|S|^2))，复数最小二乘声像预测。
            // 分母里的泄漏项保证 S->0（纯居中）时 beta 不会爆掉。
            const float den = powS + 0.05f * (powM + powS) + 1.0e-9f;
            float br = (mr * sr + mi * si) / den;
            float bi = (mi * sr - mr * si) / den;
            // 用平方比较做限幅，只有在确实超过上限时才开方
            const float bm2 = br * br + bi * bi;
            if (bm2 > betaMax2)
            {
                const float s = betaMax / std::sqrt (bm2 + 1.0e-9f);
                br *= s; bi *= s;
            }
            betaRe[(size_t) k] = 0.45f * betaRe[(size_t) k] + 0.55f * br;
            betaIm[(size_t) k] = 0.45f * betaIm[(size_t) k] + 0.55f * bi;
        }

        // ---- pass 2：频率方向 3 点平滑 + 7 点局部峰度（滑动窗口求和）----
        float acc7 = 0.0f;
        for (int k = 0; k <= numBins; ++k)
        {
            const int km = juce::jmax (0, k - 1);
            const int kp = juce::jmin (numBins, k + 1);

            centerSmooth[(size_t) k] = (k == 0 || k == numBins)
                ? centerBuf[(size_t) k]
                : 0.25f * centerBuf[(size_t) km] + 0.50f * centerBuf[(size_t) k]
                + 0.25f * centerBuf[(size_t) kp];

            betaSRe[(size_t) k] = 0.25f * betaRe[(size_t) km] + 0.50f * betaRe[(size_t) k]
                                + 0.25f * betaRe[(size_t) kp];
            betaSIm[(size_t) k] = 0.25f * betaIm[(size_t) km] + 0.50f * betaIm[(size_t) k]
                                + 0.25f * betaIm[(size_t) kp];
            if (k == 0 || k == numBins) betaSIm[(size_t) k] = 0.0f;   // DC / Nyquist 为实数 bin

            // 7 点局部均值：中间 bin 用 O(1) 滑动求和，仅首尾各 3 个 bin 直接累加
            // （与原来的夹取重复边界逐项一致），避免每 bin 重复 7 次加法。
            if (k <= numBins - 3)
            {
                if (k < 3)
                {
                    acc7 = 0.0f;
                    for (int j = -3; j <= 3; ++j)
                        acc7 += magBuf[(size_t) juce::jlimit (0, numBins, k + j)];
                }
                else if (k == 3)
                {
                    acc7 = magBuf[0] + magBuf[1] + magBuf[2] + magBuf[3]
                         + magBuf[4] + magBuf[5] + magBuf[6];
                }
                else
                {
                    acc7 += magBuf[(size_t) (k + 3)] - magBuf[(size_t) (k - 4)];
                }
            }
            else
            {
                acc7 = 0.0f;
                for (int j = -3; j <= 3; ++j)
                    acc7 += magBuf[(size_t) juce::jlimit (0, numBins, k + j)];
            }
            const float loc = acc7 * (1.0f / 7.0f) + 1.0e-6f;
            peaky[(size_t) k] = 0.5f * juce::jlimit (0.0f, 2.0f,
                                    magBuf[(size_t) k] / loc - 1.0f);
        }

        // ---- 参数快照（每帧只读一次 atomic）----
        bool anySolo = false;
        for (int i = 0; i < numStems; ++i)
            if (solo[(size_t) i].load (std::memory_order_relaxed)) anySolo = true;

        float g[numStems], sens[numStems];
        for (int i = 0; i < numStems; ++i)
        {
            const bool on = anySolo ? solo[(size_t) i].load (std::memory_order_relaxed)
                                    : ! mute[(size_t) i].load (std::memory_order_relaxed);
            const float db = gainDb[(size_t) i].load (std::memory_order_relaxed);
            g[i] = on ? std::pow (10.0f, db * 0.05f) : 0.0f;

            const float pct = juce::jlimit (-1.0f, 1.0f,
                sensitivity[(size_t) i].load (std::memory_order_relaxed) / 100.0f);
            sens[i] = std::exp (pct * 1.4f);
        }

        // 居中 / 偏侧两组的声像偏好（顺序：人声 钢琴 贝斯 鼓组 其他）
        const float cBias[numStems] = { externalVocals.load (std::memory_order_relaxed) ? 0.0f : 1.00f,
                                        0.05f, 0.25f, 0.30f, 0.05f };
        const float sBias[numStems] = { externalVocals.load (std::memory_order_relaxed) ? 0.0f : 0.04f,
                                        1.00f, 0.06f, 0.80f, 1.00f };

        // ---- pass 3：逐 bin 掩码 -> 重组 M / S 频谱，并统计各路电平能量 ----
        float stemPow[numStems] = {};
        float totalPow = 0.0f;

        for (int k = 0; k <= numBins; ++k)
        {
            const float c   = centerSmooth[(size_t) k];
            const float sd  = 1.0f - c;
            const float ton = 1.0f - fluxSmooth[(size_t) k];
            const float prc = fluxSmooth[(size_t) k];
            const float pk  = peaky[(size_t) k];

            float w[numStems];
            w[Vocals] = (0.08f + 1.55f * bVoc[(size_t) k])    * (0.20f + 0.80f * c)
                      * (0.30f + 0.70f * ton) * (0.60f + 0.80f * pk);
            w[Piano]  = (0.08f + 1.15f * bPiano[(size_t) k])  * (0.30f + 0.70f * sd)
                      * (0.30f + 0.70f * ton) * (0.70f + 0.60f * pk);
            w[Bass]   = (0.08f + 1.50f * bBass[(size_t) k])   * (0.50f + 0.50f * c)
                      * (0.35f + 0.65f * ton);
            // 鼓组：把「基底」从 0.25 提到 0.35。离线对照中鼓组独奏电平由
            // 约 -6.4 dB 提升到约 -5.3 dB（取 0.40 时可到 -4.6 dB），
            // 而人声 Mute 残余只变差约 0.7 dB，整体收益大于代价。
            w[Drums]  = (0.06f + 1.25f * bDrumLo[(size_t) k] + 1.35f * bDrumHi[(size_t) k])
                      * (0.35f + 0.65f * prc)
                      + 0.55f * bDrumMid[(size_t) k] * prc + 0.10f * prc;
            w[Other]  = (0.05f + 1.25f * bHigh[(size_t) k])   * (0.25f + 0.75f * sd)
                      * (0.45f + 0.55f * ton);

            float wcMax = 1.0e-9f, wsMax = 1.0e-9f;
            for (int i = 0; i < numStems; ++i)
            {
                w[i] = juce::jmax (w[i], 1.0e-5f);
                wcMax = juce::jmax (wcMax, w[i] * cBias[i]);
                wsMax = juce::jmax (wsMax, w[i] * sBias[i]);
            }

            // 先按组内最大值归一，再取 8 次幂：避免 float 下溢，且让主导轨赢者通吃
            float cwM[numStems], cwS[numStems], sumM = 0.0f, sumS = 0.0f;
            for (int i = 0; i < numStems; ++i)
            {
                cwM[i] = pow8 (juce::jmin (1.0f, w[i] * cBias[i] / wcMax)) * sens[i];
                cwS[i] = pow8 (juce::jmin (1.0f, w[i] * sBias[i] / wsMax)) * sens[i];
                sumM += cwM[i];
                sumS += cwS[i];
            }
            sumM = juce::jmax (sumM, 1.0e-12f);
            sumS = juce::jmax (sumS, 1.0e-12f);

            const float mr = fftM[(size_t) (2 * k)],  mi = fftM[(size_t) (2 * k + 1)];
            const float sr = fftS[(size_t) (2 * k)],  si = fftS[(size_t) (2 * k + 1)];
            const float powS = powSBuf[(size_t) k];

            // P = beta*S（偏侧成分在 M 中的复数映射）
            const float bre = betaSRe[(size_t) k], bim = betaSIm[(size_t) k];
            const float pRe = bre * sr - bim * si;
            const float pIm = bre * si + bim * sr;
            const float cr = mr - pRe, ci = mi - pIm;      // C = M - beta*S
            const float powC = cr * cr + ci * ci;
            totalPow += powC + powS;

            float Araw = 0.0f, Draw = 0.0f;
            for (int i = 0; i < numStems; ++i)
            {
                const float ai = cwM[i] / sumM;
                const float bi = cwS[i] / sumS;
                Araw += ai * g[i];
                Draw += bi * g[i];
                stemPow[i] += (ai * powC + bi * powS) * g[i] * g[i];
            }

            // 平方锐化（gamma=2）：把「半透明」的中间份额压下去，Solo 更干净、
            // Mute 更彻底；中性时 Araw=Draw=1 => 结果不变，不破坏无损。
            float A = floorGain + (1.0f - floorGain) * Araw * Araw;
            float D = floorGain + (1.0f - floorGain) * Draw * Draw;

            // 帧间平滑：掩码逐帧缓变，避免点亮 / 熄灭 S 时产生咔哒声。
            smoothA[(size_t) k] = 0.30f * smoothA[(size_t) k] + 0.70f * A;
            smoothD[(size_t) k] = 0.30f * smoothD[(size_t) k] + 0.70f * D;

            // v4.2 掩码时间中值滤波：把当前帧的平滑掩码写入环形历史，再对
            // 最近 medLen 帧（含当前）逐 bin 取中值作为实际输出掩码。中值对
            // 孤立误判帧鲁棒，且不产生 IIR 的拖尾（借鉴 Remix 时间上下文）。
            medHistA[(size_t) medPos][(size_t) k] = smoothA[(size_t) k];
            medHistD[(size_t) medPos][(size_t) k] = smoothD[(size_t) k];

            float va[medLen], vd[medLen];
            const int mCount = juce::jmax (1, medFill);   // 首帧至少含当前帧
            for (int m = 0; m < mCount; ++m)
            {
                const int idx = (medPos + medLen - m) % medLen;   // 由近及远
                va[m] = medHistA[(size_t) idx][(size_t) k];
                vd[m] = medHistD[(size_t) idx][(size_t) k];
            }
            for (int a = 1; a < mCount; ++a)   // 小窗口插入排序（两个数组同步）
            {
                const float x = va[a]; int b = a - 1;
                while (b >= 0 && va[b] > x) { va[b + 1] = va[b]; --b; }
                va[b + 1] = x;
                const float y = vd[a]; b = a - 1;
                while (b >= 0 && vd[b] > y) { vd[b + 1] = vd[b]; --b; }
                vd[b + 1] = y;
            }
            const float a = va[mCount >> 1];
            const float d = vd[mCount >> 1];
            const float e = d - a;   // 中值后重算，保持 M_out = A*C + D*P 代数一致

            // M_out = A*M + E*P   （= A*C + D*P） ;  S_out = D*S
            fftM[(size_t) (2 * k)]     = a * mr + e * pRe;
            fftM[(size_t) (2 * k + 1)] = a * mi + e * pIm;
            fftS[(size_t) (2 * k)]     = d * sr;
            fftS[(size_t) (2 * k + 1)] = d * si;
        }

        // 中值窗口推进（当前帧已写入 medPos 槽，滑动到下一帧）
        medPos = (medPos + 1) % medLen;
        medFill = juce::jmin (medFill + 1, medLen);

        float share[numStems];
        const float invTotal = 1.0f / (totalPow + 1.0e-9f);
        for (int i = 0; i < numStems; ++i)
            share[i] = juce::jmax (0.0f, stemPow[i] * invTotal);
        pushMeters (share, computeFrameRms());
    }
};
