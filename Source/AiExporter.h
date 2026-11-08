#pragma once

#include <JuceHeader.h>
#include "SeparationEngine.h"
#include "MFAudioFormat.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>
#include <objbase.h>   // CoInitializeEx / CoUninitialize

//==============================================================================
// AiExporter —— AI 分离导出引擎（页面整合后由「音频分离」按钮调用）
//
// 复用 SeparationEngine 的 MDX-Net (Kim_Vocal_2) 离线分离管线：
//   对每个选中的音频文件跑一遍模型分离得到人声（自动重采样回文件采样率），
//   伴奏 = 原曲 - 人声（逐样本相减，与播放路径的 instrumental 语义一致），
//   按需写出 16-bit 立体声 WAV 到「<源文件目录>/AI分离输出/」。
//
// 独立工作线程串行处理队列；消息线程只读进度 / 状态 / 结果列表。
//==============================================================================
class AiExporter : private juce::Thread
{
public:
    enum class Mode { Vocal = 0, Instr = 1, Both = 2 };

    AiExporter() : juce::Thread ("AiExporter") {}
    ~AiExporter() override { shutdown(); }

    void shutdown()
    {
        cancelFlag.store (true, std::memory_order_relaxed);
        // 先让内部的离线分离引擎放弃当前推理，否则析构时还要再等一整轮 chunk
        sep.cancel();
        stopThread (4000);
    }

    // 切歌时放弃当前批次：只发取消信号，不阻塞消息线程等线程退出。
    // 线程会在下一个 chunk / 队列检查点自行结束；期间 startExport() 会返回 false。
    void cancelExport()
    {
        cancelFlag.store (true, std::memory_order_relaxed);
        sep.cancel();
    }

    // 启动一批导出；已有任务进行中时返回 false
    bool startExport (juce::Array<juce::File> files, Mode m)
    {
        if (isThreadRunning()) return false;
        if (files.isEmpty())   return false;

        // 清掉上一轮遗留的分离状态。否则 sep 仍停在 Ready / progress=1.0，
        // 新任务一开始 overallProgress() 就会算出 100%（进度条瞬间拉满），
        // 而下面的等待循环也会立刻误判「已完成」并写出上一首的人声。
        sep.cancel();

        {
            const juce::ScopedLock sl (dataLock);
            results.clear();
            firstError.clear();
            outDir = juce::File();
            statusText = Z ("正在启动...");
        }
        {
            const juce::ScopedLock sl (dataLock);
            queue = std::move (files);
        }
        mode       = m;
        doneCount.store (0, std::memory_order_relaxed);
        failCount.store (0, std::memory_order_relaxed);
        cancelFlag.store (false, std::memory_order_relaxed);
        startThread (juce::Thread::Priority::normal);
        return true;
    }

    bool isBusy() const { return isThreadRunning(); }

    float overallProgress() const
    {
        // queue 会在 startExport() 里被替换，UI 定时器每 33ms 读一次；
        // 必须与写端用同一把锁，否则读到重分配中的 juce::Array 会踩已释放堆块。
        int total = 0;
        {
            const juce::ScopedLock sl (dataLock);
            total = queue.size();
        }
        if (total <= 0) return 0.0f;
        const float per = 1.0f / (float) total;
        return juce::jlimit (0.0f, 1.0f,
            ((float) doneCount.load (std::memory_order_relaxed) + sep.getProgress()) * per);
    }

    juce::String getStatusText() const
    {
        const juce::ScopedLock sl (dataLock);
        return statusText;
    }

    juce::Array<juce::File> getResults() const
    {
        const juce::ScopedLock sl (dataLock);
        return results;
    }

