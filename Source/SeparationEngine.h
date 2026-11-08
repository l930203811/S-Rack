#pragma once

#include <JuceHeader.h>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "kiss_fft.h"
#include "kiss_fftr.h"
}

#include "MFAudioFormat.h"

//==============================================================================
// OrtRuntime —— 修复 VST3 在机架中被加载时的 onnxruntime.dll 版本冲突
//
// 现象：Standalone 正常，VST3 在宿主里一播放就 0xc0000005 把宿主一起带走。
// 崩溃转储：MediaPlay.vst3+0x1cd30b → call qword ptr [r9+18h]，异常地址 0x18，
// r9 = Ort::Global<void>::api_ = nullptr，字符串参数是 "SeparationEngine"，
// 即首次构造 Ort::Env 时解引用了空的 ORT API 函数表。
//
// 根因：VST3 静态导入 "onnxruntime.dll"（不带路径）。宿主 LoadLibrary 插件时，
// 依赖解析按「已加载模块(按基名) → 宿主 exe 目录 → System32 → ...」查找，
// 插件 bundle 的 Contents\x86_64-win 不在搜索路径中。本机 System32 有一份
// ONNX Runtime 1.5.2（2021 年 / 7 MB），插件因此被绑到它；而
// onnxruntime_cxx_api.h 的全局初始化
//     Global<void>::api_ = OrtGetApiBase()->GetApi(ORT_API_VERSION /*=17*/)
// 在 1.5.2 上实测返回 nullptr（GetApi(8..20) 全为 0），此后任何 Ort:: 调用都会崩。
//
// 修复：首次使用 ORT 前，按「本模块所在目录」的绝对路径显式加载随插件分发的
// onnxruntime.dll（1.17.1），取它的 OrtApi 表写回 Global<void>::api_。
// 实测即便 System32 的旧 DLL 已加载，绝对路径仍会加载出独立模块（句柄不同）。
// 若重定向失败，调用方必须彻底放弃所有 Ort:: 调用：宁可 AI 分离失败，也绝不崩宿主。
//==============================================================================
namespace OrtRuntime
{
    inline void log (const juce::String& s)
    {
        juce::File ("D:\\Xiangmu\\S-Rack\\mp_debug.log")
            .appendText (juce::Time::getCurrentTime().toString (false, true) + "  [ORT] " + s + "\n");
    }

    inline bool apiAvailable() noexcept { return Ort::Global<void>::api_ != nullptr; }

    inline bool resolve()
    {
       #if ! JUCE_WINDOWS
        return apiAvailable();
       #else
        // 诊断：记录静态导入实际绑到了哪个 DLL
        {
            if (HMODULE h = ::GetModuleHandleW (L"onnxruntime.dll"))
            {
                wchar_t buf[2048] = {};
                const DWORD n = ::GetModuleFileNameW (h, buf, 2048);
                if (n > 0 && n < 2048)
                    log ("static import bound to " + juce::String (buf));
            }
        }

        if (apiAvailable())
        {
            log ("OrtApi available at module load (OK)");
            return true;
        }
        log ("OrtApi is NULL - static import bound to an incompatible onnxruntime.dll, rebinding...");

        // 用本模块内的静态变量地址反查自身 HMODULE：standalone 是 exe 目录，
        // VST3 是 Contents\x86_64-win 目录（不能用 currentExecutableFile，那会拿到宿主目录）
        HMODULE self = nullptr;
        static int anchor = 0;
        if (! ::GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                     | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCWSTR> (&anchor), &self))
        {
            log ("GetModuleHandleExW failed err=" + juce::String ((int) ::GetLastError()));
            return false;
        }
        wchar_t selfPath[2048] = {};
        const DWORD len = ::GetModuleFileNameW (self, selfPath, 2048);
        if (len == 0 || len >= 2048) { log ("GetModuleFileNameW failed"); return false; }

        const juce::File dll = juce::File (juce::String (selfPath)).getParentDirectory()
                                    .getChildFile ("onnxruntime.dll");
        if (! dll.existsAsFile())
        {
            log ("bundled DLL missing: " + dll.getFullPathName());
            return false;
        }

