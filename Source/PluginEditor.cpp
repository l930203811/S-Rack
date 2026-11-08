#include "PluginEditor.h"

namespace
{
// 与 S-Rack 应用图标同款视觉的矢量 mini logo：
// 深蓝圆角方 + 机架细环 + 大号白色 S + 底部绿黄红 LED 频谱
void drawMiniAppIcon (juce::Graphics& g, juce::Rectangle<float> b)
{
    const float radius = b.getWidth() * 0.24f;

    juce::ColourGradient grad (juce::Colour (0xff203a8e), b.getTopLeft(),
                               juce::Colour (0xff050a20), b.getBottomRight(), false);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (b, radius);
    g.setColour (juce::Colour (0xff8ab8ff).withAlpha (0.50f));
    g.drawRoundedRectangle (b.reduced (0.5f), radius, 1.0f);

    const float cx = b.getCentreX();
    const float cy = b.getY() + b.getHeight() * 0.44f;
    const float r  = b.getWidth() * 0.30f;
    g.setColour (juce::Colour (0xff8ab8ff).withAlpha (0.38f));
    g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, 0.8f);

    // 深色描边：四个方向各偏移 0.6px 画一次，再叠白字
    g.setFont (juce::Font (juce::FontOptions ("Arial", b.getHeight() * 0.62f, juce::Font::bold)));
    auto sr = b.withTrimmedBottom (b.getHeight() * 0.13f);
    g.setColour (juce::Colour (0xff0a2140));
    for (int k = 0; k < 4; ++k)
    {
        const float dx = (k == 0 || k == 3) ? -0.6f : 0.6f;
        const float dy = (k < 2) ? -0.6f : 0.6f;
        g.drawText ("S", sr.translated (dx, dy), juce::Justification::centred);
    }
    g.setColour (juce::Colour (0xffeaf7ff));
    g.drawText ("S", sr, juce::Justification::centred);

    const juce::Colour led[] = { juce::Colour (0xff33d433),
                                 juce::Colour (0xffeed500),
                                 juce::Colour (0xffe4390e) };
    const float heights[] = { 0.11f, 0.17f, 0.12f };
    const float barW = b.getWidth() * 0.12f;
    const float gap  = b.getWidth() * 0.07f;
    float x = cx - (3.0f * barW + 2.0f * gap) * 0.5f;
    for (int i = 0; i < 3; ++i)
    {
        const float hh = b.getHeight() * heights[i];
        g.setColour (led[i]);
        g.fillRoundedRectangle (x, b.getBottom() - b.getHeight() * 0.055f - hh,
                                barW, hh, 0.8f);
        x += barW + gap;
    }
}
}

