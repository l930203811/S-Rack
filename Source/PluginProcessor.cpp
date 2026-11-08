#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "MFAudioFormat.h"

#include <cstdlib>

// MF 格式诊断回调 → mp_debug.log
std::function<void (const juce::String&)> mfDebugLog = nullptr;

//==============================================================================
// 临时诊断日志（默认写 D:\Xiangmu\S-Rack\mp_debug.log，MEDIAPLAY_LOG 可覆盖）
void MediaPlayAudioProcessor::debugLog (const juce::String& s)
{
    juce::String path = "D:\\Xiangmu\\S-Rack\\mp_debug.log";
    if (const char* p = std::getenv ("MEDIAPLAY_LOG"))
        path = juce::String::fromUTF8 (p);
    juce::File (path).appendText (
        juce::Time::getCurrentTime().toString (false, true) + "  " + s + "\n");
}
MediaPlayAudioProcessor::MediaPlayAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    // MF 必须最先注册：registerBasicFormats 自带的 WindowsMediaAudioFormat 内部也用 MF，
    // 但没显式声明输出采样率，遇到 HE-AAC 会解出 12000Hz 静音垃圾；且 JUCE 的 MP3 格式
    // 靠扫描帧同步头识别文件，会把某些 MP4 容器误认成 MP3。MF 按容器结构识别，全兜底。
    formatManager.registerFormat (new MFAudioFormat(), false);  // MP4/M4A/AAC（含伪装成 .mp3 的 MP4）/WMA/MP3
    formatManager.registerBasicFormats();                       // WAV / AIFF / FLAC / OGG / WMF
    formatManager.registerFormat (new juce::MP3AudioFormat(), false);
    mfDebugLog = [] (const juce::String& s) { debugLog (s); };

    // 宿主可能把 onnxruntime.dll 解析到 System32 的旧版本（1.5.2），导致
    // Ort::Global<void>::api_ 为空、首次构造 Ort::Env 时崩掉整个宿主进程。
    // 在插件实例创建时就完成重定向并留日志；失败也不抛异常——后台分离会
    // 据此跳过模型推理，播放与 UI 照常工作。
    (void) OrtRuntime::ensure();

    for (auto& b : bars)
        b.store (0.0f, std::memory_order_relaxed);

    // 曲尾切歌由处理器自身的定时器驱动：编辑器关闭（Timer 销毁）后仍继续工作。
    // Timer 回调经 MessageManager 投递到消息线程，宿主消息循环常驻，故与 UI 是否打开无关。
    startTimer (50);
}

MediaPlayAudioProcessor::~MediaPlayAudioProcessor()
{
    stopTimer();

    // 必须在宿主卸载插件模块之前停掉全部后台线程。DLL 被卸载后，仍在运行的
    // 推理线程会跳到已释放的代码地址 → 访问违例 → 宿主整个进程闪退。
    // 先统一发取消信号，让三个线程并行退出，再逐个等待（避免串行超时叠加）。
    hance.cancel();
    separation.cancel();
    aiExporter.shutdown();
    hance.shutdown();
    separation.shutdown();

    // 宿主销毁插件时音频回调已停止，可安全清理
    if (auto* r = pendingReader.exchange (nullptr))
        delete r;
    delete activeReader;
    activeReader = nullptr;
}

//==============================================================================
void MediaPlayAudioProcessor::timerCallback()
{
    if (trackEndedFlag.exchange (0) == 1)
        handleTrackEnd();

    // 「音频分离」导出收尾：WAV 已落盘 → 重新载入缓存 → 切换按钮立即可用。
    // 导出引擎线程退出即代表批次结束（状态文本里含完整输出目录）。
    if (preSepActive.load (std::memory_order_relaxed) && ! aiExporter.isBusy())
    {
        preSepActive.store (false, std::memory_order_relaxed);
        SeparationEngine::setThreadLimit (0);
        ensureVocalFor (curSepFile, curSepRate, curSepLen);
        if (vocalReady.load (std::memory_order_relaxed))
        {
            vocalInfo = zh ("分离完成 · 已生成缓存：") + getVocalCachePathText();
            debugLog ("[VOCAL] separation READY -> " + getVocalCachePathText());
        }
        else
        {
            vocalInfo = zh ("分离未成功 · ") + aiExporter.getStatusText()
                      + zh (" · 输出目录：") + getVocalCacheDir().getFullPathName();
            debugLog ("[VOCAL] separation FAILED: " + aiExporter.getStatusText());
        }
    }
}

//==============================================================================
// 宿主状态持久化：重启机架 / 重开工程后恢复播放列表、曲目、位置、模式、音量
//==============================================================================
void MediaPlayAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement xml ("MediaPlayState");
    xml.setAttribute ("playMode", (int) playMode);
    xml.setAttribute ("volume", (double) gain.load (std::memory_order_relaxed));
    xml.setAttribute ("inputThru", inputThru.load (std::memory_order_relaxed));
    xml.setAttribute ("currentTrack", currentTrack);
    xml.setAttribute ("position", currentSeconds.load (std::memory_order_relaxed));
    xml.setAttribute ("lyricsOn", lyricsOn.load (std::memory_order_relaxed));
    xml.setAttribute ("spectrumOn", spectrumOn.load (std::memory_order_relaxed));
    xml.setAttribute ("bypassed", bypassed.load (std::memory_order_relaxed));
    xml.setAttribute ("uiTab", uiTab.load (std::memory_order_relaxed));
    // 界面语言随宿主工程保存；老工程没有该属性时回落默认中文
    xml.setAttribute ("lang", L10n::get());

    auto* list = xml.createNewChildElement ("Playlist");
    for (const auto& f : playlist)
        list->createNewChildElement ("Track")->setAttribute ("path", f.getFullPathName());

    auto* offs = xml.createNewChildElement ("LyricOffsets");
    for (const auto& kv : lyricOffsets)
    {
        auto* item = offs->createNewChildElement ("Item");
        item->setAttribute ("path", kv.first);
        item->setAttribute ("sec", kv.second);
    }

    // 实时分离混音台：每路增益 / 灵敏度 / M / S
    // 不再保存总开关：是否处理由这些参数自动推导（全部熄灭 = 原声直通）
    auto* st = xml.createNewChildElement ("Stems");
    for (int i = 0; i < StemSeparator::numStems; ++i)
    {
        auto* s = st->createNewChildElement ("Stem");
        s->setAttribute ("gainDb", (double) stems.gainDb[(size_t) i].load (std::memory_order_relaxed));
        s->setAttribute ("sens",   (double) stems.sensitivity[(size_t) i].load (std::memory_order_relaxed));
        s->setAttribute ("mute",   stems.mute[(size_t) i].load (std::memory_order_relaxed));
        s->setAttribute ("solo",   stems.solo[(size_t) i].load (std::memory_order_relaxed));
    }

    copyXmlToBinary (xml, dest);
}

void MediaPlayAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName ("MediaPlayState"))
        return;

    // 停止播放，重建列表（保留全部条目以维持索引对齐；缺文件的条目切到时自动跳过加载）
    playing.store (false, std::memory_order_relaxed);
    playlist.clear();
    if (auto* list = xml->getChildByName ("Playlist"))
        for (auto* t : list->getChildWithTagNameIterator ("Track"))
            playlist.push_back (juce::File (t->getStringAttribute ("path")));

    playMode = (PlayMode) juce::jlimit (0, 3, xml->getIntAttribute ("playMode", (int) LoopAll));
    gain.store ((float) xml->getDoubleAttribute ("volume", 0.8), std::memory_order_relaxed);
    inputThru.store (xml->getBoolAttribute ("inputThru", true), std::memory_order_relaxed);
    lyricsOn.store (xml->getBoolAttribute ("lyricsOn", true), std::memory_order_relaxed);
    spectrumOn.store (xml->getBoolAttribute ("spectrumOn", true), std::memory_order_relaxed);
    bypassed.store (xml->getBoolAttribute ("bypassed", false), std::memory_order_relaxed);
    setUiTab (xml->getIntAttribute ("uiTab", 2));
    L10n::set (xml->getIntAttribute ("lang", 0));
    // 编辑器可能先于宿主恢复状态就已构造：timerCallback 会检测语言变化并刷新全部文案

    // 歌词偏移表先于恢复曲目解析，loadTrack 时才能应用
    lyricOffsets.clear();
    if (auto* offs = xml->getChildByName ("LyricOffsets"))
        for (auto* it : offs->getChildWithTagNameIterator ("Item"))
            lyricOffsets[it->getStringAttribute ("path")] = it->getDoubleAttribute ("sec", 0.0);

    // 实时分离混音台状态（旧工程没有 Stems 节点时保持默认中性）。
    // 旧版保存的 enabled 总开关已废弃，这里刻意忽略，避免旧工程把分离强制关掉。
    if (auto* st = xml->getChildByName ("Stems"))
    {
        int i = 0;
        for (auto* s : st->getChildWithTagNameIterator ("Stem"))
        {
            if (i >= StemSeparator::numStems) break;
            stems.gainDb[(size_t) i].store (
                (float) s->getDoubleAttribute ("gainDb", 0.0), std::memory_order_relaxed);
            stems.sensitivity[(size_t) i].store (
                (float) s->getDoubleAttribute ("sens", 0.0), std::memory_order_relaxed);
            stems.mute[(size_t) i].store (s->getBoolAttribute ("mute", false), std::memory_order_relaxed);
            stems.solo[(size_t) i].store (s->getBoolAttribute ("solo", false), std::memory_order_relaxed);
            ++i;
        }
    }

    const int cur = xml->getIntAttribute ("currentTrack", -1);
    currentTrack = -1;
    currentSeconds.store (0.0, std::memory_order_relaxed);
    lengthSeconds.store (0.0, std::memory_order_relaxed);

    // 恢复上次曲目（不自动播放），文件仍存在则预载 reader，可从上次位置继续
    if (cur >= 0 && cur < (int) playlist.size() && playlist[(size_t) cur].existsAsFile())
    {
        loadTrack (cur, false);
        const double pos = xml->getDoubleAttribute ("position", 0.0);
        if (pos > 0.5)
        {
            seekRequestSeconds.store (pos, std::memory_order_relaxed);
            currentSeconds.store (pos, std::memory_order_relaxed);
        }
    }

    debugLog (juce::String::formatted ("state restored: %d tracks, cur=%d, mode=%d",
                                       (int) playlist.size(), cur, (int) playMode));
}

//==============================================================================
void MediaPlayAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    smoothedGain = gain.load (std::memory_order_relaxed);
    fifoIndex = 0;
    fifo.fill (0.0f);
    masterSmooth = 0.0f;
    masterHold = 0.0f;
    masterLevel.store (0.0f, std::memory_order_relaxed);
    masterPeak.store  (0.0f, std::memory_order_relaxed);
    stems.prepare (sampleRate, samplesPerBlock);   // 重建频段表并清空 STFT 叠接缓冲
    debugLog (juce::String::formatted ("prepareToPlay sr=%.1f block=%d", sampleRate, samplesPerBlock));
}

void MediaPlayAudioProcessor::releaseResources() {}

bool MediaPlayAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // 效果器型插件：接受「输入 <= 输出」的任意布局，避免宿主因布局协商失败而不拉取音频
    const auto out = layouts.getMainOutputChannelSet();
    const auto in  = layouts.getMainInputChannelSet();
    return out.size() > 0 && in.size() <= out.size();
}