        // 绝对路径加载：即使同名旧 DLL 已在进程中，也会得到独立模块（实测句柄不同）。
        // 数值常量避免 SDK 版本差异：DLL_LOAD_DIR(0x100) | DEFAULT_DIRS(0x800)
        const DWORD searchFlags = 0x00000100 | 0x00000800;
        HMODULE h = ::LoadLibraryExW (dll.getFullPathName().toWideCharPointer(), nullptr, searchFlags);
        if (h == nullptr)
            h = ::LoadLibraryW (dll.getFullPathName().toWideCharPointer());
        if (h == nullptr)
        {
            log ("LoadLibrary failed err=" + juce::String ((int) ::GetLastError())
                 + " path=" + dll.getFullPathName());
            return false;
        }

        using GetBaseFn = const OrtApiBase* (ORT_API_CALL*) ();
        auto getBase = reinterpret_cast<GetBaseFn> (::GetProcAddress (h, "OrtGetApiBase"));
        if (getBase == nullptr) { log ("OrtGetApiBase export missing"); return false; }
        const OrtApiBase* base = getBase();
        if (base == nullptr)    { log ("OrtGetApiBase returned null"); return false; }

        const OrtApi* api = base->GetApi (ORT_API_VERSION);
        if (api == nullptr)
        {
            log ("bundled DLL GetApi(" + juce::String (ORT_API_VERSION)
                 + ") still null, version=" + juce::String (base->GetVersionString()));
            return false;
        }

        // 覆盖 C++ wrapper 的全局函数表。故意不 FreeLibrary：该模块必须在插件
        // 整个生命周期内保持加载，否则所有 OrtApi 函数指针都会悬空。
        Ort::Global<void>::api_ = api;
        log ("rebound OK: " + dll.getFullPathName()
             + " version=" + juce::String (base->GetVersionString()));
        return true;
       #endif
    }

    // 重定向只做一次；但每次调用都重新核对当前指针，防止 CRT 的延迟全局初始化
    // 在插件构造之后又把 api_ 覆盖成空。返回 false 时调用方必须跳过全部 Ort:: 调用。
    inline bool ensure()
    {
        static std::once_flag flag;
        std::call_once (flag, [] { (void) resolve(); });
        return apiAvailable();
    }
}

//==============================================================================
// SeparationEngine —— MDX-Net (Kim_Vocal_2 / UVR ONNX) 后台人声分离引擎
//
// 管线与离线验证工具 Tools/SepVerify.cpp 逐项一致（已通过真歌试听验证）：
//   文件解码（任意采样率）→ Catmull-Rom 重采样到 44100Hz →
//   STFT(6144/1024, periodic Hann, center) → [1,4,3072,256]（c0.re,c0.im,c1.re,c1.im，
//   前 3 频点清零）→ ONNX 模型 → iSTFT(OLA/Σw²，+kTrim 对齐读出) →
//   np.hanning 重叠相加 → 44100Hz 人声 → 重采样回文件采样率。
//
// 使用方式：
//   start(file, fileRate, trackId) —— 消息线程调用；自动停止上一个任务。
//   音频线程：status()==Ready && trackId==current 时可读 vocalL/vocalR。
//   状态从 Working→Ready 之前缓冲区写完（release），Ready→Working 由 start()
//   在清任务时先翻转，音频线程每块开头重查状态。为避免音频线程仍持旧指针
//   时工作线程就改写缓冲区，worker 启动后先等待 250ms 再动缓冲区。
//
// 模型查找：exe 目录/models/ → exe 目录/ → 开发目录 ThirdParty/。
//==============================================================================
class SeparationEngine : private juce::Thread
{
public:
    enum Status { Idle = 0, Working = 1, Ready = 2, Failed = 3 };

    static constexpr int    kModelSr = 44100;
    static constexpr int    kFft     = 6144;
    static constexpr int    kHop     = 1024;
    static constexpr int    kDimF    = 3072;
    static constexpr int    kDimT    = 256;
    static constexpr int    kBins    = kFft / 2 + 1;
    static constexpr int    kTrim    = kFft / 2;
    static constexpr int    kChunk   = kHop * (kDimT - 1);
    static constexpr double kOverlap = 0.25;
    static constexpr double kCompensate = 1.035;

    SeparationEngine() : juce::Thread ("SeparationEngine") {}
    ~SeparationEngine() override { shutdown(); }