//==============================================================================
MediaPlayAudioProcessorEditor::MediaPlayAudioProcessorEditor (MediaPlayAudioProcessor& p)
    : AudioProcessorEditor (p), processorRef (p), visualizer (p), lyricsOverlay (p), playlistBox (p),
      sepStatus (p), offsetPopup (p), volumePopup (p), modePopup (p), sourcePopup (p)
{
    setOpaque (true);
    setLookAndFeel (&lookAndFeel);
    addMouseListener (&dismissListener, true);   // 监听所有子组件点击：点击弹窗外收起

    // 单一音频播放页：频谱 + 顶部状态条 + 歌词浮层（歌词浮层永远在最上层）
    addAndMakeVisible (visualizer);
    addAndMakeVisible (sepStatus);
    addAndMakeVisible (lyricsOverlay);
    lyricsOverlay.setInterceptsMouseClicks (false, false);   // 不挡下方控件交互

    // 音量：与「歌词偏移」同款——底部按钮 + 向上弹出的垂直调节器。
    // 原来的右侧常驻推子占用频谱右缘 56px，且样式与弹窗体系不统一。
    addAndMakeVisible (volumeBtn);
    volumeBtn.onClick = [this]
    {
        const bool show = ! volumePopup.isVisible();
        closeAllPopups();
        volumePopup.setVisible (show);
        syncPopupChrome();
    };

    addAndMakeVisible (volumePopup);
    volumePopup.setVisible (false);
    volumePopup.onVolumeChanged = [this] (float v)
    {
        processorRef.setVolume (v);
    };

    // 「分离音频」：跑一次 MDX，并把 人声 / 伴奏 写成 WAV 落到 AI分离输出 目录。
    // 按钮位于侧边栏「导入音频」正下方；文字随进度变化，完成后自动解锁输出源切换。
    addAndMakeVisible (sepBtn);
    sepBtn.onClick = [this] { processorRef.startPreSeparation(); };

    // 旁通（恢复）：机架里让宿主轨道输入原样通过，不叠加插件内置播放器的声音。
    // 状态随宿主持久化，因此这里读取而不是强制清零。
    addAndMakeVisible (bypassBtn);
    bypassBtn.setToggleState (processorRef.getBypassed(), juce::dontSendNotification);
    bypassBtn.onClick = [this]
    {
        processorRef.setBypassed (bypassBtn.getToggleState());
        lastTrackText = {};   // 立刻刷新侧边栏的旁通提示
    };

    // 中英文切换：默认简体中文；点击后立即重建全部界面文案
    addAndMakeVisible (langBtn);
    langBtn.onClick = [this]
    {
        MediaPlayAudioProcessor::setUiLanguage (
            MediaPlayAudioProcessor::isEnglish() ? 0 : 1);
        applyLanguage();
    };

    // 「原声」输出源切换：未分离时置灰不可点；已分离点击向上弹出三选项。
    addAndMakeVisible (sourceBtn);
    sourceBtn.onClick = [this]
    {
        if (! processorRef.hasVocalSource()) return;
        const bool show = ! sourcePopup.isVisible();
        closeAllPopups();
        sourcePopup.setVisible (show);
        syncPopupChrome();
    };

    addAndMakeVisible (sourcePopup);
    sourcePopup.setVisible (false);
    sourcePopup.onSourceChosen = [this] (int s)
    {
        processorRef.setOutputSource (s);
        sourceBtn.setButtonText (MediaPlayAudioProcessor::outputSourceName (s));
        closeAllPopups();
    };
    sourceBtn.setButtonText (
        MediaPlayAudioProcessor::outputSourceName (processorRef.getOutputSource()));

    // 侧边栏：当前曲目 + 分离状态（完整路径在主区状态条里，鼠标悬停也有 tooltip）
    trackLabel.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 10.5f, juce::Font::plain)));
    trackLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9dc0d0));
    trackLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (trackLabel);

    applyWindowTitle();

    // 歌词开关（默认开，状态随宿主持久化；仅「音频播放」页生效）
    addAndMakeVisible (lyricBtn);
    lyricBtn.setToggleState (processorRef.getLyricsEnabled(), juce::dontSendNotification);
    lyricBtn.onStateChange = [this]
    {
        processorRef.setLyricsEnabled (lyricBtn.getToggleState());
        lyricsOverlay.setVisible (lyricBtn.getToggleState());
        lyricBtn.repaint();
    };

    // 频谱开关（仅「音频播放」页生效，状态随宿主持久化）
    addAndMakeVisible (spectrumBtn);
    spectrumBtn.setToggleState (processorRef.getSpectrumEnabled(), juce::dontSendNotification);
    spectrumBtn.onStateChange = [this]
    {
        processorRef.setSpectrumEnabled (spectrumBtn.getToggleState());
        visualizer.setVisible (spectrumBtn.getToggleState());
        spectrumBtn.repaint();
    };

    // 歌词偏移：点击弹出参考线（中点=默认时间，上拉提前/下拉延后，±10s，按歌曲持久化）
    // 两个向上弹窗互斥，同一时刻只显示一个
    addAndMakeVisible (offsetBtn);
    offsetBtn.onClick = [this]
    {
        const bool show = ! offsetPopup.isVisible();
        closeAllPopups();
        offsetPopup.setVisible (show);
        if (show) offsetPopup.syncFromProcessor();
        syncPopupChrome();
    };

    addAndMakeVisible (offsetPopup);
    offsetPopup.setVisible (false);
    offsetPopup.onOffsetChanged = [this] (double v)
    {
        processorRef.setLyricOffsetAbsolute (v);
    };

    // 播放模式：点击向上弹出四选一列表
    addAndMakeVisible (modePopup);
    modePopup.setVisible (false);
    modePopup.onModeChosen = [this] (int m)
    {
        const auto pm = (MediaPlayAudioProcessor::PlayMode) m;
        processorRef.setPlayMode (pm);
        modeBtn.setButtonText (MediaPlayAudioProcessor::playModeName (pm));
        closeAllPopups();
    };

    addAndMakeVisible (openBtn);
    openBtn.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser> (
            MediaPlayAudioProcessor::zh ("选择音频文件（可多选）"), juce::File(),
            "*.mp3;*.wav;*.flac;*.ogg;*.aif;*.aiff;*.m4a;*.aac;*.mp4;*.wma");

        fileChooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems,
            [this] (const juce::FileChooser& fc)
            {
                auto results = fc.getResults();
                // 导入后不自动播放（自动播放会立即启动分离推理，拖垮系统 / 宿主），
                // 仅加入列表并加载选中，由用户手动按播放。
                if (! results.isEmpty())
                    processorRef.addFilesToPlaylist (results, false);
                fileChooser = nullptr;
            });
    };

    // 曲目列表（点击切歌；AI 分离页支持 Ctrl+多选）
    addAndMakeVisible (playlistBox);
    playlistBox.onTrackChosen = [this] (int row) { processorRef.loadTrack (row, true); };

    addAndMakeVisible (playBtn);
    playBtn.onClick = [this]
    {
        if (processorRef.isPlaying())
            processorRef.pause();
        else
            processorRef.play();
    };

    addAndMakeVisible (prevBtn);
    prevBtn.onClick = [this] { processorRef.prevTrack(); };

    addAndMakeVisible (nextBtn);
    nextBtn.onClick = [this] { processorRef.nextTrack (true); };

    // 播放模式按钮（文字胶囊显示当前模式，点击向上弹窗选择）
    addAndMakeVisible (modeBtn);
    modeBtn.setButtonText (MediaPlayAudioProcessor::playModeName (processorRef.getPlayMode()));
    modeBtn.onClick = [this]
    {
        const bool show = ! modePopup.isVisible();
        closeAllPopups();
        modePopup.setVisible (show);
        if (show) modePopup.syncFromProcessor();
        syncPopupChrome();
    };

    // 时间 / 进度
    timeLabel.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 12.5f, juce::Font::plain)));
    timeLabel.setColour (juce::Label::textColourId, juce::Colour (0xffcfe3ee));
    timeLabel.setText ("00:00 / 00:00", juce::dontSendNotification);
    addAndMakeVisible (timeLabel);

    seekSlider.setRange (0.0, 1.0, 0.001);
    seekSlider.onDragStart = [this] { seekDragging = true; };
    seekSlider.onDragEnd   = [this]
    {
        double len = processorRef.getLengthSeconds();
        if (len > 0.0)
        {
            // 留 0.5s 余量，避免拖到最右立刻触发切歌
            double target = juce::jmin ((double) seekSlider.getValue() * len, len - 0.5);
            processorRef.setPositionSeconds (juce::jmax (0.0, target));
        }
        seekDragging = false;
    };
    addAndMakeVisible (seekSlider);

    // 标题
    // 15pt：中文「S-Rack_媒体播放」与英文「S-Rack_Media Play」在 150px 内都能完整显示
    appTitle.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 15.0f, juce::Font::bold)));
    appTitle.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (appTitle);

    setSize (760, 560);
    applyTab();        // 单一页面：设置控件可见性与初始禁用状态
    applyLanguage();   // 所有控件注册完毕后再按当前语言统一重建文案
    startTimer (33);   // ~30fps

    MediaPlayAudioProcessor::debugLog ("=== S-Rack editor opened (single page + separation) ===");
}

