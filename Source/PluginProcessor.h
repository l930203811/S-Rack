#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <map>
#include "Lyrics.h"
#include "StemSeparator.h"
#include "SeparationEngine.h"
#include "HanceEngine.h"
#include "AiExporter.h"
#include "VocalCache.h"
#include "Localization.h"

class MediaPlayAudioProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    MediaPlayAudioProcessor();
    ~MediaPlayAudioProcessor() override;

    // JUCE 的 String(const char*) 按 ASCII 逐字节解析，中文必须显式按 UTF-8 构造。
    // zh() 是全项目唯一的 UI 文本入口：中文模式原样返回，英文模式由 L10n 白名单替换。
    // 目录名 / 文件名类字符串不在白名单里，因此任何语言下路径都保持一致。
    static juce::String zh (const char* s)
    {
        if (const char* en = L10n::lookup (s))
            return juce::String (juce::CharPointer_UTF8 (en));
        return juce::String (juce::CharPointer_UTF8 (s));
    }

    //===== 界面语言（0=简体中文 默认，1=English）=====
    static int  getUiLanguage()       { return L10n::get(); }
    static void setUiLanguage (int l) { L10n::set (l); }
    static bool isEnglish()           { return L10n::isEnglish(); }

    // 临时诊断日志（设置 MEDIAPLAY_LOG 环境变量后启用，定位无声问题用）
    static void debugLog (const juce::String& s);

    //===== 播放模式 =====
    enum PlayMode { Sequential = 0, LoopAll, LoopOne, Shuffle };
    static juce::String playModeName (PlayMode m)
    {
        switch (m)
        {
            case Sequential: return zh ("顺序播放");
            case LoopAll:    return zh ("列表循环");
            case LoopOne:    return zh ("单曲循环");
            case Shuffle:    return zh ("随机播放");
        }
        return {};
    }

    //===== 频谱参数 =====
    static constexpr int fftOrder = 11;
    static constexpr int fftSize  = 1 << fftOrder;   // 2048
    static constexpr int numBars  = 48;              // 频谱柱数量

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    // 宿主插件列表里显示的产品名（品牌名，不随界面语言变化）
    const juce::String getName() const override { return juce::String ("S-Rack"); }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    // 播放列表 / 播放控制（在消息线程调用）
    void addFilesToPlaylist (const juce::Array<juce::File>& files, bool playFirstNew);
    void clearPlaylist();
    void removeTrack (int index);       // 删除指定条目（右键菜单用）
    void loadTrack (int index, bool andPlay);
    void play();
    void pause();
    void stop();
    void nextTrack (bool userTriggered);
    void prevTrack();
    void handleTrackEnd();          // 自然播放结束后按模式切歌
    void setPlayMode (PlayMode m)   { playMode = m; }
    PlayMode getPlayMode() const    { return playMode; }

    bool isPlaying() const            { return playing.load (std::memory_order_relaxed); }
    double getCurrentPosition() const { return currentSeconds.load (std::memory_order_relaxed); }
    double getLengthSeconds() const   { return lengthSeconds.load (std::memory_order_relaxed); }
    void setPositionSeconds (double t)
    {
        seekRequestSeconds.store (t, std::memory_order_relaxed);
        currentSeconds.store (t, std::memory_order_relaxed);   // UI 立即响应
    }

    juce::String getCurrentTrackTitle() const;
    int getCurrentTrackIndex() const { return currentTrack; }
    int getPlaylistSize() const { return (int) playlist.size(); }
    juce::String getPlaylistItem (int i) const { return playlist[(size_t) i].getFileName(); }
    juce::File   getPlaylistFile (int i) const { return playlist[(size_t) i]; }

    // 歌词（loadTrack 时自动加载同名 .lrc；不支持/不支持逐字时逐句显示）
    Lyrics& getLyrics()            { return lyrics; }
    bool getLyricsEnabled() const  { return lyricsOn.load (std::memory_order_relaxed); }
    void setLyricsEnabled (bool on){ lyricsOn.store (on, std::memory_order_relaxed); }

    // 可视化频谱开关（隐藏/显示整个可视化区域）
    bool getSpectrumEnabled() const  { return spectrumOn.load (std::memory_order_relaxed); }
    void setSpectrumEnabled (bool on){ spectrumOn.store (on, std::memory_order_relaxed); }

    // UI 已整合为单一「音频播放」页（无 tab 标题）。此字段仅为兼容旧宿主状态保留，
    // 恒为 1；编辑器不再据此切换页面。
    int getUiTab() const      { return 1; }
    void setUiTab (int)       {}

    //===== 输出源（原声 / 伴奏 / 人声）=====
    // 三者统一为 out = (mix - vocal) * wI + vocal * wV：
    //   原声 (1,1) 精确等于 mix；伴奏 (1,0)；人声 (0,1)。
    // 权重在音频线程 40ms 线性平滑，切换绝不产生爆音。
    // 没有真人声缓存时 audio 线程强制回 (1,1)，UI 也会把切换按钮置灰。
    enum OutputSource { Original = 0, Accompaniment = 1, Vocals = 2 };
    static juce::String outputSourceName (int s)
    {
        switch (s)
        {
            case Accompaniment: return zh ("伴奏");
            case Vocals:        return zh ("人声");
            default:            return zh ("原声");
        }
    }
    int  getOutputSource() const { return outputSource.load (std::memory_order_relaxed); }
    void setOutputSource (int s) { outputSource.store (juce::jlimit (0, 2, s), std::memory_order_relaxed); }

    //===== 分离缓存位置（UI 直接展示，用户不必再猜文件在哪）=====
    // 目录固定为 <当前曲目所在目录>/AI分离输出/，文件名为 <曲名>_人声.wav。
    juce::File   getVocalCacheDir()  const;   // 输出目录（不论是否已生成）
    juce::File   getVocalCacheFile() const;   // 已命中的缓存；没有则返回「将会生成」的路径
    juce::String getVocalCachePathText() const;

    //===== 实时音频分离（5 路软掩码 STFT；参数由混音台直接读写 atomic）=====
    // 无总开关：S / M / 推子 / 灵敏度全中性时自动走原声直通（见 StemSeparator::isActive）
    StemSeparator& getStems() { return stems; }
    const StemSeparator& getStems() const { return stems; }
    SeparationEngine& getSeparation() { return separation; }
    HanceEngine& getHance() { return hance; }
    AiExporter& getExporter() { return aiExporter; }

    //===== 总输出电平（音频线程写 / UI 只读，0..1 对应 -60..0 dBFS）=====
    float getMasterLevel() const { return masterLevel.load (std::memory_order_relaxed); }
    float getMasterPeak()  const { return masterPeak.load  (std::memory_order_relaxed); }

    // 歌词偏移（按歌曲路径记忆，持久化到宿主状态）：正 = 推迟，负 = 提前
    double getLyricOffsetOfCurrent() const
    {
        if (currentTrack < 0 || currentTrack >= (int) playlist.size()) return 0.0;
        auto it = lyricOffsets.find (playlist[(size_t) currentTrack].getFullPathName());
        return it != lyricOffsets.end() ? it->second : 0.0;
    }
    void adjustLyricOffset (double delta)
    {
        if (currentTrack < 0 || currentTrack >= (int) playlist.size()) return;
        auto& off = lyricOffsets[playlist[(size_t) currentTrack].getFullPathName()];
        off = juce::jlimit (-10.0, 10.0, off + delta);
        lyrics.applyTimeOffset (delta);
    }
    // 参考线拖动用：直接设定当前歌曲的歌词偏移（秒，正=推迟，±10s 封顶）
    void setLyricOffsetAbsolute (double v)
    {
        if (currentTrack < 0 || currentTrack >= (int) playlist.size()) return;
        const auto key = playlist[(size_t) currentTrack].getFullPathName();
        double cur = 0.0;
        if (auto it = lyricOffsets.find (key); it != lyricOffsets.end())
            cur = it->second;
        const double target = juce::jlimit (-10.0, 10.0, v);
        const double delta = target - cur;
        if (std::abs (delta) < 1e-9) return;
        lyricOffsets[key] = target;
        lyrics.applyTimeOffset (delta);
    }
    void resetLyricOffset()
    {
        if (currentTrack < 0 || currentTrack >= (int) playlist.size()) return;
        auto it = lyricOffsets.find (playlist[(size_t) currentTrack].getFullPathName());
        if (it != lyricOffsets.end())
        {
            lyrics.applyTimeOffset (-it->second);
            lyricOffsets.erase (it);
        }
    }

    // 音量 / 旁通
    void setVolume (float v)
    {
        // 音量弹窗/推子拖动时每帧都会调到这里；绝不在此写日志，
        // 否则一次拖动就往 mp_debug.log 刷上百行，拖慢消息线程。
        gain.store (v, std::memory_order_relaxed);
    }
    float getVolume() const   { return gain.load(); }
    void setBypassed (bool b)
    {
        debugLog (juce::String ("UI: setBypassed ") + (b ? "ON" : "OFF"));
        bypassed = b;
    }
    bool getBypassed() const  { return bypassed.load(); }

    // 输入透传：开 = 通道输入信号直达输出（机架唱歌用）；关 = 屏蔽输入，只输出播放器（消除啸叫用）
    void setInputThru (bool on)
    {
        debugLog (juce::String ("UI: setInputThru ") + (on ? "ON" : "OFF"));
        inputThru.store (on, std::memory_order_relaxed);
    }
    bool getInputThru() const { return inputThru.load (std::memory_order_relaxed); }

    //===== 分离状态：仅供 UI（消息线程）读取 =====
    // 「已分离」= 磁盘上的 <曲名>_人声.wav 已载入（或本曲内存结果仍有效）。
    bool  hasVocalSource() const
    {
        return vocalCache.isLoaded() || separation.isReadyFor (sepTrackCounter);
    }
    bool  isPreSeparating() const    { return preSepActive.load (std::memory_order_relaxed)
                                          || aiExporter.isBusy(); }
    float getPreSepProgress() const  { return aiExporter.overallProgress(); }
    juce::String getVocalInfo() const { return vocalInfo; }

    // 点「音频分离」：后台跑一次 MDX，并把 人声/伴奏 写成 WAV 落到
    // <曲目目录>/AI分离输出/，完成后自动载入缓存、切换按钮立即可用。
    void startPreSeparation();

    // 频谱数据（音频线程写，编辑器读）
    float getBar (int i) const     { return bars[(size_t) juce::jlimit (0, numBars - 1, i)].load (std::memory_order_relaxed); }
    float getLowEnergy() const     { return lowEnergy.load (std::memory_order_relaxed); }
    float getMidEnergy() const     { return midEnergy.load (std::memory_order_relaxed); }
    float getHighEnergy() const    { return highEnergy.load (std::memory_order_relaxed); }
    bool  isSpectrumActive() const { return spectrumActive.load (std::memory_order_relaxed); }

    std::atomic<int> trackEndedFlag { 0 };  // 音频线程置位，Processor Timer 处理（界面关闭也生效）