    // 与实时播放并行跑「预分离」时限制 ORT 线程数并降低线程优先级，
    // 避免 3 个满负荷推理线程把宿主音频回调饿死（0 = 恢复离线导出的默认配置）。
    inline static std::atomic<int> threadLimit { 0 };
    static void setThreadLimit (int n) { threadLimit.store (n, std::memory_order_relaxed); }

    void shutdown()
    {
        cancel();
        // 绝不能用 stopThread(-1)：一个推理 chunk 可能跑十几秒，无限等待会让
        // 宿主 / 独立程序「关不掉」。给 4 秒宽限期后让 JUCE 自行收尾。
        stopThread (4000);
    }

    // 消息线程：暂停 / 停止 / 切歌时立即放弃当前任务，避免后台继续吃满 CPU 与内存
    // 暂停 / 停止：只放弃「工作线程里还在跑」的推理，保留整首已完成的结果。
    // 否则每次 pause→play 都会把 201 秒的模型从头重跑一遍（CPU/内存 100%）。
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
            // cancelFlag 的所有写都放在 requestLock 内，与 start() 串行化，
            // 否则「start 写 false」可能覆盖「cancel 写 true」，任务永远停不下来。
            cancelFlag.store (true, std::memory_order_relaxed);
            hasPending = false;
        }
        // 立刻让音频线程 / UI 看到「没有可用结果」：
        // 否则 status 仍是上一曲的 Ready，导出器会误判任务已完成、进度瞬间拉满。
        status.store (Idle, std::memory_order_release);
        readyTrackId.store (-1, std::memory_order_release);
        requestedTrackId.store (-1, std::memory_order_relaxed);
        progress.store (0.0f, std::memory_order_relaxed);
        pubLen.store (0, std::memory_order_release);
        notify();
    }

    // 消息线程：为曲目请求分离（立即返回；工作线程在 chunk 边界自动切换到最新请求）
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
        // 同步翻状态：start() 返回后 getStatus() 绝不可能是上一轮的 Ready。
        // 这同时修掉导出器「刚启动就读取上一首的人声缓冲」的竞态。
        status.store (Working, std::memory_order_release);
        readyTrackId.store (-1, std::memory_order_release);
        requestedTrackId.store (trackIdIn, std::memory_order_relaxed);
        progress.store (0.0f, std::memory_order_relaxed);
        pubLen.store (0, std::memory_order_release);
        notify();
        // 离线导出用 normal 优先级；与实时播放并行的「预分离」用 low，
        // 并配 threadLimit=2，避免抢占宿主音频线程。
        if (! isThreadRunning())
            startThread (threadLimit.load (std::memory_order_relaxed) > 0
                             ? juce::Thread::Priority::low
                             : juce::Thread::Priority::normal);
    }

    // UI 进度显示用（请求中的曲目，与 readyTrackId 区分）
    int    getRequestedTrackId() const { return requestedTrackId.load (std::memory_order_relaxed); }
    Status getStatus() const        { return (Status) status.load (std::memory_order_acquire); }
    float  getProgress() const      { return progress.load (std::memory_order_relaxed); }
    juce::String getError() const   { return errorMsg; }

    // ---- 音频线程：仅当返回 true 时才允许读 vocal 缓冲 ----
    bool isReadyFor (int expectedTrackId) const
    {
        return status.load (std::memory_order_acquire) == Ready
            && readyTrackId.load (std::memory_order_acquire) == expectedTrackId;
    }

    // ---- 不可变结果缓冲 ----
    // 旧实现让音频线程持 outL.data() 裸指针，worker 下一轮 runJob 一赋值
    // vector 就重新分配 → 指针悬空，读到已释放的堆块（爆音 / 0xc0000374）。
    // 改为每次任务整体发布一个 shared_ptr，音频线程按快照持有，地址永不失效。
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
            s.length = juce::jmax (0, juce::jmin (pubLen.load (std::memory_order_acquire), cap));
        }
        return s;
    }