//==============================================================================
// 核心渲染：直接在音频线程流式解码（线性插值重采样到宿主采样率）
void MediaPlayAudioProcessor::renderAudio (juce::AudioBuffer<float>& buffer)
{
    const int numOut = buffer.getNumChannels();
    const int n = buffer.getNumSamples();
    const double devRate = getSampleRate();
    const double fileRate = activeReader->sampleRate;
    const juce::int64 fileLen = activeReader->lengthInSamples;
    const double ratio = fileRate / devRate;                    // 每 1 个输出样本需要的文件样本数

    // 音量平滑（只作用于播放器输出，不影响透传的通道输入）
    const float gTarget = gain.load (std::memory_order_relaxed);
    const float g0 = smoothedGain;
    const float g1 = g0 + (gTarget - g0) * 0.25f;
    smoothedGain = g1;

    // 解码窗口大小（含插值余量）
    const int need = (int) std::ceil ((double) n * ratio) + 3;
    if (scratch.getNumChannels() < (int) activeReader->numChannels || scratch.getNumSamples() < need)
        scratch.setSize ((int) activeReader->numChannels, need, false, false, true);

    auto startPos = (juce::int64) readerPos;
    const bool unknownLen = fileLen <= 0;   // 少数容器拿不到时长：顺序解码直到 EOF
    auto readable = unknownLen ? (juce::int64) need
                               : juce::jlimit<juce::int64> (0, (juce::int64) need, fileLen - startPos);
    scratch.clear();
    const double readT0 = juce::Time::getMillisecondCounterHiRes();
    if (readable > 0)
        activeReader->read (scratch.getArrayOfWritePointers(), scratch.getNumChannels(),
                            startPos, (int) readable);
    diagMaxReadMs = juce::jmax (diagMaxReadMs,
                                juce::Time::getMillisecondCounterHiRes() - readT0);

    const float frac0 = (float) (readerPos - (double) startPos);

    for (int ch = 0; ch < numOut; ++ch)
    {
        // 输出通道映射：单声道文件复制到所有输出，多声道取 min(ch, 源声道-1)
        const int src = (activeReader->numChannels == 1)
                            ? 0 : juce::jmin (ch, (int) activeReader->numChannels - 1);
        const float* s = scratch.getReadPointer (src);
        float* dst = buffer.getWritePointer (ch);

        for (int i = 0; i < n; ++i)
        {
            const double p = (double) frac0 + (double) i * ratio;
            const int idx = (int) p;
            const float fr = (float) (p - (double) idx);
            const float g = g0 + (g1 - g0) * ((float) i / (float) juce::jmax (1, n - 1));
            dst[i] += (s[idx] + (s[idx + 1] - s[idx]) * fr) * g;   // 叠加在透传输入之上
        }
    }

    // 真人声来源：VocalCache（AI 分离页导出的 WAV）优先，其次 MDX 预分离的内存结果。
    // 两者都按「文件采样率 + 绝对文件位置」存储，与上面解码用同一组插值参数读取，
    // 因此 instrumental = mix - vocal 精确成立，Solo 不会再混入伴奏。
    // Hance 的 center-pan DSP 估计（corr≈0.96 ≈ 原混音）已彻底退出实时路径。
    sepUsedThisBlock = false;
    // 每块先确保暂存存在并清零：分离结果不可用的块绝不能让上一块的人声参与
    // 「伴奏 = mix - vocal」减法；宿主改块大小时旧写法会跳过 clear()。
    if (vocalScratch.getNumChannels() < 2 || vocalScratch.getNumSamples() < n)
        vocalScratch.setSize (2, n, false, false, true);
    vocalScratch.clear();

    const VocalCache::Snapshot cSnap =
        (vocalTrackId.load (std::memory_order_acquire) == sepActiveId)
            ? vocalCache.snapshot() : VocalCache::Snapshot{};
    const SeparationEngine::VocalSnapshot mSnap =
        separation.isReadyFor (sepActiveId) ? separation.snapshot()
                                            : SeparationEngine::VocalSnapshot{};

    const float* vsrc[2] = { nullptr, nullptr };
    int vlen = 0;
    if (cSnap.length > 0 && cSnap.L() != nullptr && cSnap.R() != nullptr)
    {
        vsrc[0] = cSnap.L(); vsrc[1] = cSnap.R(); vlen = cSnap.length;
    }
    else if (mSnap.length > 0 && mSnap.L() != nullptr && mSnap.R() != nullptr)
    {
        vsrc[0] = mSnap.L(); vsrc[1] = mSnap.R(); vlen = mSnap.length;
    }

    if (numOut >= 2 && vlen > 0)
    {
        // 一次性取快照：整个 block 内指针与长度来自同一块不可变缓冲，
        // worker 中途替换 shared_ptr 也不会让音频线程读到已释放的堆块。
        for (int ch = 0; ch < 2; ++ch)
        {
            float* dst = vocalScratch.getWritePointer (ch);
            const float* src = vsrc[ch];
            for (int i = 0; i < n; ++i)
            {
                const double p = (double) frac0 + (double) i * ratio;
                const int idx = (int) p;
                const float fr = (float) (p - (double) idx);
                const juce::int64 absIdx = startPos + (juce::int64) idx;
                float v = 0.0f;
                if (absIdx >= 0 && absIdx + 1 < vlen)
                    v = src[absIdx] + (src[absIdx + 1] - src[absIdx]) * fr;
                const float g = g0 + (g1 - g0) * ((float) i / (float) juce::jmax (1, n - 1));
                dst[i] = v * g;
            }
        }
        sepUsedThisBlock = true;
    }

    readerPos += (double) n * ratio;

    // 进度 / 结束检测（EOF 标记边沿触发：只置位一次，避免编辑器重复切歌）
    if (! unknownLen)
    {
        currentSeconds.store (juce::jmin (readerPos / fileRate, (double) fileLen / fileRate),
                              std::memory_order_relaxed);
        if (readerPos >= (double) fileLen - 1.0)
        {
            if (! eofFlagFired)
            {
                eofFlagFired = true;
                trackEndedFlag.store (1, std::memory_order_relaxed);
            }
        }
        else
            eofFlagFired = false;
    }
    else
    {
        currentSeconds.store (readerPos / fileRate, std::memory_order_relaxed);

        // 未知时长：用 MF reader 的真实 EOF + 尾部静音持续 ~1.5s 判定曲尾切歌
        auto* mf = dynamic_cast<MFAudioFormatReader*> (activeReader);
        if (mf != nullptr && mf->isEof())
        {
            float tailPeak = 0.0f;
            for (int i = 0; i < need; i += 4)
                tailPeak = juce::jmax (tailPeak, std::abs (scratch.getSample (0, i)));
            if (tailPeak < 0.001f)
            {
                if (++silentEofBlocks > (int) (1.5 * devRate / (double) juce::jmax (1, n))
                    && ! eofFlagFired)
                {
                    eofFlagFired = true;
                    trackEndedFlag.store (1, std::memory_order_relaxed);
                }
            }
            else
                silentEofBlocks = 0;
        }
    }
}

void MediaPlayAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // 诊断：记录相邻两次音频回调的最大间隔（正常约 n/sampleRate 毫秒）。
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (diagLastBlockMs > 0.0)
        diagMaxGapMs = juce::jmax (diagMaxGapMs, nowMs - diagLastBlockMs);
    diagLastBlockMs = nowMs;

    const int numIn  = juce::jmin (getTotalNumInputChannels(), buffer.getNumChannels());
    const int numOut = buffer.getNumChannels();
    const int n = buffer.getNumSamples();
    sepUsedThisBlock = false;   // renderAudio 会按需置位

    // 1) 输入处理：透传开关关闭时屏蔽输入（只输出播放器，消除麦克风→扬声器反馈啸叫）；
    //    开启时即为效果器语义——in==out 时 JUCE 的 buffer 本身就承载通道输入，不清除即为透传，
    //    仅当输出通道多于输入时，把最后的输入通道补拷到其余输出。
    if (numIn == 0 || ! inputThru.load (std::memory_order_relaxed))
        buffer.clear();
    else
        for (int ch = numIn; ch < numOut; ++ch)
            buffer.copyFrom (ch, 0, buffer.getReadPointer (numIn - 1), n);

    // 旁通 = 仅透传输入，不渲染播放器（电平表仍跟随透传信号，便于机架监听）
    if (bypassed.load (std::memory_order_relaxed))
    {
        updateMasterMeter (buffer, n);
        return;
    }

    // 1) 接管消息线程送来的新曲目（含释放旧 reader）
    if (auto* incoming = pendingReader.exchange (nullptr))
    {
        delete activeReader;
        activeReader = incoming;
        readerPos = 0.0;
        silentEofBlocks = 0;
        eofFlagFired = false;
        // 换曲时清掉上一曲残留的人声缓冲；输出源权重按新曲目的缓存状态平滑收敛。
        vocalScratch.clear();
        // loadTrack 先写 pendingSepId 再发布 pendingReader，音频线程可能夹在
        // 两者之间先走 else-if 把 id 取走；此时 exchange 拿到 -1，绝不能用它
        // 覆盖 sepActiveId，否则模型人声永远不会匹配当前曲目（Solo 仍听到伴奏）。
        const int pendingId = pendingSepId.exchange (-1, std::memory_order_acquire);
        if (pendingId != -1)
            sepActiveId = pendingId;
        const double fr = activeReader->sampleRate;
        lengthSeconds.store (fr > 0.0 ? (double) activeReader->lengthInSamples / fr : 0.0,
                             std::memory_order_relaxed);
        currentSeconds.store (0.0, std::memory_order_relaxed);
        renderBlockCount = 0;
        debugLog (juce::String::formatted ("[audio] took reader sr=%.0f ch=%u len=%lld playing=%d devSR=%.1f numIn=%d numOut=%d",
                                           activeReader->sampleRate, activeReader->numChannels,
                                           (long long) activeReader->lengthInSamples,
                                           (int) playing.load(), getSampleRate(), numIn, numOut));
    }
    else if (pendingSepId.load (std::memory_order_acquire) != -1)
    {
        // 延迟启动的分离任务（导入/恢复后按下播放）：没有新 reader 接管，
        // 也要把新任务 id 配对给本 reader，否则模型人声永远不会匹配当前曲目。
        sepActiveId = pendingSepId.exchange (-1, std::memory_order_acquire);
    }

    // 2) 应用 seek 请求
    if (const double s = seekRequestSeconds.exchange (-1.0); s >= 0.0 && activeReader != nullptr)
    {
        readerPos = s * activeReader->sampleRate;
        currentSeconds.store (s, std::memory_order_relaxed);
        eofFlagFired = false;   // 手动 seek 后重新武装曲尾检测
    }

    // 3) 解码渲染
    bool renderedPlayerThisBlock = false;
    if (playing.load (std::memory_order_relaxed) && activeReader != nullptr
        && activeReader->sampleRate > 0.0)
    {
        renderAudio (buffer);
        renderedPlayerThisBlock = true;
        if (++renderBlockCount % 86 == 0)   // 约 1 秒一次
        {
            double pk = 0.0;
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                pk = juce::jmax (pk, (double) std::abs (buffer.getSample (0, i)));
            // 全数值格式，绝不用 %s（JUCE 8 在 Windows 走 _vsnwprintf，%s 期望
            // wchar_t*，传 char* 会按 UTF-16 解读并越界读，日志里的乱码即由此而来）。
            debugLog (juce::String::formatted (
                "[audio] render pos=%.1fs vol=%.2f chPeak=%.3f gapMax=%.1fms readMax=%.1fms "
                "sepId=%d useSep=%d V=%d vlen=%d pre=%d src=%d wI=%.2f wV=%.2f thru=%d",
                getCurrentPosition(), (double) gain.load(), pk,
                diagMaxGapMs, diagMaxReadMs,
                sepActiveId, (int) sepUsedThisBlock,
                (int) vocalReady.load (std::memory_order_relaxed),
                vocalCache.snapshot().length,
                (int) preSepActive.load (std::memory_order_relaxed),
                (int) outputSource.load (std::memory_order_relaxed),
                (double) diagInstrGain, (double) diagVg,
                (int) inputThru.load (std::memory_order_relaxed)));
            diagMaxGapMs = 0.0;
            diagMaxReadMs = 0.0;
        }
    }
    else if (playing.load (std::memory_order_relaxed))
    {
        static int waitLogged = 0;   // 进程级，仅诊断用
        if (waitLogged++ % 86 == 0)
            debugLog (juce::String::formatted ("[audio] PLAYING but NO RENDER (activeReader=%p devSR=%.1f bypass=%d)",
                                               (void*) activeReader, getSampleRate(),
                                               (int) bypassed.load()));
    }

    // 4) 音量已在 renderAudio 内平滑应用（只作用于播放器，不碰透传输入）

    // 4.5) 输出源切换（原声 / 伴奏 / 人声）
    //
    // 真人声 v 由 renderAudio 按绝对文件位置写进 vocalScratch，与本块解码出的
    // 原混音 mix 逐样本对齐，因此三种输出统一成一个公式：
    //     out = (mix - v) * wI + v * wV
    //   原声 (1,1) 精确等于 mix；伴奏 (1,0) 等于 mix - v；人声 (0,1) 等于 v。
    // 权重在音频线程 40ms 线性趋近目标，切换没有爆音；分离刚就绪时原声模式的
    // (1,1) 也保证输出与就绪前完全一致，不再需要任何交叉淡化。
    // 不再经过 stems / stemsInstr 的 STFT 维纳链：那条链给伴奏引入 OLA 延迟，
    // 与未延迟的人声相加正是过去「大片杂音」的来源。
    if (numOut >= 2 && renderedPlayerThisBlock && sepUsedThisBlock)
    {
        const int src = outputSource.load (std::memory_order_relaxed);
        const float tgtI = (src == Vocals) ? 0.0f : 1.0f;
        const float tgtV = (src == Accompaniment) ? 0.0f : 1.0f;
        const float step = (float) n / juce::jmax (1.0f, 0.04f * (float) getSampleRate());
        srcGainI += juce::jlimit (-step, step, tgtI - srcGainI);
        srcGainV += juce::jlimit (-step, step, tgtV - srcGainV);
        diagInstrGain = srcGainI;
        diagVg = srcGainV;

        for (int ch = 0; ch < 2; ++ch)
        {
            float* d = buffer.getWritePointer (ch);
            const float* v = vocalScratch.getReadPointer (ch);
            for (int i = 0; i < n; ++i)
                d[i] = (d[i] - v[i]) * srcGainI + v[i] * srcGainV;
        }

        // 电平表：混音台已不再显示，只保留人声轨的真实电平，其余恒为 0。
        // 旧实现照抄伴奏 DSP 的统计，「独奏人声时其他通道还在跳」即由此而来。
        double vsum = 0.0;
        const float* v0 = vocalScratch.getReadPointer (0);
        for (int i = 0; i < n; ++i)
        {
            const float s = v0[i] * srcGainV;
            vsum += (double) s * s;
        }
        const float vrms = (float) std::sqrt (vsum / (double) juce::jmax (1, n));
        const float vdb  = 20.0f * std::log10 (vrms * 1.6f + 1.0e-6f);
        const float vLv  = juce::jlimit (0.0f, 1.0f, (vdb + 60.0f) / 60.0f);
        for (int i = 0; i < StemSeparator::numStems; ++i)
        {
            const float lv = (i == StemSeparator::Vocals) ? vLv : 0.0f;
            stems.meterLevel[(size_t) i].store (lv, std::memory_order_relaxed);
            stems.meterPeak[(size_t) i].store (lv, std::memory_order_relaxed);
        }
    }
    else if (numOut >= 2 && renderedPlayerThisBlock)
    {
        // 尚无真人声：强制回到原声权重，buffer 保持解码出的原混音不动。
        // UI 同时会把「原声/伴奏/人声」按钮置灰，两者语义一致。
        srcGainI = 1.0f;
        srcGainV = 1.0f;
        diagInstrGain = 1.0f;
        diagVg = 1.0f;
        for (int i = 0; i < StemSeparator::numStems; ++i)
        {
            stems.meterLevel[(size_t) i].store (0.0f, std::memory_order_relaxed);
            stems.meterPeak[(size_t) i].store (0.0f, std::memory_order_relaxed);
        }
    }

    // 5) 频谱分析（单声道混合，取自最终输出）
    const auto* chL = buffer.getReadPointer (0);
    for (int i = 0; i < n; ++i)
    {
        float s = chL[i];
        if (buffer.getNumChannels() > 1)
            s = 0.5f * (s + buffer.getSample (1, i));
        pushSampleIntoFifo (s);
    }

    // 6) 总输出电平（在分离之后取样，反映真正送到扬声器的信号）
    updateMasterMeter (buffer, n);
}