MediaPlayAudioProcessorEditor::~MediaPlayAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

//==============================================================================
// 独立运行版窗口标题（机架 / DAW 里标题由宿主添加前缀，此处不影响宿主行为）
// 品牌名不随界面语言变化，中英文模式下都显示 S-Rack。
void MediaPlayAudioProcessorEditor::applyWindowTitle()
{
    if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
    {
        const juce::String title ("S-Rack");
        if (w->getName() != title)
            w->setName (title);
    }
}

void MediaPlayAudioProcessorEditor::parentHierarchyChanged()
{
    juce::AudioProcessorEditor::parentHierarchyChanged();
    applyWindowTitle();
}

//==============================================================================
// 页面已整合：不再有 tab 切换，这里只负责各控件的可见性与初始禁用状态。
void MediaPlayAudioProcessorEditor::applyTab()
{
    visualizer.setVisible (processorRef.getSpectrumEnabled());
    lyricsOverlay.setVisible (processorRef.getLyricsEnabled());
    spectrumBtn.setVisible (true);
    lyricBtn.setVisible (true);
    offsetBtn.setVisible (true);
    volumeBtn.setVisible (true);
    sepBtn.setVisible (true);
    bypassBtn.setVisible (true);
    langBtn.setVisible (true);
    sourceBtn.setVisible (true);
    trackLabel.setVisible (true);
    sepStatus.setVisible (true);
    bypassBtn.setToggleState (processorRef.getBypassed(), juce::dontSendNotification);
    sourceBtn.setEnabled (processorRef.hasVocalSource());
    repaint();
}