    juce::File getOutputDir() const
    {
        const juce::ScopedLock sl (dataLock);
        return outDir;
    }

private:
    //==========================================================================
    static juce::String Z (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

    // 导出过程写独立日志 + 输出目录清单：即使应用在导出后崩溃，
    // 用户也能从 mp_debug.log 或「AI分离输出/导出结果.txt」查到分离是否成功、文件在哪。
    static void aiLog (const juce::String& s)
    {
        juce::File ("D:\\Xiangmu\\S-Rack\\mp_debug.log").appendText (
            juce::Time::getCurrentTime().toString (false, true) + "  [AI] " + s + "\n");
    }

    void appendManifest (const juce::String& line)
    {
        const auto dir = getOutputDir();
        if (dir == juce::File() || ! dir.isDirectory()) return;
        dir.getChildFile (Z ("导出结果.txt")).appendText (
            juce::Time::getCurrentTime().toString (true, true) + "  " + line + "\n");
    }

    void setStatus (const juce::String& s)
    {
        const juce::ScopedLock sl (dataLock);
        statusText = s;
    }

    void addResult (const juce::File& f)
    {
        {
            const juce::ScopedLock sl (dataLock);
            results.add (f);
        }
        aiLog (Z ("已写出：") + f.getFullPathName()
               + Z (" (") + juce::String (f.getSize() / (1024 * 1024)) + Z (" MB)"));
        // 每写完一个文件就落盘一行；即便随后崩溃，清单里也留有结果
        appendManifest (Z ("成功：") + f.getFileName() + Z (" · ")
                        + juce::String (f.getSize()) + Z (" 字节 · ") + f.getFullPathName());
    }

    void addError (const juce::String& e)
    {
        {
            const juce::ScopedLock sl (dataLock);
            failCount.store (failCount.load (std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            if (firstError.isEmpty()) firstError = e;
        }
        aiLog (Z ("失败：") + e);
        appendManifest (Z ("失败：") + e);
    }

    void run() override
    {
        ::CoInitializeEx (nullptr, COINIT_MULTITHREADED);

        juce::AudioFormatManager mgr;
        mgr.registerFormat (new MFAudioFormat(), false);
        mgr.registerBasicFormats();
        mgr.registerFormat (new juce::MP3AudioFormat(), false);

        const int total = queue.size();
        aiLog (Z ("批次开始：共 ") + juce::String (total) + Z (" 个文件"));
        for (int i = 0; i < total && ! threadShouldExit(); ++i)
        {
            const auto src = queue[i];
            // 不用 String::formatted：JUCE 8 / Windows 底层是 _vsnwprintf，
            // 变参类型稍有不匹配就会把 UTF-8 当 UTF-16 解读成乱码并越界读内存。
            // 中文状态文本一律用 String 拼接。
            setStatus (Z ("AI 分离中 ") + juce::String (i + 1) + Z ("/")
                       + juce::String (total) + Z ("：") + src.getFileName());

            // 输出目录：第一个源文件同目录下的 AI分离输出
            if (i == 0)
            {
                auto dir = src.getParentDirectory().getChildFile (Z ("AI分离输出"));
                const juce::Result r = dir.createDirectory();
                {
                    const juce::ScopedLock sl (dataLock);
                    outDir = dir;
                    if (r.failed()) firstError = r.getErrorMessage();
                }
                aiLog (Z ("输出目录：") + dir.getFullPathName()
                       + (r.failed() ? Z ("（创建失败：") + r.getErrorMessage() + Z ("）") : juce::String()));
                appendManifest (Z ("=== 批次开始：共 ") + juce::String (total)
                                + Z (" 个文件，输出目录 ") + dir.getFullPathName());
            }

            // 读取文件采样率（SeparationEngine 会把人声重采样回该采样率）
            double fileRate = 44100.0;
            {
                std::unique_ptr<juce::AudioFormatReader> rd (mgr.createReaderFor (src));
                if (rd == nullptr)
                {
                    addError (src.getFileName() + Z("：无法打开音频文件"));
                    doneCount.store (i + 1, std::memory_order_relaxed);
                    continue;
                }
                fileRate = rd->sampleRate;
            }

            // 跑模型分离并等待完成。必须校验 trackId，只认本轮任务的结果：
            // 以前只读 getStatus()，会撞上上一曲残留的 Ready，导致进度条瞬间拉满、
            // 并把旧缓冲当成当前文件的分离结果写出。
            sep.start (src, fileRate, ++jobId);
            const int myJobId = jobId;
            bool ok = false;
            while (! threadShouldExit() && ! cancelFlag.load (std::memory_order_relaxed))
            {
                if (sep.isReadyFor (myJobId)) { ok = true; break; }
                const auto st = sep.getStatus();
                if (st == SeparationEngine::Failed || st == SeparationEngine::Idle) break;
                Thread::sleep (120);
            }

            if (ok && sep.isReadyFor (myJobId)
                && ! threadShouldExit() && ! cancelFlag.load (std::memory_order_relaxed))
                writeResults (mgr, src, fileRate);
            else if (! ok && ! threadShouldExit() && ! cancelFlag.load (std::memory_order_relaxed))
                addError (src.getFileName() + Z ("：分离未完成或已失败"));

            doneCount.store (i + 1, std::memory_order_relaxed);
        }

        // 汇总状态：把输出目录写进状态文本与日志，避免「分离完了却不知道文件在哪」
        if (! threadShouldExit())
        {
            const int fail = failCount.load (std::memory_order_relaxed);
            const int okN  = total - fail;
            juce::String s = Z ("提取完成：成功 ") + juce::String (okN)
                           + Z (" 首，失败 ") + juce::String (fail) + Z (" 首");
            juce::String dirPath;
            {
                const juce::ScopedLock sl (dataLock);
                if (! firstError.isEmpty()) s += Z(" · ") + firstError;
                dirPath = outDir.getFullPathName();
                if (dirPath.isNotEmpty()) s += Z (" · 输出：") + dirPath;
                statusText = s;
            }
            aiLog (Z ("批次结束：") + s);
            appendManifest (Z ("=== 批次结束：成功 ") + juce::String (okN)
                            + Z (" 首，失败 ") + juce::String (fail) + Z (" 首"));
        }

        ::CoUninitialize();
    }

    //==========================================================================
    // 分块解码原曲（与 SeparationEngine 相同的 1 秒分块策略，避免 MF 缓冲暴涨）
    void decodeMix (juce::AudioFormatManager& mgr, const juce::File& f,
                    std::vector<float>& L, std::vector<float>& R)
    {
        std::unique_ptr<juce::AudioFormatReader> rd (mgr.createReaderFor (f));
        if (rd == nullptr) return;

        const juce::int64 total = rd->lengthInSamples;
        const int numCh = (int) rd->numChannels;
        const int blockN = 48000;
        std::vector<float> tL ((size_t) blockN), tR ((size_t) blockN);
        juce::int64 pos = 0;
        while (pos < total && ! threadShouldExit())
        {
            const int want = (int) juce::jmin<juce::int64> (blockN, total - pos);
            float* chans[2] = { tL.data(), tR.data() };
            rd->read (chans, juce::jmin (2, numCh), pos, want);
            L.insert (L.end(), tL.begin(), tL.begin() + want);
            if (numCh > 1) R.insert (R.end(), tR.begin(), tR.begin() + want);
            else           R.insert (R.end(), tL.begin(), tL.begin() + want);
            pos += want;
        }
    }

    static bool writeWav (const juce::File& f, double rate,
                          const float* l, const float* r, int n)
    {
        if (n <= 0) return false;
        f.deleteFile();

        // 绝不能把「栈上的 FileOutputStream」地址传给旧版 createWriterFor：
        // AudioFormatWriter 析构时会 `delete output`，等于 delete 一块栈内存，
        // Windows 堆管理器随即抛 0xc0000374（STATUS_HEAP_CORRUPTION）。
        // 本机 11:15:38 的崩溃正是「WAV 刚写完、应用立刻关闭」的元凶。
        // JUCE 8 新版接口用 unique_ptr 引用显式移交所有权，失败时流仍归我们释放。
        std::unique_ptr<juce::OutputStream> stream (
            std::make_unique<juce::FileOutputStream> (f));
        if (! static_cast<juce::FileOutputStream*> (stream.get())->openedOk())
            return false;

        juce::WavAudioFormat wav;
        const auto opts = juce::AudioFormatWriterOptions{}
                              .withSampleRate (rate)
                              .withNumChannels (2)
                              .withBitsPerSample (16);
        auto w = wav.createWriterFor (stream, opts);   // 成功时 stream 被置空并移交
        if (w == nullptr) return false;                // 失败时 stream 由 unique_ptr 释放

        const float* chans[2] = { l, r };
        const bool ok = w->writeFromFloatArrays (chans, 2, n);
        w.reset();                                     // 析构时关闭并写出 WAV 头
        return ok;
    }

    void writeResults (juce::AudioFormatManager& mgr, const juce::File& src, double rate)
    {
        // 持有整块结果的 shared_ptr 快照：导出期间即使 sep 开始新一轮推理，
        // 缓冲也不会被释放或重分配（旧写法裸指针会悬空 → 堆损坏 / 爆音）
        const auto snap = sep.snapshot();
        const float* vL = snap.L();
        const float* vR = snap.R();
        const int   vlen = snap.length;
        if (vL == nullptr || vR == nullptr || vlen <= 0)
        {
            addError (src.getFileName() + Z("：分离结果为空"));
            return;
        }

        const bool wantVoc   = (mode != Mode::Instr);
        const bool wantInstr = (mode != Mode::Vocal);

        std::vector<float> mixL, mixR;
        if (wantInstr) decodeMix (mgr, src, mixL, mixR);

        // 人声与原曲按文件采样率对齐，长度取交集（重采样端点可能差几个样本）
        const int n = wantInstr ? (int) juce::jmin (mixL.size(), (size_t) vlen) : vlen;
        if (n <= 0)
        {
            addError (src.getFileName() + Z("：分离结果为空"));
            return;
        }

        const auto base = src.getFileNameWithoutExtension();
        int written = 0;

        if (wantVoc)
        {
            const auto f = getOutputDir().getChildFile (base + Z("_人声.wav"));
            if (writeWav (f, rate, vL, vR, n)) { addResult (f); ++written; }
        }

        if (wantInstr)
        {
            std::vector<float> insL ((size_t) n), insR ((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                insL[(size_t) i] = juce::jlimit (-1.0f, 1.0f, mixL[(size_t) i] - vL[i]);
                insR[(size_t) i] = juce::jlimit (-1.0f, 1.0f, mixR[(size_t) i] - vR[i]);
            }
            const auto f = getOutputDir().getChildFile (base + Z("_伴奏.wav"));
            if (writeWav (f, rate, insL.data(), insR.data(), n)) { addResult (f); ++written; }
        }

        if (written == 0)
            addError (src.getFileName() + Z("：WAV 写入失败"));
    }

    //==========================================================================
    static juce::String statusDefault() { return Z ("等待任务"); }

    SeparationEngine sep;                 // 独立的离线分离实例（与播放预分离互不影响）
    juce::Array<juce::File> queue;        // 仅工作线程启动时读取
    Mode mode = Mode::Both;
    int jobId = 0;

    std::atomic<int>  doneCount { 0 };
    std::atomic<int>  failCount { 0 };
    std::atomic<bool> cancelFlag { false };

    mutable juce::CriticalSection dataLock;
    juce::String statusText { Z ("等待任务") };
    juce::String firstError;
    juce::Array<juce::File> results;
    juce::File outDir;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AiExporter)
};