//==============================================================================
// 总输出电平：RMS 为主、峰值辅助，dBFS → 0..1，快起慢落 + 峰值保持。
// 与 5 路电平同一套刻度（-60..0 dB），UI 各处可以直接复用同一绘制风格。
void MediaPlayAudioProcessor::updateMasterMeter (const juce::AudioBuffer<float>& buffer, int n)
{
    const int chs = juce::jmin (2, buffer.getNumChannels());
    const int count = juce::jmax (1, n * chs);
    double sum = 0.0;
    float peak = 0.0f;

    for (int ch = 0; ch < chs; ++ch)
    {
        const float* d = buffer.getReadPointer (ch);
        for (int i = 0; i < n; ++i)
        {
            const float v = d[i];
            sum += (double) v * v;
            peak = juce::jmax (peak, std::abs (v));
        }
    }

    const float rms = (float) std::sqrt (sum / (double) count);
    const float amp = juce::jmax (rms * 1.6f, peak * 0.55f);
    const float db  = 20.0f * std::log10 (amp + 1.0e-6f);
    const float target = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);

    const float cur  = masterSmooth;
    const float next = target > cur ? cur + 0.45f * (target - cur)   // 快起
                                    : cur + 0.08f * (target - cur);  // 慢落
    masterSmooth = next;

    float hold = masterHold;
    hold = (next > hold) ? next : juce::jmax (next, hold - 0.006f);
    masterHold = hold;

    masterLevel.store (next, std::memory_order_relaxed);
    masterPeak.store  (hold, std::memory_order_relaxed);
}