//==============================================================================
// 中英切换：zh() 接管语义，但按钮 / Label 的文字是构造时写入的静态副本，
// 必须在这里重建；paint() 内直接取 zh() 的面板只需 repaint()。
void MediaPlayAudioProcessorEditor::applyLanguage()
{
    const bool en = MediaPlayAudioProcessor::isEnglish();
    lastLanguage  = MediaPlayAudioProcessor::getUiLanguage();

    openBtn.setButtonText     (MediaPlayAudioProcessor::zh ("导入音频"));
    sepBtn.setButtonText      (MediaPlayAudioProcessor::zh ("分离音频"));
    bypassBtn.setButtonText   (MediaPlayAudioProcessor::zh ("旁通"));
    langBtn.setButtonText     (MediaPlayAudioProcessor::zh ("中 / EN"));
    langBtn.setToggleState    (en, juce::dontSendNotification);
    sourceBtn.setButtonText   (MediaPlayAudioProcessor::outputSourceName (processorRef.getOutputSource()));
    spectrumBtn.setButtonText (MediaPlayAudioProcessor::zh ("频谱"));
    lyricBtn.setButtonText    (MediaPlayAudioProcessor::zh ("歌词"));
    offsetBtn.setButtonText   (MediaPlayAudioProcessor::zh ("偏移"));
    volumeBtn.setButtonText   (MediaPlayAudioProcessor::zh ("音量"));
    modeBtn.setButtonText     (MediaPlayAudioProcessor::playModeName (processorRef.getPlayMode()));

    appTitle.setText (MediaPlayAudioProcessor::zh ("S-Rack_媒体播放"), juce::dontSendNotification);

    // 清空节流缓存：下一次 ~4Hz 刷新立刻用新语言重建动态文本
    lastSepBtnText = {};
    lastSourceText = {};
    lastTrackText  = {};
    lastMode       = -1;

    offsetPopup.repaint();
    volumePopup.repaint();
    modePopup.repaint();
    sourcePopup.repaint();
    sepStatus.applyLanguage();
    playlistBox.repaint();
    lyricsOverlay.repaint();
    repaint();
    applyWindowTitle();
}

//==============================================================================
void MediaPlayAudioProcessorEditor::paint (juce::Graphics& g)
{
    // 侧边栏背景
    g.fillAll (juce::Colour (0xff0a222d));
    g.setColour (juce::Colour (0xff05121a));
    g.fillRect (0, 0, 4, getHeight());

    // 左上角 logo：与 exe 应用图标同款（深蓝圆角方 + 播放圆盘 + 绿黄红 LED 频谱）
    drawMiniAppIcon (g, juce::Rectangle<float> (8.0f, 8.0f, 32.0f, 32.0f));

}

void MediaPlayAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    // 最右上角：「中 / EN」与「旁通」并排同一行（与顶部状态条同高）。
    // 两个按钮从右侧留 16px 边距向左侧排布，中间 6px 间距；状态条右缘给它们让位。
    const int mainX = 200;
    const int mainW = area.getWidth() - mainX;
    constexpr int hdrBtnW = 72, hdrBtnGap = 6, hdrBtnY = 3, hdrBtnH = 28;
    const int hdrRight = area.getWidth() - 16;
    const int hdrLeft  = hdrRight - hdrBtnW * 2 - hdrBtnGap;
    langBtn.setBounds   (hdrLeft, hdrBtnY, hdrBtnW, hdrBtnH);
    bypassBtn.setBounds (hdrLeft + hdrBtnW + hdrBtnGap, hdrBtnY, hdrBtnW, hdrBtnH);

    // 主区（x=200..760, y=0..486）：频谱铺底，顶部悬浮分离状态条，歌词浮层在最上
    visualizer.setBounds (mainX, 0, mainW, 486);
    sepStatus.setBounds (mainX + 8, 2,
                         juce::jmax (120, hdrLeft - 10 - (mainX + 8)), 34);
    // 歌词浮层加高到 96px（390..486，正好落在频谱倒影区），两行才能拉开净空不重叠
    lyricsOverlay.setBounds (216, 390, mainW - 16, 96);

    // 侧边栏（频谱左侧）：标题 / 导入音频 / 分离音频 / 曲目状态 / 播放模式 / 列表
    // 旁通已迁到右上角，与「中 / EN」同排；空出的高度还给多行曲目状态。
    appTitle.setBounds   (44, 12, 150, 24);
    openBtn.setBounds    (12, 44, 176, 26);
    sepBtn.setBounds     (12, 74, 176, 26);
    trackLabel.setBounds (12, 104, 176, 114);
    // 弹窗向上 166px → 顶边 y=56，仍在窗口内
    modeBtn.setBounds    (12, 222, 176, 26);
    playlistBox.setBounds (12, 252, 176, area.getHeight() - 252 - 12);

    // 底部传输区：时间 / 进度条 / 传输按钮
    const int rightEdge = area.getWidth() - 16;   // 744
    timeLabel.setBounds  (216, 486, 220, 14);
    seekSlider.setBounds (216, 502, rightEdge - 216, 16);
    prevBtn.setBounds (216, 520, 34, 34);
    playBtn.setBounds (266, 516, 42, 42);
    nextBtn.setBounds (324, 520, 34, 34);   // 右缘 358

    // 底部按钮组（从右往左）：音量 / 偏移 / 歌词 / 频谱 / 输出源
    // 「中 / EN」与「旁通」已迁到窗口最右上角，底部不再占用位置。
    volumeBtn.setBounds   (rightEdge - 56, 522, 56, 28);                  // 688
    offsetBtn.setBounds   (volumeBtn.getX() - 4 - 48, 522, 48, 28);        // 636
    lyricBtn.setBounds    (offsetBtn.getX() - 4 - 48, 522, 48, 28);        // 584
    spectrumBtn.setBounds (lyricBtn.getX() - 4 - 48, 522, 48, 28);         // 532
    sourceBtn.setBounds   (spectrumBtn.getX() - 4 - 76, 522, 76, 28);      // 452

    // 向上弹窗：宽度与触发按钮完全一致、底边贴住按钮上沿
    const auto placePopup = [] (juce::Component& popup, const juce::Component& btn, int h)
    {
        popup.setBounds (btn.getX(), btn.getY() - h, btn.getWidth(), h);
    };
    placePopup (offsetPopup, offsetBtn, 240);
    placePopup (volumePopup, volumeBtn, 240);
    placePopup (modePopup,   modeBtn,   166);
    placePopup (sourcePopup, sourceBtn, 158);   // 3 行 × 42 + 标题 26
}