private:
    // 任务级取消判定：显式取消，或已被更新的曲目请求取代。
    // 快速切歌时 start() 会把 cancelFlag 重新置 false，只看 cancelFlag 的话
    // 旧任务会继续把整首歌跑完，几个满负荷推理线程叠加就是 CPU/内存 100%。
    bool jobCancelled (int jobTrackId) const noexcept
    {
        return cancelFlag.load (std::memory_order_acquire)
            || jobTrackId != requestedTrackId.load (std::memory_order_acquire);
    }

    static void log (const juce::String& s)
    {
        juce::File ("D:\\Xiangmu\\S-Rack\\mp_debug.log").appendText (
            juce::Time::getCurrentTime().toString (false, true) + "  [SEP] " + s + "\n");
    }

    void run() override
    {
        // MF/COM 要求调用线程初始化 COM
        ::CoInitializeEx (nullptr, COINIT_MULTITHREADED);

        juce::File jobFile;
        double jobRate = 48000.0;
        int jobTrackId = -1;

        while (! threadShouldExit())
        {
            // 等待请求。绝不能在持有 requestLock 时 wait(-1)：
            // cancel()/start() 都是「先拿锁、再 notify」，持锁睡眠会与之形成互锁，
            // 且 stopThread() 也无法唤醒它 —— 表现为关闭程序时永远卡住。
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
                wait (200);   // 有限超时：即使 notify 丢失也能及时响应退出信号
                continue;
            }

            // 这里绝不能重置 cancelFlag：消息线程可能在取走 job 之后、推理开始之前
            // 按下暂停，重置会把取消请求抹掉，后台继续跑满 CPU / 内存。
            if (jobCancelled (jobTrackId))
                continue;

            // 翻成 Working：音频线程立刻停止读取上一曲的 Ready 缓冲
            status.store (Working, std::memory_order_release);
            readyTrackId.store (-1, std::memory_order_release);
            progress.store (0.0f, std::memory_order_relaxed);
        pubLen.store (0, std::memory_order_release);

            // 音频线程可能还持着上一曲缓冲的指针（状态刚翻），等它过去
            Thread::sleep (250);

            try
            {
                runJob (jobFile, jobRate, jobTrackId);
            }
            catch (const Ort::Exception& e)
            {
                // e.what() 内部会走 GetApi().GetErrorMessage；API 表为空时不能碰它
                errorMsg = juce::String ("ONNX 推理异常: ")
                         + (OrtRuntime::apiAvailable() ? juce::String (e.what())
                                                       : juce::String ("ORT API 不可用"));
                log (errorMsg);
                if (! jobCancelled (jobTrackId))
                    status.store (Failed, std::memory_order_release);
            }
            catch (const std::exception& e)
            {
                errorMsg = juce::String ("分离异常: ") + e.what();
                log (errorMsg);
                if (! jobCancelled (jobTrackId))
                    status.store (Failed, std::memory_order_release);
            }
        }

        ::CoUninitialize();
    }

    void runJob (const juce::File& audioFile, double fileRateIn, int jobTrackId)
    {
        // 硬保护：ORT API 不可用时绝不构造任何 Ort:: 对象。
        // 旧实现在宿主里会带着空的 Global<void>::api_ 去 new Ort::Env，
        // 虚表间接调用读地址 0x18 → 0xc0000005 → 整个宿主进程被带走。
        if (! OrtRuntime::ensure())
        {
            errorMsg = "ONNX Runtime 不可用，已跳过 AI 分离（播放不受影响）";
            log (errorMsg);
            status.store (Failed, std::memory_order_release);
            return;
        }

        fileRate = fileRateIn;
        juce::AudioFormatManager mgr;
        mgr.registerFormat (new MFAudioFormat(), false);
        mgr.registerBasicFormats();
        mgr.registerFormat (new juce::MP3AudioFormat(), false);

        std::unique_ptr<juce::AudioFormatReader> rd (mgr.createReaderFor (audioFile));
        if (rd == nullptr)
        {
            errorMsg = "无法打开音频文件用于分离";
            status.store (Failed, std::memory_order_release);
            return;
        }

        const double inSr = rd->sampleRate;
        const int numCh = (int) rd->numChannels;
        const juce::int64 knownLen = rd->lengthInSamples;

        // ---- 分块解码到本地采样率（L/R 平面存储）----
        // 按 1 秒分块推进，避免单次 read 巨量样本导致 MF 内部缓冲暴涨；
        // 同时天然兼容未知时长的文件（读到 EOF 为止）。
        std::vector<float> decL, decR;
        bool decodeOk = true;
        {
            const int blockN = (int) inSr;                 // 每块 ~1 秒
            std::vector<float> tmpL ((size_t) blockN), tmpR ((size_t) blockN);
            juce::int64 pos = 0;
            // 注意：JUCE AudioFormatReader::read 越过 EOF 返回 true 并填 0，
            // 绝不能以返回值判 EOF，必须用 lengthInSamples 限界（否则无限读零、
            // 内存暴涨：本机实测 3 秒文件会无限解码到崩溃）。
            const juce::int64 total = rd->lengthInSamples;
            auto* mfRd = dynamic_cast<MFAudioFormatReader*> (rd.get());
            while (! jobCancelled (jobTrackId))
            {
                const bool eof = (total > 0) ? (pos >= total)
                                             : (mfRd != nullptr && mfRd->isEof());
                if (eof) break;
                const int want = (int) ((total > 0) ? juce::jmin<juce::int64> (blockN, total - pos)
                                                    : blockN);
                float* chans[2] = { tmpL.data(), tmpR.data() };
                rd->read (chans, juce::jmin (2, numCh), pos, want);
                decL.insert (decL.end(), tmpL.begin(), tmpL.begin() + want);
                if (numCh > 1) decR.insert (decR.end(), tmpR.begin(), tmpR.begin() + want);
                else           decR.insert (decR.end(), tmpL.begin(), tmpL.begin() + want);
                pos += want;
            }
            log (juce::String::formatted ("decode done: %.1fs, ok=%d",
                                          (double) decL.size() / inSr, (int) decodeOk));
        }
        rd.reset();

        if (! decodeOk || decL.empty() || jobCancelled (jobTrackId))
        {
            errorMsg = decodeOk ? "已取消" : "解码失败";
            status.store (jobCancelled (jobTrackId) ? Idle : Failed,
                          std::memory_order_release);
            return;
        }

        // ---- 重采样到 44100 ----
        const double ratioIn = kModelSr / inSr;
        std::vector<float> mixL = resampleCR (decL.data(), decL.size(), ratioIn);
        std::vector<float> mixR = resampleCR (decR.data(), decR.size(), ratioIn);
        decL.clear(); decR.clear(); decL.shrink_to_fit(); decR.shrink_to_fit();
        if (jobCancelled (jobTrackId)) { status.store (Idle, std::memory_order_release); return; }

        // ---- UVR 预处理布局：[zeros(kTrim), mix, pad] ----
        const int mixLen = (int) mixL.size();
        int pad = kChunk - 2 * kTrim - (mixLen % (kChunk - 2 * kTrim));
        if (pad == kChunk - 2 * kTrim) pad = 0;
        const int paddedLen = kTrim + mixLen + pad;
        std::vector<float> mixtureL ((size_t) paddedLen, 0.0f), mixtureR ((size_t) paddedLen, 0.0f);
        std::copy (mixL.begin(), mixL.end(), mixtureL.begin() + kTrim);
        std::copy (mixR.begin(), mixR.end(), mixtureR.begin() + kTrim);
        mixL.clear(); mixR.clear(); mixL.shrink_to_fit(); mixR.shrink_to_fit();

        // ---- 模型会话（懒加载，跨曲目复用）----
        Ort::Env* envPtr = nullptr;
        {
            const juce::ScopedLock sl (ortLock);
            if (ortEnv == nullptr)
                ortEnv = std::make_unique<Ort::Env> (ORT_LOGGING_LEVEL_ERROR, "SeparationEngine");
            envPtr = ortEnv.get();
        }
        const juce::File modelFile = findModelFile();
        if (modelFile == juce::File())
        {
            errorMsg = "找不到 Kim_Vocal_2.onnx 模型文件";
            log (errorMsg);
            status.store (Failed, std::memory_order_release);
            return;
        }

        // 绝不能用 String::formatted("%s", char*)：JUCE 8 在 Windows 上走 _vsnwprintf，
        // %s 期望 wchar_t*，传 UTF-8 char* 会被按 UTF-16 解读成乱码并越界读相邻内存
        // （日志里 "㩄塜慩杮畭" 即由此而来），是 AI 分离时崩溃的直接原因之一。
        log ("model session creating (" + modelFile.getFullPathName() + "), mixLen="
             + juce::String (mixLen));
        Ort::Session session (*envPtr, modelFile.getFullPathName().toWideCharPointer(), makeSessionOptions());
        log ("model session ok");
        const char* inName  = "input";
        const char* outName = "output";

        // ---- FFT / 窗 ----
        RealFft fft; fft.init (kFft);
        const auto winFft = makeHannPeriodic (kFft);

        const int step = (int) ((1.0 - kOverlap) * (double) kChunk);
        std::vector<float> resultL ((size_t) paddedLen, 0.0f), resultR ((size_t) paddedLen, 0.0f);
        std::vector<float> divider ((size_t) paddedLen, 0.0f);

        std::vector<float> spekBuf ((size_t) 4 * kDimF * kDimT);
        std::vector<float> predBuf ((size_t) 4 * kDimF * kDimT);
        std::vector<float> tmpTime ((size_t) kFft);
        std::vector<float> accRe ((size_t) kChunk + kFft), accNorm ((size_t) kChunk + kFft);
        std::vector<kiss_fft_cpx> tmpFreq ((size_t) kBins);
        std::vector<float> chunkL ((size_t) kChunk), chunkR ((size_t) kChunk);
        const auto winFull = makeHannSymmetric (kChunk);

        int totalChunks = 0;
        for (int s = 0; s < paddedLen; s += step) ++totalChunks;
        log (juce::String::formatted ("separation begin: paddedLen=%d, %d chunks", paddedLen, totalChunks));

        int chunkIdx = 0;
        for (int start = 0; start < paddedLen; start += step)
        {
            if (jobCancelled (jobTrackId)) { status.store (Idle, std::memory_order_release); return; }

            const int end = juce::jmin (start + kChunk, paddedLen);
            std::fill (chunkL.begin(), chunkL.end(), 0.0f);
            std::fill (chunkR.begin(), chunkR.end(), 0.0f);
            std::copy (mixtureL.begin() + start, mixtureL.begin() + end, chunkL.begin());
            std::copy (mixtureR.begin() + start, mixtureR.begin() + end, chunkR.begin());

            // ---- STFT（center=True：帧 t 覆盖 chunk[t*hop-trim, ...)）----
            std::fill (spekBuf.begin(), spekBuf.end(), 0.0f);
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* x = (ch == 0) ? chunkL.data() : chunkR.data();
                for (int t = 0; t < kDimT; ++t)
                {
                    const int off = t * kHop - kTrim;
                    for (int i = 0; i < kFft; ++i)
                    {
                        const int idx = off + i;
                        const float s = (idx >= 0 && idx < kChunk) ? x[idx] : 0.0f;
                        tmpTime[(size_t) i] = s * winFft[(size_t) i];
                    }
                    fft.forward (tmpTime.data(), tmpFreq.data());
                    const size_t chIdx = (size_t) ch * 2;
                    for (int f = 0; f < kDimF; ++f)
                    {
                        spekBuf[((chIdx * (size_t) kDimF + (size_t) f) * (size_t) kDimT) + (size_t) t] = tmpFreq[(size_t) f].r;
                        spekBuf[(((chIdx + 1) * (size_t) kDimF + (size_t) f) * (size_t) kDimT) + (size_t) t] = tmpFreq[(size_t) f].i;
                    }
                }
            }
            for (int ch = 0; ch < 4; ++ch)
                for (int t = 0; t < kDimT; ++t)
                    for (int f = 0; f < 3; ++f)
                        spekBuf[((size_t) ch * kDimF + (size_t) f) * (size_t) kDimT + (size_t) t] = 0.0f;

            // ---- 模型推理（spekBuf 绝不能在此清零！）----
            {
                std::array<int64_t, 4> shape { 1, 4, kDimF, kDimT };
                auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
                Ort::Value in (Ort::Value::CreateTensor<float> (mem, spekBuf.data(), spekBuf.size(), shape.data(), 4));
                auto outs = session.Run (Ort::RunOptions { nullptr }, &inName, &in, 1, &outName, 1);
                std::memcpy (predBuf.data(), outs[0].GetTensorData<float>(),
                             sizeof (float) * (size_t) 4 * kDimF * kDimT);
            }

            // ---- iSTFT（读出 +kTrim 对齐）----
            const int L = (kDimT - 1) * kHop;
            for (int ch = 0; ch < 2; ++ch)
            {
                const size_t reIdx = (size_t) ch * 2, imIdx = (size_t) ch * 2 + 1;
                std::fill (accRe.begin(), accRe.end(), 0.0f);
                std::fill (accNorm.begin(), accNorm.end(), 0.0f);
                for (int t = 0; t < kDimT; ++t)
                {
                    for (int f = 0; f < kDimF; ++f)
                    {
                        tmpFreq[(size_t) f].r = predBuf[((reIdx * (size_t) kDimF + (size_t) f) * (size_t) kDimT) + (size_t) t];
                        tmpFreq[(size_t) f].i = predBuf[((imIdx * (size_t) kDimF + (size_t) f) * (size_t) kDimT) + (size_t) t];
                    }
                    for (int f = kDimF; f < kBins; ++f) { tmpFreq[(size_t) f].r = 0.0f; tmpFreq[(size_t) f].i = 0.0f; }
                    fft.inverse (tmpFreq.data(), tmpTime.data());
                    const int base = t * kHop;
                    for (int i = 0; i < kFft; ++i)
                    {
                        accRe[(size_t) (base + i)] += tmpTime[(size_t) i] * winFft[(size_t) i];
                        accNorm[(size_t) (base + i)] += winFft[(size_t) i] * winFft[(size_t) i];
                    }
                }
                float* dst = (ch == 0) ? chunkL.data() : chunkR.data();
                for (int i = 0; i < L; ++i)
                {
                    const float norm = accNorm[(size_t) (i + kTrim)];
                    dst[(size_t) i] = (norm > 1.0e-8f) ? accRe[(size_t) (i + kTrim)] / norm : 0.0f;
                }
                for (int i = L; i < kChunk; ++i) dst[(size_t) i] = 0.0f;
            }

            // ---- np.hanning 重叠相加 ----
            const int actual = end - start;
            const auto& winA = (actual == kChunk) ? winFull : makeHannSymmetric (actual);
            for (int i = 0; i < actual; ++i)
            {
                resultL[(size_t) (start + i)] += chunkL[(size_t) i] * winA[(size_t) i];
                resultR[(size_t) (start + i)] += chunkR[(size_t) i] * winA[(size_t) i];
                divider  [(size_t) (start + i)] += winA[(size_t) i];
            }

            ++chunkIdx;
            progress.store ((float) chunkIdx / (float) totalChunks, std::memory_order_relaxed);
            if (chunkIdx == 1 || chunkIdx % 10 == 0)
                log (juce::String::formatted ("chunk %d/%d", chunkIdx, totalChunks));
            // 每个 chunk 之间让出调度器：chunk 边界会分配/释放几十 MB 临时缓冲，
            // 连续跑会同时拖住堆管理器与宿主音频线程。
            juce::Thread::sleep (1);
        }

        // ---- 除以窗、去 trim、44100 → 文件采样率 ----
        std::vector<float> voc44L ((size_t) mixLen), voc44R ((size_t) mixLen);
        for (int i = 0; i < mixLen; ++i)
        {
            const float d = divider[(size_t) (i + kTrim)];
            const float inv = (d > 1.0e-6f) ? (float) (kCompensate / d) : 0.0f;
            voc44L[(size_t) i] = juce::jlimit (-1.0f, 1.0f, resultL[(size_t) (i + kTrim)] * inv);
            voc44R[(size_t) i] = juce::jlimit (-1.0f, 1.0f, resultR[(size_t) (i + kTrim)] * inv);
        }

        const double ratioOut = (double) fileRate / (double) kModelSr;
        auto result = std::make_shared<VocalData>();
        result->L = resampleCR (voc44L.data(), voc44L.size(), ratioOut);
        result->R = resampleCR (voc44R.data(), voc44R.size(), ratioOut);
        const int rlen = (int) juce::jmin (result->L.size(), result->R.size());

        // 发布：先写 readyTrackId 再置 Ready（音频线程 isReadyFor 双重校验）
        // 取消 / 切歌后不得再把上一曲的结果翻回 Ready，否则 UI 会显示已完成、
        // 音频线程也会在交叉淡化期读到过期人声。
        if (jobCancelled (jobTrackId))
        {
            status.store (Idle, std::memory_order_release);
            return;
        }
        // 发布顺序：先把长度归零，再整体替换缓冲，最后置新长度。
        // 这样音频线程即使撞在中间，也只会读到「旧缓冲 + 0 长度」或
        // 「新缓冲 + 完整长度」，绝不会拿旧缓冲配新长度造成越界读。
        pubLen.store (0, std::memory_order_release);
        std::atomic_store_explicit (&pub,
            std::shared_ptr<const VocalData> (result), std::memory_order_release);
        pubLen.store (rlen, std::memory_order_release);
        log ("separation READY (" + juce::String (rlen) + " samples)");
        readyTrackId.store (jobTrackId, std::memory_order_release);
        status.store (Ready, std::memory_order_release);
    }

    //==============================================================================
    // ---- 工具（与 SepVerify 一致）----
    static std::vector<float> resampleCR (const float* x, size_t nIn, double ratio)
    {
        // nIn < 4 时 (nIn - 4) 是 size_t 下溢，会算出天文数字的输出长度并在
        // 下面的 vector 构造里直接吃满内存 / 抛 bad_alloc；采样率异常导致的
        // ratio=0/inf/NaN 同理。先做边界检查再分配。
        if (x == nullptr || nIn < 4 || ! (ratio > 0.0) || ! std::isfinite (ratio))
            return {};
        const double outD = std::floor ((double) (nIn - 4) * ratio);
        constexpr double kMaxOut = 48000.0 * 60.0 * 60.0;   // 1 小时 @48k 的硬上限
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

    struct RealFft
    {
        kiss_fftr_cfg fwd = nullptr, inv = nullptr;
        double invScale = 1.0;
        ~RealFft()
        {
            if (fwd != nullptr) kiss_fftr_free (fwd);
            if (inv != nullptr) kiss_fftr_free (inv);
        }
        void init (int n)
        {
            fwd = kiss_fftr_alloc (n, 0, nullptr, nullptr);
            inv = kiss_fftr_alloc (n, 1, nullptr, nullptr);
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
        int n = kFft;
    };

    static std::vector<float> makeHannPeriodic (int n)
    {
        std::vector<float> w ((size_t) n);
        for (int i = 0; i < n; ++i)
            w[(size_t) i] = 0.5f - 0.5f * (float) std::cos (2.0 * juce::MathConstants<double>::pi * i / n);
        return w;
    }

    static std::vector<float> makeHannSymmetric (int n)
    {
        std::vector<float> w ((size_t) n);
        if (n <= 1) { std::fill (w.begin(), w.end(), 1.0f); return w; }
        for (int i = 0; i < n; ++i)
            w[(size_t) i] = 0.5f - 0.5f * (float) std::cos (2.0 * juce::MathConstants<double>::pi * i / (n - 1));
        return w;
    }

    static Ort::SessionOptions makeSessionOptions()
    {
        Ort::SessionOptions opts;
        // 默认给离线导出 3 线程；与实时播放并行时按 threadLimit 收敛到 2。
        const int def = juce::jlimit (1, 3, (int) std::thread::hardware_concurrency() - 1);
        const int lim = threadLimit.load (std::memory_order_relaxed);
        const int threads = lim > 0 ? juce::jmin (def, lim) : def;
        opts.SetIntraOpNumThreads (threads);
        opts.SetInterOpNumThreads (1);
        opts.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
        return opts;
    }

    static juce::File findModelFile()
    {
        const juce::File exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        juce::Array<juce::File> candidates {
            exe.getParentDirectory().getChildFile ("models/Kim_Vocal_2.onnx"),
            exe.getParentDirectory().getChildFile ("Kim_Vocal_2.onnx"),
            juce::File ("D:/Xiangmu/S-Rack/ThirdParty/Kim_Vocal_2.onnx"),
        };
        for (const auto& f : candidates)
            if (f.existsAsFile()) return f;
        return juce::File();
    }

    //==============================================================================
    juce::CriticalSection requestLock;
    juce::File pendingFile;
    double pendingRate = 48000.0;
    int pendingTrackId = -1;
    bool hasPending = false;

    double fileRate = 48000.0;
    std::atomic<int>    readyTrackId { -1 };       // 仅在 Ready 时有效
    std::atomic<int>    requestedTrackId { -1 };   // UI 进度显示用
    std::atomic<int>    status { Idle };
    std::atomic<float>  progress { 0.0f };
    std::atomic<bool>   cancelFlag { false };
    juce::String errorMsg;

    juce::CriticalSection ortLock;
    std::unique_ptr<Ort::Env> ortEnv;

    // 音频线程在 Ready 状态下按快照读取（shared_ptr 保证地址不失效）
    std::shared_ptr<const VocalData> pub;
    std::atomic<int> pubLen { 0 };
};
