// 诊断：1) STFT/iSTFT 往返精度  2) 模型通道排列假设
#include <onnxruntime_cxx_api.h>
#include <kiss_fft.h>
#include <kiss_fftr.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static constexpr int kInSampleRate = 48000;
static constexpr double kModelSr = 44100.0;
static constexpr int kFft = 6144;
static constexpr int kHop = 1024;
static constexpr int kDimF = 3072;
static constexpr int kDimT = 256;
static constexpr int kBins = kFft / 2 + 1;
static constexpr int kTrim = kFft / 2;
static constexpr int kChunk = kHop * (kDimT - 1);

struct RealFft
{
    kiss_fftr_cfg fwd = nullptr, inv = nullptr;
    double invScale = 1.0;
    void init (int n)
    {
        fwd = kiss_fftr_alloc (n, 0, nullptr, nullptr);
        inv = kiss_fftr_alloc (n, 1, nullptr, nullptr);
        std::vector<float> x (n), t (n + 2);
        std::vector<kiss_fft_cpx> F (n / 2 + 1);
        double si = 0, so = 0;
        for (int i = 0; i < n; ++i) { x[i] = std::sin (0.37f * i); si += x[i] * x[i]; }
        kiss_fftr (fwd, x.data(), F.data());
        kiss_fftri (inv, F.data(), t.data());
        for (int i = 0; i < n; ++i) so += t[i] * t[i];
        invScale = std::sqrt (si / so);
    }
};

static std::vector<float> hannPeriodic (int n)
{
    std::vector<float> w (n);
    for (int i = 0; i < n; ++i) w[i] = 0.5f - 0.5f * (float) std::cos (2.0 * 3.14159265358979323846 * i / n);
    return w;
}

static std::vector<float> resampleCR (const float* x, size_t nIn, double ratio)
{
    const size_t nOut = (size_t) std::max (0.0, std::floor ((double) (nIn - 4) * ratio));
    std::vector<float> y (nOut, 0.0f);
    for (size_t i = 0; i < nOut; ++i)
    {
        const double pos = (double) i / ratio;
        size_t i1 = (size_t) pos;
        double t = pos - (double) i1;
        if (i1 >= nIn - 2) { i1 = nIn - 2; t = 0.0; }
        const size_t i0 = (i1 == 0) ? 0 : i1 - 1;
        const size_t i2 = i1 + 1, i3 = (i2 + 1 < nIn) ? i2 + 1 : i2;
        const float p0 = x[i0], p1 = x[i1], p2 = x[i2], p3 = x[i3];
        const float a = 0.5f * (-p0 + 3.0f * p1 - 3.0f * p2 + p3);
        const float b = 0.5f * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3);
        const float c = 0.5f * (p2 - p0);
        y[i] = 0.5f * (2.0f * p1 + c * (float) t + b * (float) (t * t) + a * (float) (t * t * t));
    }
    return y;
}

static double rms (const std::vector<float>& v)
{
    if (v.empty()) return 0.0;
    double s = 0; for (float x : v) s += x * x;
    return std::sqrt (s / v.size());
}

