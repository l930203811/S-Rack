// SepVerify：MDX-Net（Kim_Vocal_2 等 UVR ONNX 模型）离线人声分离验证工具。
//
// 管线逐项对齐 UVR separate.py（SeperateMDX.demix / run_model + lib_v5/tfc_tdf_v3.py STFT）：
//   * 48kHz 立体声交错 f32 输入 → Catmull-Rom 重采样到 44100Hz（模型工作采样率）
//   * STFT：n_fft=6144、hop=1024、periodic Hann、center=True
//     → [1, 4, dim_f=2048, dim_t=256]（通道排列 c0.re, c0.im, c1.re, c1.im）
//   * 前 3 个频点清零；模型输出即人声谱
//   * iSTFT：频点补零回 3073，OLA / Σw²，去掉两端 trim=3072
//   * 分块：chunk = hop*(256-1) = 261120 样本，np.hanning 窗重叠相加，
//     overlap=0.25 → step = 0.75*chunk；输出 * compensate(1.035)
//   * 伴奏 = 混音 - 人声
//
// 用法：SepVerify.exe <model.onnx> <input.f32> <outDir> [denoise 0|1]

#include <juce_audio_utils/juce_audio_utils.h>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "kiss_fft.h"
#include "kiss_fftr.h"
}

namespace fs = std::filesystem;

//==============================================================================
static constexpr int    kInSampleRate  = 48000;   // 输入 f32 采样率
static constexpr double kModelSr       = 44100.0; // 模型工作采样率
static constexpr int    kFft           = 6144;
static constexpr int    kHop           = 1024;
static constexpr int    kDimF          = 3072;    // Kim_Vocal_2：频率轴 3072（仅丢弃奈奎斯特 bin）
static constexpr int    kDimT          = 256;
static constexpr int    kBins          = kFft / 2 + 1;   // 3073
static constexpr int    kTrim          = kFft / 2;        // 3072
static constexpr int    kChunk         = kHop * (kDimT - 1); // 261120
static constexpr int    kGenSize       = kChunk - 2 * kTrim;
static constexpr double kOverlap       = 0.25;
static constexpr double kCompensate    = 1.035;

//==============================================================================
// Catmull-Rom 4 点重采样（验证用；插件内同样可用，音质足够）
static std::vector<float> resampleCatmullRom (const float* x, size_t nIn, double ratio)
{
    const size_t nOut = (size_t) std::max (0.0, std::floor ((double) (nIn - 4) * ratio));
    std::vector<float> y (nOut, 0.0f);
    if (nIn < 4 || nOut == 0) return y;

    for (size_t i = 0; i < nOut; ++i)
    {
        const double pos = (double) i / ratio;
        size_t i1 = (size_t) pos;
        double t  = pos - (double) i1;
        if (i1 >= nIn - 2) { i1 = nIn - 2; t = 0.0; }
        const size_t i0 = (i1 == 0) ? 0 : i1 - 1;
        const size_t i2 = i1 + 1;
        const size_t i3 = (i2 + 1 < nIn) ? i2 + 1 : i2;

        const float p0 = x[i0], p1 = x[i1], p2 = x[i2], p3 = x[i3];
        const float a = 0.5f * ((-p0 + 3.0f * p1 - 3.0f * p2 + p3));
        const float b = 0.5f * ((2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3));
        const float c = 0.5f * ((p2 - p0));
        y[i] = 0.5f * ((2.0f * p1) + c * (float) t + b * (float) (t * t) + a * (float) (t * t * t));
    }
    return y;
}

//==============================================================================
// kissfft 实数 FFT 封装（6144 = 2^11 * 3，kissfft 原生支持）
struct RealFft
{
    int n = 0;
    kiss_fftr_cfg fwd = nullptr, inv = nullptr;
    double invScale = 1.0;   // kiss_fftri 输出校准比例

    RealFft() = default;
    ~RealFft() { freeCfg(); }