//==============================================================================
void MediaPlayAudioProcessorEditor::timerCallback()
{
    // 语言可能由用户点击或宿主恢复状态改变；统一在这里检测并刷新全部文案
    if (MediaPlayAudioProcessor::getUiLanguage() != lastLanguage)
        applyLanguage();
    const bool bypassNow = processorRef.getBypassed();
    if (bypassBtn.getToggleState() != bypassNow)
        bypassBtn.setToggleState (bypassNow, juce::dontSendNotification);

    // 诊断自动加载：设置 MEDIAPLAY_AUTOLOAD 环境变量后 2 秒自动导入并播放
    if (! autoLoaded)
    {
        if (const char* al = std::getenv ("MEDIAPLAY_AUTOLOAD"))
        {
            if (juce::Time::getMillisecondCounterHiRes() - bootMs > 2000.0)
            {
                autoLoaded = true;
                juce::Array<juce::File> files;
                files.add (juce::File (juce::CharPointer_UTF8 (al)));
                MediaPlayAudioProcessor::debugLog ("[editor] auto load: " + juce::String (juce::CharPointer_UTF8 (al)));
                processorRef.addFilesToPlaylist (files, true);
            }
        }
        else
        {
            autoLoaded = true;
        }
    }

    // 诊断 seek 测试：MEDIAPLAY_SEEK=秒数，加载 5 秒后自动跳转（沙箱验证拖动 seek 用）
    if (autoLoaded && ! seekTested)
    {
        if (const char* sk = std::getenv ("MEDIAPLAY_SEEK"))
        {
            if (juce::Time::getMillisecondCounterHiRes() - bootMs > 5000.0)
            {
                seekTested = true;
                const double t = juce::String (sk).getDoubleValue();
                MediaPlayAudioProcessor::debugLog (
                    juce::String::formatted ("[editor] seek test -> %.1fs", t));
                processorRef.setPositionSeconds (t);
            }
        }
        else
        {
            seekTested = true;
        }
    }

    // 曲尾切歌由 Processor 自身的 Timer 处理，界面关闭后也能自动切歌
    // 播放/暂停图标状态
    playBtn.setIcon (processorRef.isPlaying() ? IconButton::Pause : IconButton::Play);

    // 播放模式文字同步（只在真的变化时重建字符串）
    const int mode = (int) processorRef.getPlayMode();
    if (mode != lastMode)
    {
        lastMode = mode;
        modeBtn.setButtonText (MediaPlayAudioProcessor::playModeName ((MediaPlayAudioProcessor::PlayMode) mode));
    }

    // 时间 / 进度
    double pos = processorRef.getCurrentPosition();
    double len = processorRef.getLengthSeconds();
    timeLabel.setText (formatTime (pos) + " / " + formatTime (len), juce::dontSendNotification);
    if (! seekDragging && len > 0.0)
        seekSlider.setValue (pos / len, juce::dontSendNotification);

    // 播放列表同步（条目数 / 当前曲目变化时才刷新）
    const int ps = processorRef.getPlaylistSize();
    if (ps != lastPlaylistSize)
    {
        lastPlaylistSize = ps;
        playlistBox.updateContent();
    }
    const int ct = processorRef.getCurrentTrackIndex();
    if (ct != lastCurTrack)
    {
        lastCurTrack = ct;
        playlistBox.repaint();
    }

    // 单一页面：频谱 / 歌词每帧驱动，状态文字一律降到 ~4Hz 更新
    if (visualizer.isVisible()) visualizer.timerTicked();
    if (lyricsOverlay.isVisible()) lyricsOverlay.repaint();     // ~30fps 驱动 KTV 渐染

    // 兜底：窗口挂载完成时可能还没找到 DocumentWindow，之后每秒校正一次标题
    if (++titleCheckTick % 30 == 0)
        applyWindowTitle();

    // 弹窗打开时同步处理器状态（切歌后偏移把手跟随 / 混音台改音量后把手跟随）
    if (offsetPopup.isVisible()) offsetPopup.syncFromProcessor();
    if (volumePopup.isVisible()) volumePopup.syncFromProcessor();

    // 分离状态（~4Hz）：按钮文字 / 输出源按钮可用性 / 状态条 / 侧边栏
    if (++sepTick % 8 == 0)
    {
        const bool canSwitch = processorRef.hasVocalSource();
        juce::String btnText;
        if (processorRef.isPreSeparating())
            btnText = MediaPlayAudioProcessor::zh ("分离中 ")
                    + juce::String (juce::roundToInt (processorRef.getPreSepProgress() * 100.0f)) + "%";
        else if (canSwitch)
            btnText = MediaPlayAudioProcessor::zh ("重新分离");
        else
            btnText = MediaPlayAudioProcessor::zh ("分离音频");
        if (btnText != lastSepBtnText)
        {
            lastSepBtnText = btnText;
            sepBtn.setButtonText (btnText);
        }

        // 当前曲目未分离 → 切换按钮置灰；已分离 → 可点并向上弹三选项。
        if (sourceBtn.isEnabled() != canSwitch)
        {
            sourceBtn.setEnabled (canSwitch);
            if (! canSwitch) closeAllPopups();   // 切到未分离的歌时收起弹窗
        }
        const auto srcText = MediaPlayAudioProcessor::outputSourceName (
            processorRef.getOutputSource());
        if (srcText != lastSourceText)
        {
            lastSourceText = srcText;
            sourceBtn.setButtonText (srcText);
        }

        sepStatus.refresh();   // 主区顶部：状态 + 缓存完整路径 + 打开目录

        // 侧边栏：当前曲目 + 分离状态 + 输出目录名（完整路径见状态条 / 悬停提示）
        juce::String t;
        if (processorRef.getBypassed())
        {
            // 旁通时收敛成三行，避免 84px 的标签被裁切
            t = MediaPlayAudioProcessor::zh ("当前曲目：\n")
              + processorRef.getCurrentTrackTitle() + "\n"
              + MediaPlayAudioProcessor::zh ("旁通中 · 仅透传宿主输入");
        }
        else
        {
            t = MediaPlayAudioProcessor::zh ("当前曲目：\n")
              + processorRef.getCurrentTrackTitle() + "\n";
            if (processorRef.isPreSeparating())
                t += MediaPlayAudioProcessor::zh ("分离中，请稍候…\n");
            else
                t += canSwitch ? MediaPlayAudioProcessor::zh ("✓ 已分离\n")
                               : MediaPlayAudioProcessor::zh ("尚未分离\n");
            t += MediaPlayAudioProcessor::zh ("输出目录：\n")
               + processorRef.getVocalCacheDir().getFileName()
               + MediaPlayAudioProcessor::zh ("\n（与源文件同目录）");
        }
        if (t != lastTrackText)
        {
            lastTrackText = t;
            trackLabel.setText (t, juce::dontSendNotification);
            trackLabel.setTooltip (processorRef.getVocalCachePathText());
        }
    }
}