int main()
{
    setvbuf (stdout, nullptr, _IONBF, 0);

    // 读 f32，取前 48k*7s → 重采样到 44.1k
    FILE* fp = nullptr;
    _wfopen_s (&fp, L"D:\\Xiangmu\\S-Rack\\build\\user_test.f32", L"rb");
    if (fp == nullptr) { std::printf ("open f32 failed\n"); return 1; }
    const size_t want48 = (size_t) kInSampleRate * 7;
    std::vector<float> raw (want48 * 2);
    fread (raw.data(), sizeof (float), raw.size(), fp);
    fclose (fp);
    std::vector<float> l48 (want48), r48 (want48);
    for (size_t i = 0; i < want48; ++i) { l48[i] = raw[2 * i]; r48[i] = raw[2 * i + 1]; }
    const double ratio = kModelSr / kInSampleRate;
    std::vector<float> mixL = resampleCR (l48.data(), want48, ratio);
    std::vector<float> mixR = resampleCR (r48.data(), want48, ratio);
    std::printf ("input rms L=%.5f R=%.5f\n", rms (mixL), rms (mixR));

    const auto win = hannPeriodic (kFft);
    RealFft fft; fft.init (kFft);

    // 组装一个 chunk（前 261120 样本，够填 256 帧）
    std::vector<float> chunk (kChunk, 0.0f);
    std::copy (mixL.begin(), mixL.begin() + std::min (mixL.size(), (size_t) kChunk), chunk.begin());
    std::vector<float> chunkR (kChunk, 0.0f);
    std::copy (mixR.begin(), mixR.begin() + std::min (mixR.size(), (size_t) kChunk), chunkR.begin());

    // ---- STFT ----
    std::vector<float> spek ((size_t) 4 * kDimF * kDimT, 0.0f);
    {
        std::vector<float> tt (kFft);
        std::vector<kiss_fft_cpx> tf (kBins);
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* x = ch == 0 ? chunk.data() : chunkR.data();
            for (int t = 0; t < kDimT; ++t)
            {
                const int off = t * kHop - kTrim;
                for (int i = 0; i < kFft; ++i)
                {
                    const int idx = off + i;
                    const float s = (idx >= 0 && idx < kChunk) ? x[idx] : 0.0f;
                    tt[i] = s * win[i];
                }
                fft.fwd ? kiss_fftr (fft.fwd, tt.data(), tf.data()) : void (0);
                for (int f = 0; f < kDimF; ++f)
                {
                    spek[(((size_t) ch * 2 + 0) * kDimF + f) * kDimT + t] = tf[f].r;
                    spek[(((size_t) ch * 2 + 1) * kDimF + f) * kDimT + t] = tf[f].i;
                }
            }
        }
        std::printf ("stft rms=%.5f\n", rms (spek));
    }

    // ---- iSTFT 往返验证（不经模型）----
    {
        std::vector<float> acc ((size_t) kChunk + kFft, 0.0f), accN ((size_t) kChunk + kFft, 0.0f);
        std::vector<float> tt (kFft);
        std::vector<kiss_fft_cpx> tf (kBins);
        for (int ch = 0; ch < 2; ++ch)
        {
            std::fill (acc.begin(), acc.end(), 0.0f);
            std::fill (accN.begin(), accN.end(), 0.0f);
            for (int t = 0; t < kDimT; ++t)
            {
                for (int f = 0; f < kBins; ++f)
                {
                    if (f < kDimF)
                    {
                        tf[f].r = spek[(((size_t) ch * 2 + 0) * kDimF + f) * kDimT + t];
                        tf[f].i = spek[(((size_t) ch * 2 + 1) * kDimF + f) * kDimT + t];
                    }
                    else { tf[f].r = 0; tf[f].i = 0; }
                }
                kiss_fftri (fft.inv, tf.data(), tt.data());
                for (int i = 0; i < kFft; ++i) tt[i] = (float) ((double) tt[i] * fft.invScale);
                const int base = t * kHop;
                for (int i = 0; i < kFft; ++i)
                {
                    acc[base + i] += tt[i] * win[i];
                    accN[base + i] += win[i] * win[i];
                }
            }
            // 与原始 chunk 对比（内部区域）
            const float* x = ch == 0 ? chunk.data() : chunkR.data();
            double diff = 0, orig = 0;
            for (int i = kTrim; i < kChunk - kTrim; ++i)
            {
                const float y = accN[i] > 1e-8f ? acc[i] / accN[i] : 0.0f;
                diff += (y - x[i]) * (y - x[i]);
                orig += x[i] * x[i];
            }
            std::printf ("istft roundtrip ch%d: err=%.6f (相对 %.4f)\n", ch, std::sqrt (diff), std::sqrt (diff / (orig + 1e-9)));
        }
    }

    // ---- 模型通道排列测试 ----
    Ort::Env env (ORT_LOGGING_LEVEL_WARNING, "SepDebug");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads (4);
    Ort::Session session (env, L"D:\\Xiangmu\\S-Rack\\ThirdParty\\Kim_Vocal_2.onnx", opts);

    const char* inName = "input";
    const char* outName = "output";

    auto runPerm = [&] (const char* label, const int* map)   // map[输出槽] = 源通道
    {
        std::vector<float> in ((size_t) 4 * kDimF * kDimT);
        for (int dst = 0; dst < 4; ++dst)
        {
            const int src = map[dst];
            std::memcpy (in.data() + (size_t) dst * kDimF * kDimT,
                         spek.data() + (size_t) src * kDimF * kDimT,
                         sizeof (float) * (size_t) kDimF * kDimT);
        }
        for (int f = 0; f < 3; ++f)
            for (int ch = 0; ch < 4; ++ch)
                for (int t = 0; t < kDimT; ++t)
                    in[((size_t) ch * kDimF + f) * kDimT + t] = 0.0f;

        std::array<int64_t, 4> shape { 1, 4, kDimF, kDimT };
        auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value v (Ort::Value::CreateTensor<float> (mem, in.data(), in.size(), shape.data(), 4));
        auto outs = session.Run (Ort::RunOptions { nullptr }, &inName, &v, 1, &outName, 1);
        const float* o = outs[0].GetTensorData<float>();
        double s = 0;
        for (size_t i = 0; i < (size_t) 4 * kDimF * kDimT; ++i) s += o[i] * o[i];
        std::printf ("perm %s -> out rms=%.6f\n", label, std::sqrt (s / ((size_t) 4 * kDimF * kDimT)));
    };

    // UVR 布局: ch*2+re/im → [L.re, L.im, R.re, R.im]
    { const int m[4] = { 0, 1, 2, 3 }; runPerm ("[Lre,Lim,Rre,Rim]", m); }
    // 交错: [Lre, Rre, Lim, Rim]
    { const int m[4] = { 0, 2, 1, 3 }; runPerm ("[Lre,Rre,Lim,Rim]", m); }
    // L/R 互换
    { const int m[4] = { 2, 3, 0, 1 }; runPerm ("[Rre,Rim,Lre,Lim]", m); }
    // 负谱（imag 取反等价于输入取反 → 输出极性）
    { const int m[4] = { 0, -1, 2, -3 }; }   // 略：极性不影响 RMS
    return 0;
}