private:
    // 独立于编辑器的定时器：接管曲尾切歌，不依赖 UI 是否打开
    void timerCallback() override;

    //===== 播放列表 =====
    juce::AudioFormatManager formatManager;
    std::vector<juce::File> playlist;
    int currentTrack = -1;
    PlayMode playMode = LoopAll;
    Lyrics lyrics;                                     // 当前曲目歌词（消息线程访问）
    std::atomic<bool> lyricsOn { true };               // 歌词显示开关
    std::atomic<bool> spectrumOn { true };             // 可视化频谱显示开关
    std::atomic<int>  uiTab { 1 };                     // 兼容旧状态：恒为音频播放页
    std::map<juce::String, double> lyricOffsets;       // 按歌曲路径记忆的歌词偏移（秒）

    //===== 直读播放引擎（无 AudioTransportSource，宿主无关）=====
    // 消息线程只负责打开 reader 丢进 pendingReader；音频线程接管/释放/解码。
    std::atomic<juce::AudioFormatReader*> pendingReader { nullptr };
    juce::AudioFormatReader* activeReader = nullptr;   // 仅音频线程触碰
    juce::AudioBuffer<float> scratch;                  // 解码暂存窗（仅音频线程）
    double readerPos = 0.0;                            // 文件内采样点位置（音频线程）
    int silentEofBlocks = 0;                           // 未知时长文件的 EOF 静音计数（音频线程）
    bool eofFlagFired = false;                         // EOF 已置位标记（边沿触发，防止重复切歌）
    std::atomic<bool>   playing { false };
    std::atomic<double> seekRequestSeconds { -1.0 };
    std::atomic<double> currentSeconds { 0.0 };
    std::atomic<double> lengthSeconds  { 0.0 };

    std::atomic<float> gain { 0.8f };                  // 目标音量
    float smoothedGain = 0.8f;                         // 音频线程内平滑
    std::atomic<bool>  bypassed { false };
    std::atomic<bool>  inputThru { true };             // 输入透传开关（默认开）

    //===== FFT 频谱分析 =====
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize,
        juce::dsp::WindowingFunction<float>::hann };
    std::array<float, fftSize> fifo {};
    std::array<float, fftSize> windowBuf {};
    std::array<float, fftSize * 2> fftData {};
    int fifoIndex = 0;
    std::atomic<bool> spectrumActive { false };

    std::array<std::atomic<float>, numBars> bars {};
    std::atomic<float> lowEnergy { 0.0f };
    std::atomic<float> midEnergy { 0.0f };
    std::atomic<float> highEnergy { 0.0f };

    void pushSampleIntoFifo (float sample);
    void computeSpectrum();
    void renderAudio (juce::AudioBuffer<float>& buffer);
    void updateMasterMeter (const juce::AudioBuffer<float>& buffer, int numSamples);
    int renderBlockCount = 0;   // 诊断用

    // 总输出电平：统计最终立体声输出（已含音量、分离、输入透传）
    std::atomic<float> masterLevel { 0.0f };
    std::atomic<float> masterPeak  { 0.0f };
    float masterSmooth = 0.0f, masterHold = 0.0f;   // 音频线程独占

    //===== 实时音频分离 =====
    StemSeparator stems;

    //===== MDX-Net 模型人声分离（后台预分离 + 缓存播放）=====
    // 真人声只来自 VocalCache（AI 分离页导出的 WAV）或 separation 的 MDX 结果。
    // HanceEngine 的 center-pan DSP 估计（corr≈0.96 ≈ 原混音）已退出实时路径：
    // 它让 Solo 人声直接输出整首歌，是「独奏仍听到伴奏」的根因。
    SeparationEngine separation;
    AiExporter aiExporter;              // 分离导出引擎（独立 sep 实例，结果落盘）
    HanceEngine hance;                 // 保留但不再启动
    VocalCache vocalCache;             // 磁盘上的真人声缓存（消息线程加载）
    std::atomic<bool>  vocalReady { false };
    std::atomic<bool>  preSepActive { false };  // 一次「音频分离」导出进行中
    std::atomic<int>   vocalTrackId { -1 };   // 与 sepActiveId 配对，防止跨曲目错配
    juce::String vocalInfo;                    // 仅消息线程读写
    void ensureVocalFor (const juce::File& f, double rate, juce::int64 len);
    std::atomic<int> pendingSepId { -1 };   // 消息线程写（与 pendingReader 配对）
    int sepTrackCounter = 0;                // 消息线程递增的分离任务 id
    juce::File curSepFile;                  // 当前曲目分离任务参数
    double curSepRate = 0.0;
    juce::int64 curSepLen = 0;              // 当前曲目的文件采样数（重载缓存时校验时长用）

    // 输出源：消息线程写目标，音频线程持有平滑后的实际权重
    std::atomic<int> outputSource { 0 };    // 0 原声 / 1 伴奏 / 2 人声
    float srcGainI = 1.0f, srcGainV = 1.0f; // 音频线程独占（40ms 线性趋近）
    int sepActiveId = -1;                   // 音频线程：当前 reader 对应的分离任务 id
    bool sepUsedThisBlock = false;          // renderAudio 设置（音频线程）
    juce::AudioBuffer<float> vocalScratch;  // 设备采样率人声（音频线程）
    // 输出源不再经过 stemsInstr 的 STFT：伴奏由 mix - vocal 直接相减得到，
    // 零延迟、位精确，旧的 instrScratch / oldScratch / 淡化缓冲全部取消。

    // 音频线程诊断（仅音频线程读写，节流日志输出后归零）：
    // gapMax = 相邻音频回调最大间隔，readMax = MF 解码 read() 最大耗时。
    // gapMax 大而 readMax 小 ⇒ 后台线程抢占 CPU / 宿主调度饿死音频线程；
    // readMax 也大 ⇒ MF 解码本身在音频线程阻塞。
    double diagLastBlockMs = 0.0;
    double diagMaxGapMs = 0.0;
    double diagMaxReadMs = 0.0;
    float  diagInstrGain = 1.0f;
    float  diagVg = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MediaPlayAudioProcessor)
};
