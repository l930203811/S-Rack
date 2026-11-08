#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <array>
#include <cstdint>

//==============================================================================
// 实时音频分离面板：5 路混音台（人声 / 钢琴 / 贝斯 / 鼓组 / 其他）
//                  + 最右侧「总音量」列（与 5 路同款卡片 / 推子 / 电平柱）。
//
// 参考 Remix 的列布局纯自绘：垂直增益推子 + 数值框 + S/M 按钮 +
// 灵敏度旋钮 + 数值框。所有界面文案为中文，仅 S / M 保留机架通用英文缩写。
// 分轨参数直接读写 StemSeparator 的 atomic；总音量走 Processor 的 gain，
// 电平取 getMasterLevel()/getMasterPeak()，天然与音频线程同步。
//==============================================================================
class StemPanel : public juce::Component
{
public:
    static constexpr int numColumns = StemSeparator::numStems + 1;
    static constexpr int masterCol  = StemSeparator::numStems;

    explicit StemPanel (MediaPlayAudioProcessor& p)
        : proc (p), sep (p.getStems()) {}

    // 面板顶部预留给状态提示条 + 歌词窄条的高度，混音台内容从此处开始
    void setContentTop (int y) { contentTop = y; repaint(); }

    // 由编辑器定时器调用：参数或电平变化时才重绘，避免静止时无谓刷新
    void refreshIfChanged()
    {
        auto sig = signature();
        for (int i = 0; i < StemSeparator::numStems; ++i)
        {
            sig = sig * 131u + (std::uint64_t) (int)
                  (sep.meterLevel[(size_t) i].load (std::memory_order_relaxed) * 200.0f);
            sig = sig * 131u + (std::uint64_t) (int)
                  (sep.meterPeak[(size_t) i].load (std::memory_order_relaxed) * 100.0f);
        }
        // 总音量列：音量值与主输出电平也参与重绘判定
        sig = sig * 131u + (std::uint64_t) (int) (proc.getVolume() * 1000.0f);
        sig = sig * 131u + (std::uint64_t) (int) (proc.getMasterLevel() * 200.0f);
        sig = sig * 131u + (std::uint64_t) (int) (proc.getMasterPeak()  * 100.0f);
        // 分离进度参与重绘判定
        auto& sepEng = proc.getSeparation();
        if (sepEng.getRequestedTrackId() == proc.getCurrentTrackIndex())
            sig = sig * 131u + (std::uint64_t) (int) (sepEng.getProgress() * 500.0f)
                + (std::uint64_t) (int) sepEng.getStatus();
        if (sig != lastSig) { lastSig = sig; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        const float w = (float) getWidth();
        const float h = (float) getHeight();

        // ---- 背景 ----
        juce::ColourGradient bg (juce::Colour (0xff0a2230), 0.0f, 0.0f,
                                 juce::Colour (0xff040d16), w * 0.35f, h, false);
        g.setGradientFill (bg);
        g.fillAll();

        // ---- 顶部状态提示条（0..34，无主标题）----
        drawHeader (g, w);

        // 歌词已移到混音台下方，此处不再保留顶部歌词衬底条。

        // ---- 5 路混音台 + 最右侧总音量列 ----
        const auto L = layout();
        for (int i = 0; i < StemSeparator::numStems; ++i)
            drawColumn (g, L[(size_t) i], i);
        drawMasterColumn (g, L[(size_t) masterCol]);

        // ---- 底部中文操作提示 ----
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 11.0f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff6f93a5));
        g.drawText (z ("拖动推子调增益 / 总音量 · 滚轮微调 · 双击归位 · S 独奏 / M 静音 · 旋钮调灵敏度 · 复位回到原声"),
                    juce::Rectangle<float> (0.0f, h - 22.0f, w, 18.0f),
                    juce::Justification::centred);
    }

    //==========================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto p = e.getPosition().toFloat();
        int col = -1;
        hit = hitTest (p, col);
        activeCol = col;

        if (hit == Hit::Reset) { resetAll(); hit = Hit::None; return; }
        if (hit == Hit::None || col < 0) return;

        dragStartY = p.y;
        if (col < StemSeparator::numStems)
        {
            dragStartGain = sep.gainDb[(size_t) col].load (std::memory_order_relaxed);
            dragStartSens = sep.sensitivity[(size_t) col].load (std::memory_order_relaxed);
        }

        switch (hit)
        {
            case Hit::Solo:
                sep.solo[(size_t) col].store (! sep.solo[(size_t) col].load (std::memory_order_relaxed),
                                              std::memory_order_relaxed);
                hit = Hit::None;
                break;
            case Hit::Mute:
                sep.mute[(size_t) col].store (! sep.mute[(size_t) col].load (std::memory_order_relaxed),
                                              std::memory_order_relaxed);
                hit = Hit::None;
                break;
            case Hit::Gain:        applyGainAt (col, p.y); break;
            case Hit::Sens:        break;   // 等 mouseDrag 再按位移调整
            case Hit::MasterGain:  applyMasterVolumeAt (p.y); break;
            case Hit::MasterMute:  toggleMasterMute(); hit = Hit::None; break;
            case Hit::MasterFull:  setMasterVolume (1.0f); hit = Hit::None; break;
            default: break;
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (hit == Hit::None || activeCol < 0) return;
        const auto p = e.getPosition().toFloat();

        if (hit == Hit::Gain)
            applyGainAt (activeCol, p.y);
        else if (hit == Hit::Sens)
            setSens (activeCol, dragStartSens + (dragStartY - p.y) * 0.9f);
        else if (hit == Hit::MasterGain)
            applyMasterVolumeAt (p.y);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        hit = Hit::None;
        activeCol = -1;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        int c = -1;
        const auto h = hitTest (e.getPosition().toFloat(), c);
        if (h != hover || c != hoverCol) { hover = h; hoverCol = c; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hover = Hit::None; hoverCol = -1; repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        int col = -1;
        const auto h = hitTest (e.getPosition().toFloat(), col);
        if (col < 0 || (h != Hit::Gain && h != Hit::Sens && h != Hit::MasterGain)) return;
        const float dir = wheel.deltaY > 0.0f ? 1.0f : -1.0f;

        if (h == Hit::MasterGain)
            setMasterVolume (proc.getVolume() + dir * 0.02f);
        else if (h == Hit::Gain)
            setGain (col, sep.gainDb[(size_t) col].load (std::memory_order_relaxed) + dir * 1.0f);
        else
            setSens (col, sep.sensitivity[(size_t) col].load (std::memory_order_relaxed) + dir * 2.0f);
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        int col = -1;
        const auto h = hitTest (e.getPosition().toFloat(), col);
        if (col < 0) return;
        if (h == Hit::Gain)        setGain (col, 0.0f);
        if (h == Hit::Sens)        setSens (col, 0.0f);
        if (h == Hit::MasterGain)  setMasterVolume (1.0f);
        repaint();
    }

private:
    //==========================================================================
    static juce::String z (const char* s) { return MediaPlayAudioProcessor::zh (s); }

    static const char* stemName (int i)
    {
        static const char* n[] = { "人声", "钢琴", "贝斯", "鼓组", "其他" };
        return n[juce::jlimit (0, StemSeparator::numStems - 1, i)];
    }
    static const char* stemHint (int i)
    {
        static const char* n[] = { "中置主唱", "键盘与和声", "低频声部", "节奏与打击", "环境与残留" };
        return n[juce::jlimit (0, StemSeparator::numStems - 1, i)];
    }
    static juce::Colour stemColour (int i)
    {
        const juce::Colour c[] = {
            juce::Colour (0xff29b6f6), juce::Colour (0xffa78bfa),
            juce::Colour (0xffeecb3b), juce::Colour (0xfff08a0c), juce::Colour (0xff8fb0bf)
        };
        return c[juce::jlimit (0, StemSeparator::numStems - 1, i)];
    }

    enum class Hit { None, Gain, Sens, Solo, Mute, Reset,
                     MasterGain, MasterMute, MasterFull };

    struct Col
    {
        juce::Rectangle<float> card, nameBar, gainLabel, gainTrack, gainValue,
                               meter, meterScale,
                               solo, mute, sensLabel, knob, sensValue;
    };

    std::array<Col, numColumns> layout() const
    {
        const float w = (float) getWidth();
        const float h = (float) getHeight();
        const float y0 = (float) contentTop;
        const float pad = 8.0f, gap = 4.0f;
        const float cw = (w - 2.0f * pad - (float) (numColumns - 1) * gap) / (float) numColumns;

        // 弹性拉伸：面板更高（铺满右侧区域）时把推子 / 电平柱拉长，
        // 卡片整体随之下移加高，底部仍保留中文操作提示条。
        const float extra = juce::jmax (0.0f, (h - 26.0f) - (y0 + 340.0f));
        const float trackH = 140.0f + extra;

        std::array<Col, numColumns> out {};
        for (int i = 0; i < numColumns; ++i)
        {
            const float x = pad + (float) i * (cw + gap);
            auto& c = out[(size_t) i];
            c.card       = { x, y0 - 4.0f, cw, 344.0f + extra };
            c.nameBar    = { x, y0 + 2.0f, cw, 32.0f };
            c.gainLabel  = { x + 4.0f, y0 + 36.0f, cw - 8.0f, 14.0f };
            // 窄列布局：dB 刻度 / 推子 / 电平柱 / dB 刻度带按 88px 列宽重新排布
            c.gainTrack  = { x + 24.0f, y0 + 52.0f, 10.0f, trackH };
            c.meter      = { x + 40.0f, y0 + 52.0f, 11.0f, trackH };
            c.meterScale = { x + 54.0f, y0 + 52.0f,
                             (x + cw - 4.0f) - (x + 54.0f), trackH };
            c.gainValue  = { x + 8.0f, y0 + 196.0f + extra, cw - 16.0f, 22.0f };
            c.solo       = { x + cw * 0.5f - 31.0f, y0 + 222.0f + extra, 28.0f, 22.0f };
            c.mute       = { x + cw * 0.5f + 3.0f,  y0 + 222.0f + extra, 28.0f, 22.0f };
            c.sensLabel  = { x + 4.0f, y0 + 248.0f + extra, cw - 8.0f, 14.0f };
            c.knob       = { x + cw * 0.5f - 27.0f, y0 + 264.0f + extra, 54.0f, 54.0f };
            c.sensValue  = { x + 8.0f, y0 + 320.0f + extra, cw - 16.0f, 22.0f };
        }
        return out;
    }

    juce::Rectangle<float> resetRect() const
    {
        return { (float) getWidth() - 82.0f, 6.0f, 70.0f, 23.0f };
    }

    Hit hitTest (juce::Point<float> p, int& col) const
    {
        col = -1;
        if (resetRect().contains (p)) return Hit::Reset;

        const auto L = layout();
        for (int i = 0; i < numColumns; ++i)
        {
            const auto& c = L[(size_t) i];
            if (! c.card.contains (p)) continue;
            col = i;
            if (i == masterCol)
            {
                if (c.solo.contains (p)) return Hit::MasterMute;
                if (c.mute.contains (p)) return Hit::MasterFull;
                if (c.gainValue.contains (p)) return Hit::MasterGain;
                if (c.gainTrack.expanded (5.0f, 4.0f).contains (p)) return Hit::MasterGain;
                return Hit::None;
            }
            if (c.solo.contains (p))       return Hit::Solo;
            if (c.mute.contains (p))       return Hit::Mute;
            if (c.knob.contains (p) || c.sensValue.contains (p)) return Hit::Sens;
            if (c.gainValue.contains (p))  return Hit::Gain;
            if (c.gainTrack.expanded (5.0f, 4.0f).contains (p)) return Hit::Gain;
            return Hit::None;
        }
        return Hit::None;
    }

    //==========================================================================
    void drawHeader (juce::Graphics& g, float w) const
    {
        g.setColour (juce::Colour (0xff071a26));
        g.fillRect (0.0f, 0.0f, w, 34.0f);
        g.setColour (juce::Colour (0xff1b3c4c).withAlpha (0.7f));
        g.fillRect (0.0f, 33.0f, w, 1.0f);

        // 迷你 LED 频谱装饰（与 exe 图标同一套绿黄红）
        const juce::Colour led[] = { juce::Colour (0xff33d433), juce::Colour (0xff8edb1e),
                                     juce::Colour (0xffeed500), juce::Colour (0xfff08a0c),
                                     juce::Colour (0xffe4390e) };
        const float hh[] = { 7.0f, 12.0f, 17.0f, 11.0f, 6.0f };
        for (int i = 0; i < 5; ++i)
        {
            g.setColour (led[i]);
            g.fillRoundedRectangle (14.0f + (float) i * 6.0f, 24.0f - hh[i], 4.0f, hh[i], 1.0f);
        }

        // 不再有「实时音频分离」大标题：改成本面板唯一需要用户知道的操作说明
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 11.5f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff9dc0d0));

        // AI 模型分离状态（针对当前曲目）：进行中显示进度，失败给出提示
        auto& sepEng = proc.getSeparation();
        const int sepStat = (int) sepEng.getStatus();
        const bool sepHere = sepEng.getRequestedTrackId() == proc.getCurrentTrackIndex();
        juce::String hintText (z ("点亮 S 独奏音轨 · S 全灭零延迟原声 · 复位一键还原"));
        if (sepHere && sepStat == (int) SeparationEngine::Working)
        {
            const int pct = (int) (sepEng.getProgress() * 100.0f);
            hintText = juce::String::formatted (z ("AI 智能分离中 %d%%（完成后人声轨更纯净）"), pct);
        }
        else if (sepHere && sepStat == (int) SeparationEngine::Failed)
            hintText = z ("AI 智能分离失败（将使用实时掩码模式）");
        else if (sepHere && sepStat == (int) SeparationEngine::Ready)
            hintText = z ("AI 智能分离就绪 · 人声轨为模型输出");

        g.drawText (hintText,
                    juce::Rectangle<float> (52.0f, 3.0f, (float) getWidth() - 240.0f, 28.0f),
                    juce::Justification::centredLeft);

        // 进度细条
        if (sepHere && sepStat == (int) SeparationEngine::Working)
        {
            const float pw = (float) getWidth() - 240.0f;
            const float px = 52.0f;
            const float py = 30.0f;
            g.setColour (juce::Colour (0xff10303e));
            g.fillRoundedRectangle (px, py, pw, 2.5f, 1.2f);
            g.setColour (juce::Colour (0xff29b6f6));
            g.fillRoundedRectangle (px, py, pw * juce::jlimit (0.0f, 1.0f, sepEng.getProgress()), 2.5f, 1.2f);
        }

        // 状态由参数自动推导，无需手动开关
        const bool on = sep.isActive();
        g.setColour (on ? juce::Colour (0xff33d433) : juce::Colour (0xff6f93a5));
        g.fillEllipse ((float) getWidth() - 172.0f, 15.0f, 6.0f, 6.0f);
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 10.5f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff9dc0d0));
        g.drawText (on ? z ("分离处理中") : z ("零延迟直通"),
                    juce::Rectangle<float> ((float) getWidth() - 162.0f, 4.0f, 74.0f, 26.0f),
                    juce::Justification::centredLeft);

        const auto r = resetRect();
        const bool over = hover == Hit::Reset;
        g.setColour (over ? juce::Colour (0xff164a63) : juce::Colour (0xff10303e));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (juce::Colour (0xff29b6f6).withAlpha (0.55f));
        g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 11.5f, juce::Font::plain)));
        g.setColour (juce::Colours::white);
        g.drawText (z ("复位"), r, juce::Justification::centred);
    }

    void drawColumn (juce::Graphics& g, const Col& c, int i) const
    {
        // 原声直通时各路电平柱依然随音乐跳动，列保持常亮，不再整体变灰
        const auto accent = stemColour (i);

        // 卡片
        g.setColour (juce::Colour (0xff0b202c));
        g.fillRoundedRectangle (c.card, 7.0f);
        g.setColour (juce::Colour (0xff1d3d4c));
        g.drawRoundedRectangle (c.card.reduced (0.5f), 7.0f, 1.0f);

        // 顶部色条 + 中文名 + 中文副标题
        g.setColour (accent.withAlpha (0.95f));
        g.fillRoundedRectangle (juce::Rectangle<float> (c.nameBar.getX() + 8.0f,
                                                        c.nameBar.getY(),
                                                        c.nameBar.getWidth() - 16.0f, 3.0f), 1.5f);
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 15.0f, juce::Font::bold)));
        g.setColour (juce::Colours::white);
        g.drawText (z (stemName (i)),
                    c.nameBar.withTrimmedTop (5.0f).withHeight (18.0f),
                    juce::Justification::centred);
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 9.5f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff6f93a5));
        g.drawText (z (stemHint (i)),
                    c.nameBar.withTrimmedTop (22.0f).withHeight (12.0f),
                    juce::Justification::centred);

        // 增益标签 + 右侧电平标题
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 10.0f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff8fb0bf));
        g.drawText (z ("增益"), c.gainLabel, juce::Justification::centredLeft);
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 9.0f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff6f93a5));
        g.drawText (z ("电平"),
                    juce::Rectangle<float> (c.card.getRight() - 42.0f, c.gainLabel.getY(), 38.0f, 14.0f),
                    juce::Justification::centredRight);

        // 推子轨道与刻度（dB 为通用单位符号，保留）
        const auto tr = c.gainTrack;
        g.setColour (juce::Colour (0xff162f3c));
        g.fillRoundedRectangle (tr, 4.0f);

        const float db = sep.gainDb[(size_t) i].load (std::memory_order_relaxed);
        const float hy = yForGain (c, db);
        g.setColour (accent.withAlpha (0.85f));
        g.fillRoundedRectangle (tr.getX() + 1.0f, hy, tr.getWidth() - 2.0f,
                                juce::jmax (0.0f, tr.getBottom() - hy), 3.0f);

        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 8.5f, juce::Font::plain)));
        const int ticks[] = { 10, 0, -10, -20, -40, -60 };
        for (const int t : ticks)
        {
            const float y = yForGain (c, (float) t);
            const bool zero = (t == 0);
            g.setColour (zero ? juce::Colour (0xff29b6f6).withAlpha (0.75f)
                              : juce::Colour (0xff36586a));
            g.fillRect (tr.getX() + 2.0f, y - 0.5f, tr.getWidth() - 4.0f, 1.0f);
            g.setColour (zero ? juce::Colour (0xff9dc0d0) : juce::Colour (0xff5a7d8d));
            g.drawText (juce::String (t),
                        juce::Rectangle<float> (c.card.getX() + 2.0f, y - 6.0f, 22.0f, 12.0f),
                        juce::Justification::centredLeft);
        }

        // 推子把手（白色圆角块 + 横纹）
        const auto handle = juce::Rectangle<float> (tr.getX() - 5.0f, hy - 5.0f,
                                                     tr.getWidth() + 10.0f, 10.0f);
        g.setColour (juce::Colour (0xffeaf6ff));
        g.fillRoundedRectangle (handle, 3.0f);
        g.setColour (accent);
        g.fillRect (handle.getX() + 4.0f, handle.getCentreY() - 0.5f,
                    handle.getWidth() - 8.0f, 1.0f);

        // 推子右侧的实时电平柱（绿→黄→红 + LED 分段 + 白色峰值帽 + dB 刻度）
        drawMeter (g, c,
                   sep.meterLevel[(size_t) i].load (std::memory_order_relaxed),
                   sep.meterPeak[(size_t) i].load (std::memory_order_relaxed));

        // 增益数值框
        drawValueBox (g, c.gainValue, juce::String::formatted ("%+.1f dB", db),
                      std::abs (db) > 0.01f, accent);

        // S / M（机架通用英文缩写，按用户要求保留）
        drawToggle (g, c.solo, "S", sep.solo[(size_t) i].load (std::memory_order_relaxed),
                    juce::Colour (0xff29b6f6));
        drawToggle (g, c.mute, "M", sep.mute[(size_t) i].load (std::memory_order_relaxed),
                    juce::Colour (0xffe4390e));

        // 灵敏度标签
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 10.0f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff8fb0bf));
        g.drawText (z ("灵敏度"), c.sensLabel, juce::Justification::centredLeft);
        drawKnob (g, c.knob, sep.sensitivity[(size_t) i].load (std::memory_order_relaxed));
        drawValueBox (g, c.sensValue,
                      juce::String (juce::roundToInt (
                          sep.sensitivity[(size_t) i].load (std::memory_order_relaxed))) + "%",
                      std::abs (sep.sensitivity[(size_t) i].load (std::memory_order_relaxed)) > 0.01f,
                      accent);
    }

    // 实时电平柱：0..1 映射 -60..0 dBFS，渐变按全局 Y 取色，
    // 因此低电平时只有绿色段点亮，接近 0 dB 才会出现黄 / 红。
    void drawMeter (juce::Graphics& g, const Col& c, float lev, float pk) const
    {
        const auto r = c.meter;

        g.setColour (juce::Colour (0xff162f3c));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (juce::Colour (0xff2b4a5a));
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);

        const float fillH = juce::jlimit (0.0f, r.getHeight(), lev * r.getHeight());

        if (fillH > 0.5f)
        {
            juce::ColourGradient grad (juce::Colour (0xff33d433), 0.0f, r.getBottom(),
                                       juce::Colour (0xffe4390e), 0.0f, r.getY(), false);
            grad.addColour (0.45, juce::Colour (0xff8edb1e));
            grad.addColour (0.68, juce::Colour (0xffeed500));
            grad.addColour (0.86, juce::Colour (0xfff08a0c));
            g.setGradientFill (grad);
            g.fillRoundedRectangle (r.getX() + 1.0f, r.getBottom() - fillH,
                                    r.getWidth() - 2.0f, fillH, 2.0f);
        }

        // LED 分段暗线
        g.setColour (juce::Colour (0xff0a2230).withAlpha (0.55f));
        for (float y = 5.0f; y < r.getHeight(); y += 6.0f)
            g.fillRect (r.getX() + 1.0f, r.getBottom() - y, r.getWidth() - 2.0f, 1.0f);

        // 峰值帽（慢落）
        if (pk > 0.01f)
        {
            const float py = r.getBottom()
                           - juce::jlimit (0.0f, 1.0f, pk) * r.getHeight();
            g.setColour (juce::Colour (0xffeaf6ff));
            g.fillRect (r.getX() + 1.0f, py - 1.0f, r.getWidth() - 2.0f, 2.0f);
        }

        // 右侧 dB 刻度
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 8.0f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff5a7d8d));
        const int ticks[] = { 0, -6, -12, -24, -40, -60 };
        for (const int t : ticks)
        {
            const float f = (float) (t + 60) / 60.0f;
            const float y = r.getBottom() - f * r.getHeight();
            g.drawText (juce::String (t),
                        juce::Rectangle<float> (c.meterScale.getX(), y - 6.0f,
                                                c.meterScale.getWidth(), 12.0f),
                        juce::Justification::centredLeft);
        }
    }

    //==========================================================================
    // 最右侧「总音量」列：与 5 路完全同款卡片 / 推子 / 电平柱 / 数值框，
    // 下半部分把 S/M 换成「静音 / 归位」，把灵敏度旋钮换成输出电平圆盘。
    void drawMasterColumn (juce::Graphics& g, const Col& c) const
    {
        const auto accent = juce::Colour (0xff33d433);
        const float vol = juce::jlimit (0.0f, 1.0f, proc.getVolume());
        const bool muted = vol < 0.001f;

        g.setColour (juce::Colour (0xff0b202c));
        g.fillRoundedRectangle (c.card, 7.0f);
        g.setColour (juce::Colour (0xff1d3d4c));
        g.drawRoundedRectangle (c.card.reduced (0.5f), 7.0f, 1.0f);

        g.setColour (accent.withAlpha (0.95f));
        g.fillRoundedRectangle (juce::Rectangle<float> (c.nameBar.getX() + 8.0f,
                                                        c.nameBar.getY(),
                                                        c.nameBar.getWidth() - 16.0f, 3.0f), 1.5f);
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 15.0f, juce::Font::bold)));
        g.setColour (juce::Colours::white);
        g.drawText (z ("总音量"), c.nameBar.withTrimmedTop (5.0f).withHeight (18.0f),
                    juce::Justification::centred);
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 9.5f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff6f93a5));
        g.drawText (z ("主输出"), c.nameBar.withTrimmedTop (22.0f).withHeight (12.0f),
                    juce::Justification::centred);

        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 10.0f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff8fb0bf));
        g.drawText (z ("音量"), c.gainLabel, juce::Justification::centredLeft);
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 9.0f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff6f93a5));
        g.drawText (z ("电平"),
                    juce::Rectangle<float> (c.card.getRight() - 42.0f, c.gainLabel.getY(), 38.0f, 14.0f),
                    juce::Justification::centredRight);

        // 推子：顶部 100%、底部 0%，与分轨推子同一套把手与刻度风格
        const auto tr = c.gainTrack;
        g.setColour (juce::Colour (0xff162f3c));
        g.fillRoundedRectangle (tr, 4.0f);
        const float hy = yForMasterVolume (c, vol);
        g.setColour (accent.withAlpha (0.85f));
        g.fillRoundedRectangle (tr.getX() + 1.0f, hy, tr.getWidth() - 2.0f,
                                juce::jmax (0.0f, tr.getBottom() - hy), 3.0f);

        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 8.5f, juce::Font::plain)));
        const int ticks[] = { 100, 75, 50, 25, 0 };
        for (const int v : ticks)
        {
            const float y = yForMasterVolume (c, (float) v / 100.0f);
            const bool full = (v == 100);
            g.setColour (full ? juce::Colour (0xff33d433).withAlpha (0.75f)
                              : juce::Colour (0xff36586a));
            g.fillRect (tr.getX() + 2.0f, y - 0.5f, tr.getWidth() - 4.0f, 1.0f);
            g.setColour (full ? juce::Colour (0xff9dc0d0) : juce::Colour (0xff5a7d8d));
            g.drawText (juce::String (v),
                        juce::Rectangle<float> (c.card.getX() + 2.0f, y - 6.0f, 22.0f, 12.0f),
                        juce::Justification::centredLeft);
        }

        const auto handle = juce::Rectangle<float> (tr.getX() - 5.0f, hy - 5.0f,
                                                     tr.getWidth() + 10.0f, 10.0f);
        g.setColour (juce::Colour (0xffeaf6ff));
        g.fillRoundedRectangle (handle, 3.0f);
        g.setColour (accent);
        g.fillRect (handle.getX() + 4.0f, handle.getCentreY() - 0.5f,
                    handle.getWidth() - 8.0f, 1.0f);

        // 主输出电平柱（与 5 路共用同一套绘制 / LED 分段 / dB 刻度）
        drawMeter (g, c, proc.getMasterLevel(), proc.getMasterPeak());

        drawValueBox (g, c.gainValue,
                      juce::String (juce::roundToInt (vol * 100.0f)) + "%",
                      std::abs (vol - 1.0f) > 0.001f, accent);

        // 与 S/M 同位置的两个动作按钮
        drawActionButton (g, c.solo, z ("静音"), muted, juce::Colour (0xffe4390e));
        drawActionButton (g, c.mute, z ("归位"), false, accent);

        // 与「灵敏度」同位置：输出电平圆盘 + 峰值 dB 数值框
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 10.0f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff8fb0bf));
        g.drawText (z ("输出峰值"), c.sensLabel, juce::Justification::centredLeft);
        drawOutputDial (g, c.knob, proc.getMasterLevel(), proc.getMasterPeak());

        const float pkDb = juce::jlimit (0.0f, 1.0f, proc.getMasterPeak()) * 60.0f - 60.0f;
        drawValueBox (g, c.sensValue,
                      juce::String::formatted ("%.1f dB", (double) pkDb),
                      pkDb > -59.0f, accent);
    }

    static void drawActionButton (juce::Graphics& g, juce::Rectangle<float> r,
                                  const juce::String& label, bool on, juce::Colour onColour)
    {
        g.setColour (on ? onColour.withAlpha (0.90f) : juce::Colour (0xff173240));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (on ? onColour : juce::Colour (0xff2b4a5a));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 9.5f, juce::Font::bold)));
        g.setColour (on ? juce::Colours::white : juce::Colour (0xff9dc0d0));
        g.drawText (label, r, juce::Justification::centred);
    }

    // 圆形输出电平表：外圈弧长 = masterLevel，中心数字 = 峰值 dBFS
    static void drawOutputDial (juce::Graphics& g, juce::Rectangle<float> r,
                                float lev, float pk)
    {
        const float cx = r.getCentreX(), cy = r.getCentreY();
        const float rad = juce::jmin (r.getWidth(), r.getHeight()) * 0.42f;
        const float pi = juce::MathConstants<float>::pi;
        const float from = pi * 0.75f, span = pi * 1.5f;

        juce::Path base;
        base.addCentredArc (cx, cy, rad, rad, 0.0f, from, from + span, true);
        g.setColour (juce::Colour (0xff23414f));
        g.strokePath (base, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));

        if (lev > 0.001f)
        {
            juce::Path lit;
            lit.addCentredArc (cx, cy, rad, rad, 0.0f, from,
                               from + span * juce::jlimit (0.0f, 1.0f, lev), true);
            g.setColour (juce::Colour (0xff33d433));
            g.strokePath (lit, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        }

        g.setColour (juce::Colour (0xff0c2836));
        g.fillEllipse (cx - rad * 0.72f, cy - rad * 0.72f, rad * 1.44f, rad * 1.44f);
        g.setColour (juce::Colour (0xff2b5a70));
        g.drawEllipse (cx - rad * 0.72f, cy - rad * 0.72f, rad * 1.44f, rad * 1.44f, 1.0f);

        const float db = juce::jlimit (0.0f, 1.0f, pk) * 60.0f - 60.0f;
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 10.0f, juce::Font::bold)));
        g.setColour (juce::Colours::white);
        g.drawText (juce::String (juce::roundToInt (db)),
                    juce::Rectangle<float> (cx - rad, cy - 8.0f, rad * 2.0f, 16.0f),
                    juce::Justification::centred);
    }

    static void drawValueBox (juce::Graphics& g, juce::Rectangle<float> r,
                              const juce::String& text, bool changed, juce::Colour accent)
    {
        g.setColour (juce::Colour (0xff061722));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (changed ? accent.withAlpha (0.75f) : juce::Colour (0xff24485a));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 11.5f, juce::Font::bold)));
        g.setColour (changed ? juce::Colour (0xff9adcff) : juce::Colour (0xffcfe3ee));
        g.drawText (text, r, juce::Justification::centred);
    }

    static void drawToggle (juce::Graphics& g, juce::Rectangle<float> r,
                            const char* label, bool on, juce::Colour onColour)
    {
        g.setColour (on ? onColour.withAlpha (0.90f) : juce::Colour (0xff173240));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (on ? onColour : juce::Colour (0xff2b4a5a));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 11.0f, juce::Font::bold)));
        g.setColour (on ? juce::Colours::white : juce::Colour (0xff9dc0d0));
        g.drawText (label, r, juce::Justification::centred);
    }

    static void drawKnob (juce::Graphics& g, juce::Rectangle<float> r, float sens)
    {
        const float cx = r.getCentreX(), cy = r.getCentreY();
        const float rad = juce::jmin (r.getWidth(), r.getHeight()) * 0.42f;
        const float pi = juce::MathConstants<float>::pi;
        const float from = pi * 0.75f, span = pi * 1.5f;
        const float frac = juce::jlimit (0.0f, 1.0f, (sens + 100.0f) / 200.0f);

        juce::Path base;
        base.addCentredArc (cx, cy, rad, rad, 0.0f, from, from + span, true);
        g.setColour (juce::Colour (0xff23414f));
        g.strokePath (base, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));

        if (frac > 0.001f)
        {
            juce::Path lit;
            lit.addCentredArc (cx, cy, rad, rad, 0.0f, from, from + span * frac, true);
            g.setColour (juce::Colour (0xff29b6f6));
            g.strokePath (lit, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        }

        g.setColour (juce::Colour (0xff0c2836));
        g.fillEllipse (cx - rad * 0.72f, cy - rad * 0.72f, rad * 1.44f, rad * 1.44f);
        g.setColour (juce::Colour (0xff2b5a70));
        g.drawEllipse (cx - rad * 0.72f, cy - rad * 0.72f, rad * 1.44f, rad * 1.44f, 1.0f);

        const float a = from + span * frac;
        g.setColour (juce::Colours::white);
        g.drawLine (cx + std::cos (a) * rad * 0.28f, cy + std::sin (a) * rad * 0.28f,
                    cx + std::cos (a) * rad * 0.66f, cy + std::sin (a) * rad * 0.66f, 2.2f);
    }

    //==========================================================================
    //==========================================================================
    // 总音量：推子 / 滚轮 / 双击 / 静音 / 归位
    static float yForMasterVolume (const Col& c, float v)
    {
        const float t = juce::jlimit (0.0f, 1.0f, 1.0f - v);
        return c.gainTrack.getY() + t * c.gainTrack.getHeight();
    }

    void applyMasterVolumeAt (float y)
    {
        const auto c = layout()[(size_t) masterCol];
        const float t = juce::jlimit (0.0f, 1.0f,
                                      (y - c.gainTrack.getY()) / c.gainTrack.getHeight());
        setMasterVolume (1.0f - t);
    }

    void setMasterVolume (float v)
    {
        const float nv = juce::jlimit (0.0f, 1.0f, v);
        if (nv > 0.001f) lastNonZeroVolume = nv;
        proc.setVolume (nv);
    }

    void toggleMasterMute()
    {
        if (proc.getVolume() < 0.001f)
            proc.setVolume (lastNonZeroVolume > 0.001f ? lastNonZeroVolume : 1.0f);
        else
        {
            lastNonZeroVolume = proc.getVolume();
            proc.setVolume (0.0f);
        }
    }

    static float yForGain (const Col& c, float db)
    {
        const float t = juce::jlimit (0.0f, 1.0f, (10.0f - db) / 70.0f);
        return c.gainTrack.getY() + t * c.gainTrack.getHeight();
    }

    void applyGainAt (int col, float y)
    {
        const auto c = layout()[(size_t) col];
        const float t = juce::jlimit (0.0f, 1.0f, (y - c.gainTrack.getY()) / c.gainTrack.getHeight());
        setGain (col, 10.0f - t * 70.0f);
    }

    void setGain (int col, float db)
    {
        sep.gainDb[(size_t) col].store (juce::jlimit (-60.0f, 10.0f, db),
                                        std::memory_order_relaxed);
    }

    void setSens (int col, float v)
    {
        sep.sensitivity[(size_t) col].store (juce::jlimit (-100.0f, 100.0f, v),
                                             std::memory_order_relaxed);
    }

    void resetAll()
    {
        for (int i = 0; i < StemSeparator::numStems; ++i)
        {
            sep.gainDb[(size_t) i].store (0.0f, std::memory_order_relaxed);
            sep.sensitivity[(size_t) i].store (0.0f, std::memory_order_relaxed);
            sep.mute[(size_t) i].store (false, std::memory_order_relaxed);
            sep.solo[(size_t) i].store (false, std::memory_order_relaxed);
        }
        // 全部回到中性 ⇒ isActive() 自动变 false ⇒ 原声直通
        repaint();
    }

    std::uint64_t signature() const
    {
        std::uint64_t s = sep.isActive() ? 1u : 0u;
        for (int i = 0; i < StemSeparator::numStems; ++i)
        {
            s = s * 131u + (std::uint64_t) (int) (sep.gainDb[(size_t) i].load (std::memory_order_relaxed) * 10.0f);
            s = s * 131u + (std::uint64_t) (int) (sep.sensitivity[(size_t) i].load (std::memory_order_relaxed) * 10.0f);
            s = s * 131u + (std::uint64_t) (sep.mute[(size_t) i].load (std::memory_order_relaxed) ? 1 : 0);
            s = s * 131u + (std::uint64_t) (sep.solo[(size_t) i].load (std::memory_order_relaxed) ? 1 : 0);
        }
        s = s * 131u + (std::uint64_t) (int) (proc.getVolume() * 1000.0f);
        return s;
    }

    MediaPlayAudioProcessor& proc;
    StemSeparator& sep;
    float lastNonZeroVolume = 1.0f;
    int contentTop = 40;
    Hit hit = Hit::None, hover = Hit::None;
    int activeCol = -1, hoverCol = -1;
    float dragStartY = 0.0f, dragStartGain = 0.0f, dragStartSens = 0.0f;
    std::uint64_t lastSig = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemPanel)
};