//==============================================================================
void MediaPlayAudioProcessor::pushSampleIntoFifo (float sample)
{
    fifo[(size_t) fifoIndex] = sample;
    if (++fifoIndex == fftSize)
    {
        fifoIndex = 0;
        computeSpectrum();
    }
}

void MediaPlayAudioProcessor::computeSpectrum()
{
    const auto sr = (float) getSampleRate();

    std::copy (fifo.begin(), fifo.end(), windowBuf.begin());
    window.multiplyWithWindowingTable (windowBuf.data(), (size_t) fftSize);

    // JUCE real-only FFT 的输入必须是【前 fftSize 个连续实数】；变换后才会把
    // 同一数组重新解释为交错的复数 bin (d[2k]=实部, d[2k+1]=虚部)。
    // 交错写入样本会造成 2 倍插零上采样 → 频谱镜像，柱形图完全失真。
    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] = windowBuf[(size_t) i];
    fft.performRealOnlyForwardTransform (fftData.data(), true);

    // bin 幅度（1024 个），映射到 48 根对数频率柱
    const int numBins = fftSize / 2;
    std::array<float, numBars> newBars {};
    const float minFreq = 30.0f, maxFreq = juce::jmin (16000.0f, sr * 0.5f);

    for (int b = 0; b < numBars; ++b)
    {
        float f0 = minFreq * std::pow (maxFreq / minFreq, (float) b / numBars);
        float f1 = minFreq * std::pow (maxFreq / minFreq, (float) (b + 1) / numBars);
        int bin0 = juce::jlimit (0, numBins - 1, (int) std::floor (f0 / (sr * 0.5f) * numBins));
        int bin1 = juce::jlimit (bin0 + 1, numBins, (int) std::ceil (f1 / (sr * 0.5f) * numBins));

        float sum = 0.0f;
        for (int k = bin0; k < bin1; ++k)
        {
            float re = fftData[(size_t) (2 * k)];
            float im = fftData[(size_t) (2 * k + 1)];
            sum += std::sqrt (re * re + im * im);
        }
        float mag = sum / (float) (bin1 - bin0);

        float db = juce::Decibels::gainToDecibels (mag / (float) numBins * 4.0f, -120.0f);
        newBars[(size_t) b] = juce::jlimit (0.0f, 1.0f, (db + 72.0f) / 60.0f);
    }
    for (int b = 0; b < numBars; ++b)
        bars[(size_t) b].store (newBars[(size_t) b], std::memory_order_relaxed);

    // 低 / 中 / 高 频段能量（用于节奏跳动：太阳=低频，山峦=中频，星星=高频）
    auto bandEnergy = [&] (float f0, float f1)
    {
        int bin0 = juce::jlimit (0, numBins - 1, (int) (f0 / (sr * 0.5f) * numBins));
        int bin1 = juce::jlimit (bin0 + 1, numBins, (int) (f1 / (sr * 0.5f) * numBins));
        float sum = 0.0f;
        for (int k = bin0; k < bin1; ++k)
        {
            float re = fftData[(size_t) (2 * k)];
            float im = fftData[(size_t) (2 * k + 1)];
            sum += std::sqrt (re * re + im * im);
        }
        float mag = (sum / (float) (bin1 - bin0)) / (float) numBins * 4.0f;
        float db  = juce::Decibels::gainToDecibels (mag, -120.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 48.0f);
    };

    lowEnergy.store  (bandEnergy (20.0f, 250.0f),    std::memory_order_relaxed);
    midEnergy.store  (bandEnergy (250.0f, 2000.0f),  std::memory_order_relaxed);
    highEnergy.store (bandEnergy (2000.0f, 12000.0f), std::memory_order_relaxed);

    spectrumActive.store (true, std::memory_order_relaxed);
}

//==============================================================================
// 播放列表 / 播放控制（消息线程）
//==============================================================================
void MediaPlayAudioProcessor::addFilesToPlaylist (const juce::Array<juce::File>& files, bool playFirstNew)
{
    int firstNew = (int) playlist.size();
    for (const auto& f : files)
        playlist.push_back (f);

    if (playFirstNew && ! playlist.empty())
        loadTrack (firstNew, true);
}

void MediaPlayAudioProcessor::clearPlaylist()
{
    playing.store (false, std::memory_order_relaxed);
    hance.cancel();
    separation.cancel();
    // 丢弃尚未被音频线程接管的 reader，避免泄漏
    if (auto* r = pendingReader.exchange (nullptr))
        delete r;

    playlist.clear();          // 之前只重置了状态，列表条目其实没被清空
    currentTrack = -1;
    seekRequestSeconds.store (-1.0, std::memory_order_relaxed);
    currentSeconds.store (0.0, std::memory_order_relaxed);
    lengthSeconds.store (0.0, std::memory_order_relaxed);
    trackEndedFlag.store (0, std::memory_order_relaxed);
    lyrics.clear();
}

void MediaPlayAudioProcessor::removeTrack (int index)
{
    if (index < 0 || index >= (int) playlist.size()) return;

    if (index == currentTrack)
    {
        const bool wasPlaying = isPlaying();
        playlist.erase (playlist.begin() + index);
        playing.store (false, std::memory_order_relaxed);

        if (playlist.empty())
        {
            clearPlaylist();
            return;
        }

        // 删掉正在播放的曲目：自动接上后一首（删末尾则回到新的最后一首），
        // 并保持原来的播放/暂停状态；loadTrack 失败时保持静音，不会继续放旧文件。
        currentTrack = -1;
        lyrics.clear();
        currentSeconds.store (0.0, std::memory_order_relaxed);
        lengthSeconds.store (0.0, std::memory_order_relaxed);
        trackEndedFlag.store (0, std::memory_order_relaxed);
        loadTrack (juce::jmin (index, (int) playlist.size() - 1), wasPlaying);
    }
    else
    {
        // 删除当前曲目之前的条目：索引左移，正在播放的文件不变
        if (index < currentTrack)
            --currentTrack;
        playlist.erase (playlist.begin() + index);
    }
}