    void init (int size)
    {
        freeCfg();
        n = size;
        fwd = kiss_fftr_alloc (n, 0, nullptr, nullptr);
        inv = kiss_fftr_alloc (n, 1, nullptr, nullptr);

        // 经验校准：正变换后逆变换的往返增益（kissfft 逆变换不归一化）
        std::vector<float> x ((size_t) n), t ((size_t) n + 2);
        std::vector<kiss_fft_cpx> F ((size_t) n / 2 + 1);
        double sumIn = 0.0, sumOut = 0.0;
        for (int i = 0; i < n; ++i) { x[(size_t) i] = (float) std::sin (0.37 * i); sumIn += x[(size_t) i] * x[(size_t) i]; }
        kiss_fftr (fwd, x.data(), F.data());
        kiss_fftri (inv, F.data(), t.data());
        for (int i = 0; i < n; ++i) { const double v = t[(size_t) i]; sumOut += v * v; }
        invScale = (sumOut > 0.0) ? std::sqrt (sumIn / sumOut) : 1.0;
    }

    void forward (const float* time, kiss_fft_cpx* freq) const { kiss_fftr (fwd, time, freq); }
    void inverse (const kiss_fft_cpx* freq, float* time) const
    {
        kiss_fftri (inv, freq, time);
        for (int i = 0; i < n; ++i) time[i] = (float) ((double) time[i] * invScale);
    }

private:
    void freeCfg()
    {
        if (fwd != nullptr) { kiss_fftr_free (fwd); fwd = nullptr; }
        if (inv != nullptr) { kiss_fftr_free (inv); inv = nullptr; }
    }
};

static std::vector<float> makeHannPeriodic (int n)   // torch.hann_window(periodic=True)
{
    std::vector<float> w ((size_t) n);
    for (int i = 0; i < n; ++i)
        w[(size_t) i] = 0.5f - 0.5f * (float) std::cos (2.0 * juce::MathConstants<double>::pi * i / n);
    return w;
}

static std::vector<float> makeHannSymmetric (int n)  // np.hanning
{
    std::vector<float> w ((size_t) n);
    if (n <= 1) { std::fill (w.begin(), w.end(), 1.0f); return w; }
    for (int i = 0; i < n; ++i)
        w[(size_t) i] = 0.5f - 0.5f * (float) std::cos (2.0 * juce::MathConstants<double>::pi * i / (n - 1));
    return w;
}

//==============================================================================
class Separator
{
public:
    Separator (const fs::path& modelPath, bool useDenoise)
        : denoise (useDenoise)
    {
        env = std::make_unique<Ort::Env> (ORT_LOGGING_LEVEL_WARNING, "SepVerify");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads ((int) std::thread::hardware_concurrency());
        opts.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
        session = std::make_unique<Ort::Session> (*env, modelPath.wstring().c_str(), opts);

        Ort::AllocatorWithDefaultOptions alloc;
        inputName  = session->GetInputNameAllocated (0, alloc).get();
        outputName = session->GetOutputNameAllocated (0, alloc).get();

        auto inInfo = session->GetInputTypeInfo (0).GetTensorTypeAndShapeInfo();
        inDims = inInfo.GetShape();
        std::printf ("[model] input elem type=%d\n", (int) inInfo.GetElementType());
        auto outInfo = session->GetOutputTypeInfo (0).GetTensorTypeAndShapeInfo();
        outDims = outInfo.GetShape();

        std::printf ("[model] input='%s' dims=[", inputName.c_str());
        for (auto d : inDims) std::printf ("%lld ", (long long) d);
        std::printf ("] output='%s' dims=[", outputName.c_str());
        for (auto d : outDims) std::printf ("%lld ", (long long) d);
        std::printf ("]\n");
    }

    // spek: [1,4,dimF,dimT] 已清零前 3 频点；输出写入 pred（同形状）
    void run (const float* spek, float* pred)
    {
        std::array<int64_t, 4> shape { 1, 4, kDimF, kDimT };
        auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value in (Ort::Value::CreateTensor<float> (mem, const_cast<float*> (spek), (size_t) 4 * kDimF * kDimT, shape.data(), 4));
        const char* const inName  = inputName.c_str();
        const char* const outName = outputName.c_str();
        std::printf ("[run] before Run\n");
        auto outs = session->Run (Ort::RunOptions { nullptr },
                                  &inName, &in, 1, &outName, 1);
        std::printf ("[run] after Run, outs=%d\n", (int) outs.size());
        std::memcpy (pred, outs[0].GetTensorMutableData<float>(), sizeof (float) * (size_t) 4 * kDimF * kDimT);
    }