//==============================================================================
void MediaPlayAudioProcessorEditor::closeAllPopups()
{
    offsetPopup.setVisible (false);
    volumePopup.setVisible (false);
    modePopup.setVisible (false);
    sourcePopup.setVisible (false);
    syncPopupChrome();
}

// 弹窗打开时，把对应按钮切成「基座」外观（上方直角 + accent 高亮）
void MediaPlayAudioProcessorEditor::syncPopupChrome()
{
    offsetBtn.setAttached (offsetPopup.isVisible());
    volumeBtn.setAttached (volumePopup.isVisible());
    modeBtn.setAttached   (modePopup.isVisible());
    sourceBtn.setAttached (sourcePopup.isVisible());
}

void MediaPlayAudioProcessorEditor::handleGlobalMouseDown (juce::Component* c)
{
    if (c == nullptr) return;

    const auto inside = [c] (juce::Component& comp)
    {
        return c == &comp || comp.isParentOf (c);
    };

    // 点击弹窗或它们的触发按钮时由按钮逻辑处理，其余区域一律收起
    if (inside (offsetPopup) || inside (volumePopup) || inside (modePopup) || inside (sourcePopup)
        || inside (offsetBtn) || inside (volumeBtn) || inside (modeBtn) || inside (sourceBtn)
        || inside (sepStatus))
        return;

    closeAllPopups();
}

juce::String MediaPlayAudioProcessorEditor::formatTime (double seconds)
{
    if (seconds < 0.0 || std::isnan (seconds) || std::isinf (seconds))
        seconds = 0.0;
    const int total = (int) seconds;
    if (total >= 3600)
        return juce::String::formatted ("%d:%02d:%02d", total / 3600, (total % 3600) / 60, total % 60);
    return juce::String::formatted ("%02d:%02d", total / 60, total % 60);
}