void MediaPlayAudioProcessor::loadTrack (int index, bool andPlay)
{
    lyrics.clear();   // 无论成败，旧歌词先清掉，避免曲词错位

    if (index < 0 || index >= (int) playlist.size())
        return;

    // 先确保 reader 打开成功再切曲 —— 失败则保持现状，曲名与音频始终一致
    auto file = playlist[(size_t) index];
    auto* reader = formatManager.createReaderFor (file);
    if (reader == nullptr)
    {
        debugLog ("loadTrack FAILED (reader==null): " + file.getFullPathName());
        return;
    }

    lyrics.loadForFile (file);
    auto savedOff = lyricOffsets.find (file.getFullPathName());
    if (savedOff != lyricOffsets.end() && savedOff->second != 0.0)
        lyrics.applyTimeOffset (savedOff->second);   // 应用该曲记忆的偏移
    // JUCE 8 的 String::formatted 在 Windows 走 _vsnwprintf，%s 需要 wchar_t*；
    // 传 char* 会按 UTF-16 解读成乱码并越界读相邻内存（日志 "潬摡摥" 即由此而来）。
    // 一律改用 String 拼接，彻底避开变参类型陷阱。
    debugLog ("lyrics: "
              + juce::String (lyrics.isLoaded() ? "loaded" : "none")
              + " (" + juce::String (lyrics.hasWordTiming() ? "word" : "line") + ")"
              + ", lines=" + juce::String (lyrics.getLineCount()));

    debugLog (juce::String::formatted ("loadTrack ok idx=%d sr=%.0f ch=%u len=%lld (%.1fs)",
                                       index, reader->sampleRate, reader->numChannels,
                                       (long long) reader->lengthInSamples,
                                       reader->sampleRate > 0 ? reader->lengthInSamples / reader->sampleRate : 0.0));

    // 切歌 / 重新导入时先放弃上一曲的后台推理。否则旧任务会一直跑到整曲结束，
    // 暂停后 CPU/内存依然 100%，多切几首就会堆积多个满负荷推理线程。
    hance.cancel();
    separation.cancel();
    // 切歌时放弃上一曲尚未跑完的导出线程（不阻塞消息线程，线程自行退出）。
    aiExporter.cancelExport();
    preSepActive.store (false, std::memory_order_relaxed);
    SeparationEngine::setThreadLimit (0);
    // 每首曲目都回到「原声」，避免切到未分离的歌时突然只剩人声。
    outputSource.store (0, std::memory_order_relaxed);

    currentTrack = index;
    curSepFile = file;
    curSepRate = reader->sampleRate;
    curSepLen  = reader->lengthInSamples;

    // 每首曲目都分配独立的分离任务 id，并与 pendingReader 一起交给音频线程。
    // 无论是否立即播放都要发：只导入不播放时旧实现不更新 id，音频线程会一直
    // 拿上一曲的 id 去配对，导致新曲目的人声缓冲永远被判为「不匹配」。
    const int newId = ++sepTrackCounter;
    pendingSepId.store (newId, std::memory_order_release);
    vocalTrackId.store (newId, std::memory_order_release);

    // 真人声来源：优先读 AI 分离页导出的 <曲名>_人声.wav（消息线程，切歌时一次）。
    // 不再启动任何「实时 DSP 估计」模型 —— 那种估计 corr≈0.96，Solo 只会输出原曲。
    ensureVocalFor (file, reader->sampleRate, reader->lengthInSamples);

    // 覆盖前先释放尚未被音频线程接管的旧 reader（快速切歌/删歌时可能出现）
    if (auto* stale = pendingReader.exchange (reader, std::memory_order_relaxed))
        delete stale;
    seekRequestSeconds.store (-1.0, std::memory_order_relaxed);
    currentSeconds.store (0.0, std::memory_order_relaxed);

    if (andPlay)
        playing.store (true, std::memory_order_relaxed);
}

void MediaPlayAudioProcessor::play()
{
    debugLog ("UI: play() pressed");
    if (currentTrack < 0)
    {
        if (! playlist.empty())
            loadTrack (0, true);
        return;
    }

    // 播放到尾部后再按播放 → 从头开始
    const double len = lengthSeconds.load (std::memory_order_relaxed);
    const double pos = currentSeconds.load (std::memory_order_relaxed);
    if (len > 0.0 && pos >= len - 0.05)
        seekRequestSeconds.store (0.0, std::memory_order_relaxed);

    // 播放本身不启动任何模型：真人声来自磁盘缓存（VocalCache）或用户显式
    // 发起的 MDX 预分离。这样普通播放永远不会被后台推理抢 CPU。
    const bool anyReady = vocalReady.load (std::memory_order_relaxed)
                       || separation.isReadyFor (sepTrackCounter);
    debugLog (juce::String ("play(): vocalSource=") + (anyReady ? "ready" : "none")
              + juce::String (" preSep=") + (preSepActive.load() ? "running" : "idle"));

    playing.store (true, std::memory_order_relaxed);
}

//==============================================================================
// 缓存位置：UI 上的状态条、侧边栏、日志都从这里取，保证「文件在哪」永远一致。
juce::File MediaPlayAudioProcessor::getVocalCacheDir() const
{
    if (curSepFile == juce::File()) return {};
    const auto dir = curSepFile.getParentDirectory();
    // 用户直接试听导出的人声时，文件本身就在 AI分离输出 目录里，不要再套一层
    if (dir.getFileName() == zh ("AI分离输出")) return dir;
    return dir.getChildFile (zh ("AI分离输出"));
}