    bool isDenoise() const { return denoise; }

private:
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::string inputName, outputName;
    std::vector<int64_t> inDims, outDims;
    bool denoise = false;
};

//==============================================================================
// 单个 chunk 的 STFT → 模型 → iSTFT
// chunkIn: [2][kChunk]（44.1kHz）；out: [2][kChunk] 人声波形
static void processChunk (Separator& sep, const RealFft& fft,
                          const std::vector<float>& win,
                          const float* chunkL, const float* chunkR,
                          float* outL, float* outR,
                          std::vector<float>& spekBuf, std::vector<float>& predBuf,
                          std::vector<float>& tmpTime, std::vector<kiss_fft_cpx>& tmpFreq,
                          std::vector<float>& accRe, std::vector<float>& accNorm)
{
    std::printf ("[pc] stft begin\n");
    const int T = kDimT;

    // ---- STFT（center=True：先补 kTrim 零，帧 t 起点 = t*hop）----
    // 输入在调用方已按 [zeros(kTrim), x, ...] 布局给出（直接用 chunkIn 前向填充）
    spekBuf.assign ((size_t) 4 * kDimF * kDimT, 0.0f);

    for (int ch = 0; ch < 2; ++ch)
    {
        const float* x = (ch == 0) ? chunkL : chunkR;
        for (int t = 0; t < T; ++t)
        {
            const int off = t * kHop - kTrim;   // 相对 chunk 起始（chunk 含前端 kTrim 零）
            for (int i = 0; i < kFft; ++i)
            {
                const int idx = off + i;
                // 调用方保证 chunk 数组前后各留 kTrim 空间（负索引区为 0）
                float s = 0.0f;
                if (idx >= 0 && idx < kChunk) s = x[idx];
                else if (idx < 0 || idx >= kChunk) s = 0.0f;
                tmpTime[(size_t) i] = s * win[(size_t) i];
            }
            fft.forward (tmpTime.data(), tmpFreq.data());
            for (int f = 0; f < kDimF; ++f)
            {
                const size_t chIdx = (size_t) ch * 2;   // re
                spekBuf[((chIdx * (size_t) kDimF + (size_t) f) * (size_t) T) + (size_t) t] = tmpFreq[(size_t) f].r;
                spekBuf[(((chIdx + 1) * (size_t) kDimF + (size_t) f) * (size_t) T) + (size_t) t] = tmpFreq[(size_t) f].i;
            }
        }
    }

    // 前 3 个频点清零（UVR: spek[:, :, :3, :] *= 0）
    for (int ch = 0; ch < 4; ++ch)
        for (int t = 0; t < T; ++t)
            for (int f = 0; f < 3; ++f)
                spekBuf[((size_t) ch * kDimF + (size_t) f) * (size_t) T + (size_t) t] = 0.0f;

    // ---- 模型推理（denoise 模式跑两次取平均：0.5*(M(x) - M(-x))）----
    // 注意：spekBuf 在上面 STFT 已填好，这里绝不能再清零！
    std::printf ("[pc] stft done, model begin\n");
    predBuf.assign ((size_t) 4 * kDimF * kDimT, 0.0f);
    sep.run (spekBuf.data(), predBuf.data());
    std::printf ("[pc] model run 1 done\n");
    if (sep.isDenoise())
    {
        std::vector<float> neg (spekBuf.size()), pred2 (predBuf.size());
        for (size_t i = 0; i < spekBuf.size(); ++i) neg[i] = -spekBuf[i];
        sep.run (neg.data(), pred2.data());
        for (size_t i = 0; i < predBuf.size(); ++i)
            predBuf[i] = 0.5f * (predBuf[i] - pred2[i]);
    }

    // ---- iSTFT：4 通道 [re,im]×2 → 复数频谱（补零至 kBins）→ OLA ----
    const int L = (T - 1) * kHop;   // 261120
    std::fill (accRe.begin(), accRe.end(), 0.0f);
    std::fill (accNorm.begin(), accNorm.end(), 0.0f);
    std::fill (tmpFreq.begin(), tmpFreq.end(), kiss_fft_cpx { 0.0f, 0.0f });

    for (int ch = 0; ch < 2; ++ch)
    {
        const size_t reIdx = (size_t) ch * 2, imIdx = (size_t) ch * 2 + 1;
        std::fill (accRe.begin(), accRe.end(), 0.0f);
        std::fill (accNorm.begin(), accNorm.end(), 0.0f);

        for (int t = 0; t < T; ++t)
        {
            for (int f = 0; f < kDimF; ++f)
            {
                tmpFreq[(size_t) f].r = predBuf[((reIdx * (size_t) kDimF + (size_t) f) * (size_t) T) + (size_t) t];
                tmpFreq[(size_t) f].i = predBuf[((imIdx * (size_t) kDimF + (size_t) f) * (size_t) T) + (size_t) t];
            }
            for (int f = kDimF; f < kBins; ++f) { tmpFreq[(size_t) f].r = 0.0f; tmpFreq[(size_t) f].i = 0.0f; }

            fft.inverse (tmpFreq.data(), tmpTime.data());
            const int base = t * kHop;
            for (int i = 0; i < kFft; ++i)
            {
                accRe[(size_t) (base + i)] += tmpTime[(size_t) i] * win[(size_t) i];
                accNorm[(size_t) (base + i)] += win[(size_t) i] * win[(size_t) i];
            }
        }

        float* dst = (ch == 0) ? outL : outR;
        // acc[j] 表示 chunk 位置 j-kTrim（帧 t 覆盖 [t*hop-kTrim, ...)），读出需 +kTrim 对齐
        for (int i = 0; i < L; ++i)
        {
            const float norm = accNorm[(size_t) (i + kTrim)];
            dst[(size_t) i] = (norm > 1e-8f) ? accRe[(size_t) (i + kTrim)] / norm : 0.0f;
        }
    }
}