juce::File MediaPlayAudioProcessor::getVocalCacheFile() const
{
    if (curSepFile == juce::File()) return {};
    const auto hit = VocalCache::findVocalFile (curSepFile);
    if (hit != juce::File()) return hit;   // 已存在：显示真实命中的文件
    // 未分离：显示「点音频分离后将会生成」的完整路径
    return getVocalCacheDir().getChildFile (
        curSepFile.getFileNameWithoutExtension() + zh ("_人声.wav"));
}

juce::String MediaPlayAudioProcessor::getVocalCachePathText() const
{
    const auto f = getVocalCacheFile();
    return f == juce::File() ? zh ("（未加载曲目）") : f.getFullPathName();
}

//==============================================================================
// 切歌时加载真人声缓存（消息线程）
void MediaPlayAudioProcessor::ensureVocalFor (const juce::File& f, double rate, juce::int64 len)
{
    juce::String info;
    const bool ok = vocalCache.loadFor (f, rate, len, info);
    vocalReady.store (ok, std::memory_order_relaxed);
    vocalInfo = (ok ? zh ("已分离 · ") : zh ("尚未分离 · ")) + info
              + zh (" | 缓存：") + getVocalCachePathText();
    debugLog (juce::String ("[VOCAL] ") + (ok ? "cache hit: " : "no cache: ")
              + getVocalCachePathText());
}

//==============================================================================
// 用户点「音频分离」：后台跑一次 MDX-Net，并把 人声 / 伴奏 写成 WAV
// 落到 <曲目目录>/AI分离输出/。以前这一步只把结果留在内存里，重启即失效，
// 用户也完全不知道文件在哪；现在结果真实落盘、路径在 UI 上直接可见，
// 且下次切回这首歌（甚至重开宿主）会自动命中缓存，无需再分离。
void MediaPlayAudioProcessor::startPreSeparation()
{
    if (curSepFile == juce::File() || curSepRate <= 0.0)
    {
        debugLog ("[VOCAL] separation skipped: no track");
        return;
    }
    if (aiExporter.isBusy() || preSepActive.load (std::memory_order_relaxed))
    {
        debugLog ("[VOCAL] separation already running");
        return;
    }

    juce::Array<juce::File> files;
    files.add (curSepFile);
    if (! aiExporter.startExport (files, AiExporter::Mode::Both))
    {
        vocalInfo = zh ("上一轮分离正在退出，请稍后重试");
        debugLog ("[VOCAL] startExport refused (previous thread still running)");
        return;
    }
    preSepActive.store (true, std::memory_order_relaxed);
    vocalInfo = zh ("正在分离 · 将写入：") + getVocalCachePathText();
    debugLog ("[VOCAL] separation started -> " + getVocalCacheDir().getFullPathName()
              + " | " + curSepFile.getFileName());
}

void MediaPlayAudioProcessor::pause()
{
    debugLog ("UI: pause()");
    playing.store (false, std::memory_order_relaxed);
    hance.cancelPendingWork();
    // 用户显式发起的预分离不受暂停影响（它有可见进度，跑完即停），
    // 但普通播放绝不留下后台推理，避免「暂停后 CPU/内存 100%」。
    if (! preSepActive.load (std::memory_order_relaxed))
        separation.cancelPendingWork();
}

void MediaPlayAudioProcessor::stop()
{
    debugLog ("UI: stop()");
    playing.store (false, std::memory_order_relaxed);
    hance.cancelPendingWork();
    if (! preSepActive.load (std::memory_order_relaxed))
        separation.cancelPendingWork();
    seekRequestSeconds.store (0.0, std::memory_order_relaxed);
    currentSeconds.store (0.0, std::memory_order_relaxed);
}

int pickShuffle (int current, int size)
{
    if (size <= 1) return current;
    int n = current;
    juce::Random r = juce::Random::getSystemRandom();
    while (n == current)
        n = r.nextInt (size);
    return n;
}

void MediaPlayAudioProcessor::nextTrack (bool userTriggered)
{
    if (playlist.empty()) return;

    switch (playMode)
    {
        case LoopOne:
            if (userTriggered)
            {
                loadTrack ((currentTrack + 1) % (int) playlist.size(), true);
            }
            else
            {
                seekRequestSeconds.store (0.0, std::memory_order_relaxed);
                playing.store (true, std::memory_order_relaxed);
            }
            break;

        case LoopAll:
            loadTrack ((currentTrack + 1) % (int) playlist.size(), true);
            break;

        case Shuffle:
            loadTrack (pickShuffle (currentTrack, (int) playlist.size()), true);
            break;

        case Sequential:
        default:
            if (currentTrack + 1 < (int) playlist.size())
                loadTrack (currentTrack + 1, true);
            else
                stop();   // 播完列表即停
            break;
    }
}

void MediaPlayAudioProcessor::prevTrack()
{
    if (playlist.empty()) return;

    if (playMode == Shuffle)
    {
        loadTrack (pickShuffle (currentTrack, (int) playlist.size()), true);
        return;
    }

    const bool wasPlaying = isPlaying();

    // 播放超过 3 秒 → 先回到本曲开头
    if (getCurrentPosition() > 3.0)
    {
        setPositionSeconds (0.0);
        if (wasPlaying) playing.store (true, std::memory_order_relaxed);
        return;
    }

    const int n = (currentTrack <= 0 ? (int) playlist.size() - 1 : currentTrack - 1);
    loadTrack (n, wasPlaying);
}

void MediaPlayAudioProcessor::handleTrackEnd()
{
    nextTrack (false);
}

juce::String MediaPlayAudioProcessor::getCurrentTrackTitle() const
{
    if (currentTrack < 0 || currentTrack >= (int) playlist.size())
        return zh ("未加载曲目");
    return playlist[(size_t) currentTrack].getFileName();
}

juce::AudioProcessorEditor* MediaPlayAudioProcessor::createEditor()
{
    return new MediaPlayAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MediaPlayAudioProcessor();
}