//==============================================================================
static int runAll (const fs::path& modelPath, const fs::path& inputPath,
                   const fs::path& outDir, bool denoise);

int main (int argc, char** argv)
{
    setvbuf (stdout, nullptr, _IONBF, 0);
    setvbuf (stderr, nullptr, _IONBF, 0);

    if (argc < 4)
    {
        std::printf ("usage: SepVerify.exe <model.onnx> <input.f32> <outDir> [denoise]\n");
        return 1;
    }
    const fs::path modelPath = argv[1];
    const fs::path inputPath = argv[2];
    const fs::path outDir    = argv[3];
    const bool denoise = (argc >= 5) ? (std::string (argv[4]) == "1") : true;

    try
    {
        return runAll (modelPath, inputPath, outDir, denoise);
    }
    catch (const Ort::Exception& e)
    {
        std::printf ("[ORT ERROR] %s (code %d)\n", e.what(), (int) e.GetOrtErrorCode());
        return 3;
    }
    catch (const std::exception& e)
    {
        std::printf ("[ERROR] %s\n", e.what());
        return 3;
    }
}

static int runAll (const fs::path& modelPath, const fs::path& inputPath,
                   const fs::path& outDir, bool denoise)
{
    {
        std::error_code ec;
        fs::create_directories (outDir, ec);
    }

    // ---- 读入 48k 交错 f32 ----
    juce::MemoryBlock raw;
    juce::File (inputPath.string()).loadFileAsData (raw);
    const size_t nFrames48 = raw.getSize() / (2 * sizeof (float));
    std::vector<float> mixL48 (nFrames48), mixR48 (nFrames48);
    {
        const float* p = reinterpret_cast<const float*> (raw.getData());
        for (size_t i = 0; i < nFrames48; ++i) { mixL48[i] = p[2 * i]; mixR48[i] = p[2 * i + 1]; }
    }
    const double ratio = kModelSr / (double) kInSampleRate;
    std::vector<float> mixL = resampleCatmullRom (mixL48.data(), nFrames48, ratio);
    std::vector<float> mixR = resampleCatmullRom (mixR48.data(), nFrames48, ratio);
    const int mixLen = (int) mixL.size();
    std::printf ("[input] %.2f s @44.1kHz (%d samples)\n", mixLen / kModelSr, mixLen);

    // ---- UVR 预处理布局 ----
    int pad = kGenSize + kTrim - (mixLen % kGenSize);
    if (pad == kGenSize) pad = 0;
    const int paddedLen = kTrim + mixLen + pad;
    std::vector<float> mixtureL ((size_t) paddedLen, 0.0f), mixtureR ((size_t) paddedLen, 0.0f);
    std::copy (mixL.begin(), mixL.end(), mixtureL.begin() + kTrim);
    std::copy (mixR.begin(), mixR.end(), mixtureR.begin() + kTrim);

    const int step = (int) ((1.0 - kOverlap) * (double) kChunk);
    std::vector<float> resultL ((size_t) paddedLen, 0.0f), resultR ((size_t) paddedLen, 0.0f);
    std::vector<float> divider ((size_t) paddedLen, 0.0f);

    const auto winFull = makeHannSymmetric (kChunk);
    RealFft fft; fft.init (kFft);
    const auto winFft = makeHannPeriodic (kFft);

    Separator sep (modelPath, denoise);

    std::vector<float> spekBuf, predBuf, tmpTime ((size_t) kFft),
                       accRe ((size_t) (kChunk + kFft)), accNorm ((size_t) (kChunk + kFft));
    std::vector<kiss_fft_cpx> tmpFreq ((size_t) kBins);
    std::vector<float> chunkL ((size_t) kChunk), chunkR ((size_t) kChunk),
                       outVL ((size_t) kChunk), outVR ((size_t) kChunk);

    const auto t0 = std::chrono::steady_clock::now();
    double inferSeconds = 0.0;
    int chunkIdx = 0, totalChunks = 0;
    for (int s = 0; s < paddedLen; s += step) ++totalChunks;

    for (int start = 0; start < paddedLen; start += step)
    {
        const int end = std::min (start + kChunk, paddedLen);
        const int actual = end - start;

        std::fill (chunkL.begin(), chunkL.end(), 0.0f);
        std::fill (chunkR.begin(), chunkR.end(), 0.0f);
        std::copy (mixtureL.begin() + start, mixtureL.begin() + end, chunkL.begin());
        std::copy (mixtureR.begin() + start, mixtureR.begin() + end, chunkR.begin());

        const auto ti0 = std::chrono::steady_clock::now();
        processChunk (sep, fft, winFft, chunkL.data(), chunkR.data(),
                      outVL.data(), outVR.data(), spekBuf, predBuf,
                      tmpTime, tmpFreq, accRe, accNorm);
        const auto ti1 = std::chrono::steady_clock::now();
        inferSeconds += std::chrono::duration<double> (ti1 - ti0).count();

        // np.hanning(actual) 窗 + 重叠相加
        std::vector<float> winA = (actual == kChunk) ? winFull : makeHannSymmetric (actual);
        for (int i = 0; i < actual; ++i)
        {
            resultL[(size_t) (start + i)] += outVL[(size_t) i] * winA[(size_t) i];
            resultR[(size_t) (start + i)] += outVR[(size_t) i] * winA[(size_t) i];
            divider  [(size_t) (start + i)] += winA[(size_t) i];
        }

        ++chunkIdx;
        std::printf ("[chunk %d/%d] %.1fs 推理累计 %.2fs\n", chunkIdx, totalChunks,
                     chunkIdx * step / kModelSr, inferSeconds);
        std::fflush (stdout);
    }

    // ---- 除以窗、去 trim、裁回原长 ----
    std::vector<float> vocL ((size_t) mixLen), vocR ((size_t) mixLen), insL ((size_t) mixLen), insR ((size_t) mixLen);
    for (int i = 0; i < mixLen; ++i)
    {
        const int j = i + kTrim;
        const float d = divider[(size_t) j];
        float vL = (d > 1e-6f) ? resultL[(size_t) j] / d : 0.0f;
        float vR = (d > 1e-6f) ? resultR[(size_t) j] / d : 0.0f;
        vL = juce::jlimit (-1.0f, 1.0f, vL * (float) kCompensate);
        vR = juce::jlimit (-1.0f, 1.0f, vR * (float) kCompensate);
        vocL[(size_t) i] = vL;  vocR[(size_t) i] = vR;
        insL[(size_t) i] = juce::jlimit (-1.0f, 1.0f, mixL[(size_t) i] - vL);
        insR[(size_t) i] = juce::jlimit (-1.0f, 1.0f, mixR[(size_t) i] - vR);
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double totalSec = std::chrono::duration<double> (t1 - t0).count();
    const double audioSec = mixLen / kModelSr;
    std::printf ("[done] 音频 %.1fs | 模型推理 %.1fs (%.2fx 实时) | 总耗时 %.1fs\n",
                 audioSec, inferSeconds, audioSec / inferSeconds, totalSec);

    // 手写 24-bit PCM WAV（避免 JUCE 写盘路径的不确定性）
    auto writeWav = [] (const fs::path& path, const std::vector<float>& l, const std::vector<float>& r, double sr) -> bool
    {
        const int numFrames = (int) l.size();
        const int bytesPerSample = 3;
        const int dataBytes = numFrames * 2 * bytesPerSample;
        std::vector<unsigned char> buf (44 + (size_t) dataBytes);
        auto put32 = [&] (size_t& o, uint32_t v) { buf[o++] = (unsigned char) v; buf[o++] = (unsigned char) (v >> 8); buf[o++] = (unsigned char) (v >> 16); buf[o++] = (unsigned char) (v >> 24); };
        size_t o = 0;
        buf[o++] = 'R'; buf[o++] = 'I'; buf[o++] = 'F'; buf[o++] = 'F';
        put32 (o, (uint32_t) (36 + dataBytes));
        buf[o++] = 'W'; buf[o++] = 'A'; buf[o++] = 'V'; buf[o++] = 'E';
        buf[o++] = 'f'; buf[o++] = 'm'; buf[o++] = 't'; buf[o++] = ' ';
        put32 (o, 16);
        buf[o++] = 1; buf[o++] = 0;                       // PCM
        buf[o++] = 2; buf[o++] = 0;                       // stereo
        put32 (o, (uint32_t) sr);
        put32 (o, (uint32_t) (sr * 2 * bytesPerSample));
        buf[o++] = (unsigned char) (2 * bytesPerSample); buf[o++] = 0;
        buf[o++] = 24; buf[o++] = 0;
        buf[o++] = 'd'; buf[o++] = 'a'; buf[o++] = 't'; buf[o++] = 'a';
        put32 (o, (uint32_t) dataBytes);

        for (int i = 0; i < numFrames; ++i)
            for (const float* ch : { l.data(), r.data() })
            {
                float v = juce::jlimit (-1.0f, 1.0f, ch[i]);
                int32_t s = (int32_t) (v * 8388607.0f);
                buf[o++] = (unsigned char) s; buf[o++] = (unsigned char) ((uint32_t) s >> 8); buf[o++] = (unsigned char) ((uint32_t) s >> 16);
            }

        FILE* fp = nullptr;
        if (_wfopen_s (&fp, path.wstring().c_str(), L"wb") != 0 || fp == nullptr) return false;
        const size_t wrote = fwrite (buf.data(), 1, buf.size(), fp);
        fclose (fp);
        return wrote == buf.size();
    };

    std::printf ("[wav] writing vocals...\n");
    const bool ok1 = writeWav (outDir / "mdx_vocals.wav", vocL, vocR, kModelSr);
    std::printf ("[wav] vocals=%d, writing instrumental...\n", (int) ok1);
    const bool ok2 = writeWav (outDir / "mdx_instrumental.wav", insL, insR, kModelSr);
    std::printf ("[wav] instrumental=%d, writing reference mix...\n", (int) ok2);
    const bool ok3 = writeWav (outDir / "mdx_reference_mix.wav", mixL, mixR, kModelSr);
    std::printf ("[wav] mix=%d -> %s\n", (int) ok3, outDir.string().c_str());
    std::fflush (nullptr);
    _exit ((ok1 && ok2 && ok3) ? 0 : 2);   // 跳过静态析构（ORT 线程池退出可触发 fail-fast）
}
