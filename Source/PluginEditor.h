#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "VisualizerComponent.h"
#include "StemPanel.h"

//==============================================================================
// 胶囊按钮（侧边栏样式，参考截图）
class PillButton : public juce::Button
{
public:
    PillButton (const juce::String& text, bool toggling = false)
        : juce::Button (text)
    {
        setClickingTogglesState (toggling);
    }

    // 上方弹窗打开时，把按钮渲染成「基座」：上方直角 + accent 高亮，与弹窗无缝相接
    void setAttached (bool a)
    {
        if (attached != a) { attached = a; repaint(); }
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        // 使用完整 bounds（不内缩），attached 时与上方弹窗面板像素级等宽对齐
        auto b = getLocalBounds().toFloat();
        const bool enabled = isEnabled();
        const bool on = enabled && (getToggleState() || attached);

        juce::Colour bg = ! enabled ? juce::Colour (0xff0b2029)
                          : attached ? juce::Colour (0xff164a63)
                          : (on ? juce::Colour (0xff123a4d)
                                : (over ? juce::Colour (0xff164457) : juce::Colour (0xff10303e)));
        if (down && enabled) bg = bg.brighter (0.12f);

        if (attached)
        {
            const float r = 6.0f;
            juce::Path shape;
            shape.startNewSubPath (b.getX(), b.getY());
            shape.lineTo (b.getRight(), b.getY());
            shape.lineTo (b.getRight(), b.getBottom() - r);
            shape.quadraticTo (b.getRight(), b.getBottom(), b.getRight() - r, b.getBottom());
            shape.lineTo (b.getX() + r, b.getBottom());
            shape.quadraticTo (b.getX(), b.getBottom(), b.getX(), b.getBottom() - r);
            shape.closeSubPath();
            g.setColour (bg);
            g.fillPath (shape);

            // 只描左 / 右 / 下三边，上边留给弹窗继续
            juce::Path edge;
            edge.startNewSubPath (b.getX() + 0.5f, b.getY());
            edge.lineTo (b.getX() + 0.5f, b.getBottom() - r);
            edge.quadraticTo (b.getX() + 0.5f, b.getBottom() - 0.5f,
                              b.getX() + r, b.getBottom() - 0.5f);
            edge.lineTo (b.getRight() - r, b.getBottom() - 0.5f);
            edge.quadraticTo (b.getRight() - 0.5f, b.getBottom() - 0.5f,
                              b.getRight() - 0.5f, b.getBottom() - r);
            edge.lineTo (b.getRight() - 0.5f, b.getY());
            g.setColour (juce::Colour (0xff29b6f6).withAlpha (0.90f));
            g.strokePath (edge, juce::PathStrokeType (1.2f));
        }
        else
        {
            const auto rb = b.reduced (0.5f);
            g.setColour (bg);
            g.fillRoundedRectangle (rb, 6.0f);

            if (on)
            {
                g.setColour (juce::Colour (0xff29b6f6).withAlpha (0.9f));
                g.drawRoundedRectangle (rb.reduced (1.0f), 6.0f, 1.2f);
            }
        }

        g.setColour (! enabled ? juce::Colour (0xff4e6b7a)
                               : (on ? juce::Colours::white : juce::Colour (0xffcfe3ee)));
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 14.0f, juce::Font::plain)));
        g.drawText (getButtonText(), b, juce::Justification::centred);
    }

private:
    bool attached = false;
};

//==============================================================================
// 传输图标按钮（▶ / ⏸ / ⏮ / ⏭），主按钮为青色圆底
class IconButton : public juce::Button
{
public:
    enum Icon { Play, Pause, Prev, Next };

    IconButton (Icon ic, bool mainButton = false)
        : juce::Button ("icon"), icon (ic), main (mainButton) {}

    void setIcon (Icon ic)
    {
        if (icon != ic) { icon = ic; repaint(); }
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat();
        const float cx = b.getCentreX(), cy = b.getCentreY();
        const float d = juce::jmin (b.getWidth(), b.getHeight());
        const float s = d * 0.34f;   // 图标半径

        if (main)
        {
            auto bg = juce::Colour (0xff29b6f6);
            if (down) bg = bg.darker (0.15f);
            else if (over) bg = bg.brighter (0.12f);
            g.setColour (bg);
            g.fillEllipse (cx - d * 0.5f, cy - d * 0.5f, d, d);
        }
        else
        {
            if (over || down)
            {
                g.setColour (juce::Colour (0x2effffff));
                g.fillEllipse (cx - d * 0.5f, cy - d * 0.5f, d, d);
            }
        }

        juce::Path p;
        switch (icon)
        {
            case Play:
                p.addTriangle (cx - s * 0.7f, cy - s, cx - s * 0.7f, cy + s, cx + s, cy);
                break;
            case Pause:
                p.addRoundedRectangle (cx - s * 0.75f, cy - s, s * 0.5f, s * 2.0f, 1.5f);
                p.addRoundedRectangle (cx + s * 0.25f, cy - s, s * 0.5f, s * 2.0f, 1.5f);
                break;
            case Prev:
                p.addRoundedRectangle (cx - s * 0.8f, cy - s, s * 0.32f, s * 2.0f, 1.0f);
                p.addTriangle (cx + s * 0.8f, cy - s, cx + s * 0.8f, cy + s, cx - s * 0.35f, cy);
                break;
            case Next:
                p.addTriangle (cx - s * 0.8f, cy - s, cx - s * 0.8f, cy + s, cx + s * 0.35f, cy);
                p.addRoundedRectangle (cx + s * 0.48f, cy - s, s * 0.32f, s * 2.0f, 1.0f);
                break;
        }

        g.setColour (main ? juce::Colour (0xff082430) : juce::Colour (0xfff0ecff));
        g.fillPath (p);
    }

private:
    Icon icon;
    bool main;
};

//==============================================================================
// 侧边栏曲目列表（当前曲目高亮，点击切歌）
class PlaylistComponent : public juce::ListBox, private juce::ListBoxModel
{
public:
    std::function<void (int)> onTrackChosen;

    explicit PlaylistComponent (MediaPlayAudioProcessor& p) : proc (p)
    {
        setModel (this);
        setRowHeight (26);
        setMultipleSelectionEnabled (true);      // AI 分离页支持多选批量提取
        setClickingTogglesRowSelection (false);
        setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        setOpaque (false);
        updateContent();
    }

    int getNumRows() override { return proc.getPlaylistSize(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
    {
        const bool cur = (row == proc.getCurrentTrackIndex());
        if (cur)
        {
            g.setColour (juce::Colour (0xff123a4d));
            g.fillRoundedRectangle (2.0f, 1.0f, (float) w - 4.0f, (float) h - 2.0f, 5.0f);
        }
        g.setColour (cur ? juce::Colour (0xff29b6f6) : juce::Colour (0xffcfe3ee));
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 12.5f, juce::Font::plain)));
        g.drawText (juce::String (row + 1) + ". " + proc.getPlaylistItem (row),
                    juce::Rectangle<int> (10, 0, w - 16, h),
                    juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        // 单击只负责选中；右键弹出菜单
        if (e.mods.isPopupMenu())
            showContextMenu (row);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && onTrackChosen)
            onTrackChosen (row);
    }

    // 列表空白处右键：只提供「清空列表」
    void backgroundClicked (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            showContextMenu (-1);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff0c2b39));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 8.0f);
        ListBox::paint (g);
        if (getNumRows() == 0)
        {
            g.setColour (juce::Colour (0xff5f7f92));
            g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 12.5f, juce::Font::plain)));
            g.drawText (MediaPlayAudioProcessor::zh ("播放列表为空"),
                        getLocalBounds(), juce::Justification::centred);
        }
    }

private:
    void showContextMenu (int row)
    {
        juce::PopupMenu menu;
        if (row >= 0)
            menu.addItem (1, MediaPlayAudioProcessor::zh ("删除选中"), true, false);
        menu.addItem (2, MediaPlayAudioProcessor::zh ("清空列表"), true, false);

        menu.showMenuAsync (juce::PopupMenu::Options()
                                .withTargetComponent (*this)
                                .withMousePosition()
                                .withDeletionCheck (*this),
                            [this, row] (int result)
                            {
                                if (result == 1 && row >= 0) proc.removeTrack (row);
                                else if (result == 2)        proc.clearPlaylist();
                                updateContent();
                                deselectAllRows();
                                repaint();
                            });
    }

    MediaPlayAudioProcessor& proc;
};

//==============================================================================
// 播放器滑条外观（细轨 + 圆点）
class PlayerLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           const juce::Slider::SliderStyle style, juce::Slider&) override
    {
        auto trackColour = juce::Colour (0xff2a3b46);
        auto fillColour = juce::Colour (0xff29b6f6);

        if (style == juce::Slider::LinearVertical)
        {
            const float cx = (float) x + (float) width * 0.5f;
            const float trackW = 6.0f;

            g.setColour (trackColour);
            g.fillRoundedRectangle (cx - trackW * 0.5f, (float) y, trackW, (float) height, 3.0f);

            const float fillH = (float) height - sliderPos;
            g.setColour (fillColour.withAlpha (0.9f));
            g.fillRoundedRectangle (cx - trackW * 0.5f, (float) y + sliderPos, trackW, fillH, 3.0f);

            g.setColour (juce::Colours::white);
            g.fillEllipse (cx - 7.0f, (float) y + sliderPos - 7.0f, 14.0f, 14.0f);
        }
        else // LinearHorizontal（进度条 / 音量条）
        {
            const float cy = (float) y + (float) height * 0.5f;
            const float trackH = 4.0f;

            g.setColour (trackColour);
            g.fillRoundedRectangle ((float) x, cy - trackH * 0.5f, (float) width, trackH, 2.0f);

            const float fillW = sliderPos - (float) x;
            g.setColour (juce::Colour (0xff29b6f6));
            g.fillRoundedRectangle ((float) x, cy - trackH * 0.5f, fillW, trackH, 2.0f);

            g.setColour (juce::Colours::white);
            g.fillEllipse (sliderPos - 6.0f, cy - 6.0f, 12.0f, 12.0f);
        }
    }
};

//==============================================================================
// KTV 歌词浮层：双行显示（当前行靠左带渐染，下一行靠右）
// 两行之间保留固定净空，衬底/裁剪区都不越界，避免上下两行视觉粘连。
// 有逐字时间轴按字渐染，否则整行按时间比例渐染
class LyricsOverlay : public juce::Component
{
public:
    explicit LyricsOverlay (MediaPlayAudioProcessor& p) : proc (p) {}

    void paint (juce::Graphics& g) override
    {
        auto& lyr = proc.getLyrics();

        // 歌词已开启（本浮层仅在开启时可见），但未匹配到同名 .lrc 时，
        // 在歌词区直接给出中/英文提示，避免「开了歌词却一片空白」的困惑。
        if (! lyr.isLoaded())
        {
            const auto b = getLocalBounds().toFloat();
            const bool hasTrack = proc.getCurrentTrackIndex() >= 0;
            if (hasTrack)
            {
                g.setColour (juce::Colour (0xff9fd0dd));
                g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 13.0f, juce::Font::plain)));
                g.drawText (MediaPlayAudioProcessor::zh ("未找到歌词文件"),
                            juce::Rectangle<float> (0.0f, b.getY(), b.getWidth(), b.getHeight() * 0.52f),
                            juce::Justification::centred);
                g.setColour (juce::Colour (0xff66808f));
                g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 10.5f, juce::Font::plain)));
                g.drawText (MediaPlayAudioProcessor::zh ("把同名 .lrc 放到歌曲同目录"),
                            juce::Rectangle<float> (0.0f, b.getY() + b.getHeight() * 0.52f, b.getWidth(), b.getHeight() * 0.48f),
                            juce::Justification::centred);
            }
            else
            {
                g.setColour (juce::Colour (0xff9fd0dd));
                g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 13.0f, juce::Font::plain)));
                g.drawText (MediaPlayAudioProcessor::zh ("未加载曲目"), b, juce::Justification::centred);
            }
            return;
        }

        const double t = proc.getCurrentPosition();
        const int cur = lyr.lineIndexAt (t);

        auto bounds = getLocalBounds().toFloat();
        const float w = bounds.getWidth(), h = bounds.getHeight();

        // 均分两行 + 中间固定 14px 净空 + 上下各留 2px；字号按行高收敛，衬底只外扩 2px，
        // 因此两行的字与深色底板都不会互相压到（旧版 0.44/0.40 高度 + 8px 外扩会连成一块）。
        constexpr float rowGap = 14.0f, outerPad = 2.0f;
        const float rowH = juce::jmax (1.0f, (h - outerPad * 2.0f - rowGap) * 0.5f);
        const auto row1 = juce::Rectangle<float> (0.0f, outerPad, w, rowH);
        const auto row2 = juce::Rectangle<float> (0.0f, outerPad + rowH + rowGap, w, rowH);
        const float cx1 = w * 0.36f;   // 第一行更靠左
        const float cx2 = w * 0.66f;   // 第二行更靠右

        if (cur >= 0)
        {
            drawKtvLine (g, lyr.line (cur), t, row1, cx1, true);
            drawKtvLine (g, lyr.line (cur + 1), t, row2, cx2, false);
        }
        else if (lyr.getLineCount() > 0)
        {
            drawKtvLine (g, lyr.line (0), t, row2, cx2, false);   // 尚未开唱，先在下行预告
        }
    }

private:
    static void drawKtvLine (juce::Graphics& g, const LyricLine* L, double t,
                             juce::Rectangle<float> area, float centreX, bool current)
    {
        if (L == nullptr || L->text.trim().isEmpty()) return;

        // 字号随歌词行高自适应：当前行略大于预告行，且都明显小于行高，留出上下净空
        const float baseH = juce::jmin (current ? 34.0f : 28.0f, area.getHeight() * 0.78f);
        juce::Font font (juce::FontOptions ("Microsoft YaHei UI", baseH, juce::Font::bold));
        float textW = juce::GlyphArrangement::getStringWidth (font, L->text);
        // 限制宽度：不超过区域，也不越过自身中心到左右边缘（错位排布时不越界/不重叠）
        const float maxW = juce::jmin (area.getWidth() * 0.94f,
                                       2.0f * juce::jmin (centreX, area.getWidth() - centreX) * 0.96f);
        if (textW > maxW)
        {
            font = font.withHeight (baseH * maxW / textW);
            textW = maxW;
        }

        const float textX = centreX - textW * 0.5f;
        const auto textRect = juce::Rectangle<float> (textX, area.getY(), textW + 4.0f, area.getHeight());

        // 深色衬底：把歌词和频谱隔开。纵向只外扩 2px，避免两行底板叠在一起。
        const auto backing = textRect.expanded (12.0f, 2.0f);
        g.setColour (juce::Colour (0xa8061a24));
        g.fillRoundedRectangle (backing, (float) backing.getHeight() * 0.30f);

        g.setFont (font);
        // 阴影提升可读性
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.drawText (L->text, textRect.translated (2.0f, 2.0f), juce::Justification::centredLeft, false);
        // 未唱底色：改成中性浅白，避免和已唱渐变的绿端撞色（原来用绿色）
        g.setColour (current ? juce::Colour (0xfff0f7fb) : juce::Colour (0xcfffffff));
        g.drawText (L->text, textRect, juce::Justification::centredLeft, false);

        // 已唱渐染：红 → 黄 → 绿，直接复用频谱 LED 的三色
        // （红 0xE4390E / 黄 0xEED500 / 绿 0x33D433，见 VisualizerComponent::drawLedBars）
        float filled = 0.0f;
        if (current)
        {
            if (! L->words.empty())
            {
                for (const auto& wd : L->words)
                {
                    if (t >= wd.t1)
                        filled += juce::GlyphArrangement::getStringWidth (font, wd.text);
                    else if (t > wd.t0)
                    {
                        const float f = (float) ((t - wd.t0) / juce::jmax (0.05, wd.t1 - wd.t0));
                        filled += juce::GlyphArrangement::getStringWidth (font, wd.text) * juce::jlimit (0.0f, 1.0f, f);
                        break;
                    }
                    else break;
                }
            }
            else if (L->t1 > L->t0)
            {
                filled = textW * (float) juce::jlimit (0.0, 1.0, (t - L->t0) / (L->t1 - L->t0));
            }
            filled = juce::jlimit (0.0f, textW, filled);
        }

        if (filled > 0.5f)
        {
            g.saveState();
            g.reduceClipRegion (juce::Rectangle<float> (textX - 2.0f, area.getY() - 2.0f,
                                                        filled + 2.0f, area.getHeight() + 4.0f)
                                    .toNearestInt());
            // 斜向渐变：文字横向占绝大多数权重，视觉上是「红 → 黄 → 绿」自左向右展开，
            // 同时带一点自上而下的色差，和顶部频谱柱的 VU 配色一致。
            // 渐变坐标固定在整行文字上（不随 filled 变化），逐字点亮时颜色不会跳动。
            juce::ColourGradient sung (juce::Colour (0xffe4390e),
                                       textRect.getX(), textRect.getY(),
                                       juce::Colour (0xff33d433),
                                       textRect.getRight(), textRect.getBottom(), false);
            sung.addColour (0.55, juce::Colour (0xffeed500));
            g.setGradientFill (sung);
            g.drawText (L->text, textRect, juce::Justification::centredLeft, false);
            g.restoreState();
        }
    }

    MediaPlayAudioProcessor& proc;
};

//==============================================================================
// 向上弹窗通用外观：与触发按钮同宽、底边紧贴按钮上沿的「延伸式」面板。
// 上方圆角、下方直角，底边不描边；底部有向按钮汇聚的 accent 高光，
// 与下方 attached 状态的按钮拼在一起，视觉上像从按钮本身向上长出来。
inline void drawVerticalPopupPanel (juce::Graphics& g, juce::Component& c, const juce::String& title)
{
    const auto b = c.getLocalBounds().toFloat();
    const float w = b.getWidth();
    const float r = juce::jmin (10.0f, w * 0.20f);

    juce::Path panel;
    panel.startNewSubPath (b.getX(), b.getBottom());
    panel.lineTo (b.getX(), b.getY() + r);
    panel.quadraticTo (b.getX(), b.getY(), b.getX() + r, b.getY());
    panel.lineTo (b.getRight() - r, b.getY());
    panel.quadraticTo (b.getRight(), b.getY(), b.getRight(), b.getY() + r);
    panel.lineTo (b.getRight(), b.getBottom());
    panel.closeSubPath();
    g.setColour (juce::Colour (0xf2061a24));
    g.fillPath (panel);

    // 底部只留极淡的过渡，不做横向亮带，避免把面板与按钮「切」开
    g.setColour (juce::Colour (0xff29b6f6).withAlpha (0.07f));
    g.fillRect (b.getX() + 1.5f, b.getBottom() - 8.0f, w - 3.0f, 7.0f);

    // 描边只走上 / 左 / 右三边，底边留空，与下方按钮连成一体
    juce::Path edge;
    edge.startNewSubPath (b.getX() + 0.5f, b.getBottom());
    edge.lineTo (b.getX() + 0.5f, b.getY() + r);
    edge.quadraticTo (b.getX() + 0.5f, b.getY() + 0.5f, b.getX() + r, b.getY() + 0.5f);
    edge.lineTo (b.getRight() - r, b.getY() + 0.5f);
    edge.quadraticTo (b.getRight() - 0.5f, b.getY() + 0.5f, b.getRight() - 0.5f, b.getY() + r);
    edge.lineTo (b.getRight() - 0.5f, b.getBottom());
    g.setColour (juce::Colour (0xff29b6f6).withAlpha (0.65f));
    g.strokePath (edge, juce::PathStrokeType (1.0f));

    // 接缝亮线：与按钮上沿对齐，暗示两者是同一块
    g.setColour (juce::Colour (0xff29b6f6).withAlpha (0.30f));
    g.fillRect (b.getX() + 2.0f, b.getBottom() - 1.0f, w - 4.0f, 1.0f);

    // 标题：窄面板自动缩小字号
    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI",
                                              w < 60.0f ? 10.0f : 11.5f, juce::Font::bold)));
    g.drawText (title, juce::Rectangle<float> (0, 5.0f, w, 17.0f), juce::Justification::centred);
}

//==============================================================================
// 歌词偏移参考线弹窗：点击「偏移」按钮后在其上方弹出
// 垂直参考线，中点=默认歌词时间(0s)；把手向上拖=歌词提前，向下拖=歌词延后，±10s
class LyricOffsetPopup : public juce::Component
{
public:
    std::function<void (double)> onOffsetChanged;   // 拖动时回调（秒，正=推迟）
    std::function<void()> onCloseRequest;           // 点击弹窗外区域请求关闭

    explicit LyricOffsetPopup (MediaPlayAudioProcessor& p) : proc (p)
    {
        setAlwaysOnTop (true);
    }

    // 非拖动状态下同步显示当前歌曲的偏移（切歌后调用）
    void syncFromProcessor()
    {
        if (! dragging) repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const float w = b.getWidth(), h = b.getHeight();
        const bool narrow = w < 60.0f;
        drawVerticalPopupPanel (g, *this, MediaPlayAudioProcessor::zh ("歌词偏移"));

        const auto tr = trackRect();
        const float cx = tr.getCentreX();

        // 上 / 下方向提示：窄面板改为居中排布，不再占用轨道右侧空间
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI",
                                                  narrow ? 9.0f : 10.0f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff8bc34a));
        g.drawText (MediaPlayAudioProcessor::zh ("提前"),
                    juce::Rectangle<float> (0, 24.0f, w, 14.0f), juce::Justification::centred);
        g.setColour (juce::Colour (0xffffd54f));
        g.drawText (MediaPlayAudioProcessor::zh ("延后"),
                    juce::Rectangle<float> (0, tr.getBottom() + 2.0f, w, 14.0f),
                    juce::Justification::centred);

        // 轨道
        g.setColour (juce::Colour (0xff2a3b46));
        g.fillRoundedRectangle (tr, 3.0f);

        // 刻度：0 / ±2 / ±4 / ±6 / ±8 / ±10（居中短横线）
        for (int s = -10; s <= 10; s += 2)
        {
            const float fy = yForSeconds ((double) s);
            const bool zero = (s == 0);
            const float half = zero ? juce::jmin (11.0f, w * 0.20f)
                                    : juce::jmin (7.0f, w * 0.13f);
            g.setColour (zero ? juce::Colour (0xff29b6f6) : juce::Colour (0x40ffffff));
            g.fillRect (cx - half, fy - 0.5f, half * 2.0f, 1.0f);
            if (zero && ! narrow)
            {
                g.setColour (juce::Colour (0xff29b6f6));
                g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 9.0f, juce::Font::plain)));
                g.drawText ("0", juce::Rectangle<float> (cx + 12.0f, fy - 6.0f, 24.0f, 12.0f),
                            juce::Justification::centredLeft);
            }
        }

        // 已应用区段高亮（中点 → 把手）
        const double off = currentOffset();
        const float hy = yForSeconds (off);
        const float zeroY = yForSeconds (0.0);
        g.setColour (juce::Colour (0xff29b6f6).withAlpha (0.55f));
        g.fillRect (tr.getX() + 1.0f, juce::jmin (hy, zeroY),
                    tr.getWidth() - 2.0f, std::abs (hy - zeroY));

        // 把手（白圆 + accent 描边）
        g.setColour (juce::Colours::white);
        g.fillEllipse (cx - 7.0f, hy - 7.0f, 14.0f, 14.0f);
        g.setColour (juce::Colour (0xff29b6f6));
        g.drawEllipse (cx - 7.0f, hy - 7.0f, 14.0f, 14.0f, 2.0f);

        // 底部数值
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI",
                                                  narrow ? 10.5f : 12.0f, juce::Font::bold)));
        g.setColour (std::abs (off) < 0.05 ? juce::Colour (0xffcfe3ee) : juce::Colour (0xff29b6f6));
        g.drawText (offText (off), juce::Rectangle<float> (0, h - 27.0f, w, 19.0f),
                    juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = true;
        applyFromY (e.position.y);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging) applyFromY (e.position.y);
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

    // 点击轨道区外（面板空白处）关闭
    void mouseDownOutside() {}

private:
    juce::Rectangle<float> trackRect() const
    {
        const auto b = getLocalBounds().toFloat();
        const float cx = b.getWidth() * 0.5f;
        const float tw = b.getWidth() < 60.0f ? 5.0f : 6.0f;
        return { cx - tw * 0.5f, 42.0f, tw, b.getHeight() - 42.0f - 48.0f };
    }

    float yForSeconds (double s) const
    {
        const auto tr = trackRect();
        return (float) (tr.getY() + (juce::jlimit (-10.0, 10.0, s) / 10.0 * 0.5 + 0.5) * tr.getHeight());
    }

    void applyFromY (float y)
    {
        const auto tr = trackRect();
        const double f = juce::jlimit (0.0, 1.0, (double) (y - tr.getY()) / tr.getHeight());
        double v = (f - 0.5) * 20.0;                 // 上=-10(提前) 下=+10(延后)
        v = juce::roundToInt (v * 10.0) / 10.0;      // 0.1s 步进
        if (onOffsetChanged) onOffsetChanged (v);
        repaint();
    }

    double currentOffset() const { return proc.getLyricOffsetOfCurrent(); }

    static juce::String offText (double off)
    {
        // 纯 ASCII，避免窄面板放不下中文，也彻底避开中文字面量编码问题
        if (std::abs (off) < 0.05) return "0.0s";
        return (off < 0 ? "-" : "+") + juce::String (std::abs (off), 1) + "s";
    }

    MediaPlayAudioProcessor& proc;
    bool dragging = false;
};

//==============================================================================
// 音量弹窗：与「歌词偏移」完全同款——点击底部按钮向上弹出的垂直调节器。
// 上 = 100%、下 = 0%，双击面板空白处归位 100%；不再占用频谱右侧的常驻推子。
class VolumePopup : public juce::Component
{
public:
    std::function<void (float)> onVolumeChanged;

    explicit VolumePopup (MediaPlayAudioProcessor& p) : proc (p)
    {
        setAlwaysOnTop (true);
    }

    // 由编辑器定时器调用：只在数值真的变化时重绘，避免空闲时每 33ms
    // 反复重建中文 String / 触发组件重绘（AI 页宿主堆损坏的老教训）。
    void syncFromProcessor()
    {
        const float v = juce::jlimit (0.0f, 1.0f, proc.getVolume());
        if (! dragging && std::abs (v - lastPainted) > 0.004f)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const float w = b.getWidth(), h = b.getHeight();
        const bool narrow = w < 60.0f;
        const float vol = juce::jlimit (0.0f, 1.0f, proc.getVolume());
        lastPainted = vol;

        drawVerticalPopupPanel (g, *this, MediaPlayAudioProcessor::zh ("音量"));

        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI",
                                                  narrow ? 9.0f : 10.0f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff8bc34a));
        g.drawText ("100", juce::Rectangle<float> (0, 24.0f, w, 14.0f), juce::Justification::centred);
        g.setColour (juce::Colour (0xffffd54f));
        g.drawText ("0", juce::Rectangle<float> (0, trackRect().getBottom() + 2.0f, w, 14.0f),
                    juce::Justification::centred);

        const auto tr = trackRect();
        const float cx = tr.getCentreX();
        g.setColour (juce::Colour (0xff2a3b46));
        g.fillRoundedRectangle (tr, 3.0f);

        // 刻度：0 / 25 / 50 / 75 / 100
        for (int s = 0; s <= 100; s += 25)
        {
            const float fy = yForVolume ((float) s / 100.0f);
            const bool major = (s == 0 || s == 100);
            const float half = major ? juce::jmin (11.0f, w * 0.20f)
                                     : juce::jmin (7.0f, w * 0.13f);
            g.setColour (major ? juce::Colour (0xff29b6f6) : juce::Colour (0x40ffffff));
            g.fillRect (cx - half, fy - 0.5f, half * 2.0f, 1.0f);
        }

        // 已应用区段高亮（把手 → 底部）
        const float hy = yForVolume (vol);
        g.setColour (juce::Colour (0xff29b6f6).withAlpha (0.55f));
        g.fillRect (tr.getX() + 1.0f, hy, tr.getWidth() - 2.0f, tr.getBottom() - hy);

        // 把手
        g.setColour (juce::Colours::white);
        g.fillEllipse (cx - 7.0f, hy - 7.0f, 14.0f, 14.0f);
        g.setColour (juce::Colour (0xff29b6f6));
        g.drawEllipse (cx - 7.0f, hy - 7.0f, 14.0f, 14.0f, 2.0f);

        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI",
                                                  narrow ? 10.5f : 12.0f, juce::Font::bold)));
        g.setColour (vol > 0.995f ? juce::Colour (0xffcfe3ee) : juce::Colour (0xff29b6f6));
        g.drawText (juce::String (juce::roundToInt (vol * 100.0f)) + "%",
                    juce::Rectangle<float> (0, h - 27.0f, w, 19.0f), juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = true;
        applyFromY (e.position.y);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging) applyFromY (e.position.y);
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (onVolumeChanged) onVolumeChanged (1.0f);
        repaint();
    }

private:
    juce::Rectangle<float> trackRect() const
    {
        const auto b = getLocalBounds().toFloat();
        const float cx = b.getWidth() * 0.5f;
        const float tw = b.getWidth() < 60.0f ? 5.0f : 6.0f;
        return { cx - tw * 0.5f, 42.0f, tw, b.getHeight() - 42.0f - 48.0f };
    }

    float yForVolume (float v) const
    {
        const auto tr = trackRect();
        return (float) (tr.getY() + (1.0 - (double) juce::jlimit (0.0f, 1.0f, v)) * tr.getHeight());
    }

    void applyFromY (float y)
    {
        const auto tr = trackRect();
        const float f = juce::jlimit (0.0f, 1.0f, (float) ((y - tr.getY()) / tr.getHeight()));
        const float v = (float) juce::roundToInt ((1.0f - f) * 100.0f) / 100.0f;
        if (onVolumeChanged) onVolumeChanged (v);
        repaint();
    }

    MediaPlayAudioProcessor& proc;
    bool dragging = false;
    float lastPainted = -1.0f;
};

//==============================================================================
// 播放模式弹窗：向上弹出的四选一列表，样式与偏移参考线统一
class PlayModePopup : public juce::Component
{
public:
    std::function<void (int)> onModeChosen;

    explicit PlayModePopup (MediaPlayAudioProcessor& p) : proc (p)
    {
        setAlwaysOnTop (true);
    }

    void syncFromProcessor() { repaint(); }

    void paint (juce::Graphics& g) override
    {
        drawVerticalPopupPanel (g, *this, MediaPlayAudioProcessor::zh ("播放模式"));

        const int cur = (int) proc.getPlayMode();
        for (int i = 0; i < 4; ++i)
        {
            const auto r = rowRect (i);
            const bool sel = (i == cur);
            const bool hot = (i == hoverRow);

            if (sel || hot)
            {
                g.setColour (sel ? juce::Colour (0xff123a4d) : juce::Colour (0x22ffffff));
                g.fillRoundedRectangle (r.reduced (3.0f, 2.0f), 6.0f);
            }
            if (sel)
            {
                g.setColour (juce::Colour (0xff29b6f6).withAlpha (0.90f));
                g.drawRoundedRectangle (r.reduced (3.0f, 2.0f), 6.0f, 1.0f);
                g.fillEllipse (r.getX() + 7.0f, r.getCentreY() - 3.0f, 6.0f, 6.0f);
            }

            g.setColour (sel ? juce::Colours::white : juce::Colour (0xffcfe3ee));
            g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 10.5f,
                                                      sel ? juce::Font::bold : juce::Font::plain)));
            g.drawText (MediaPlayAudioProcessor::playModeName ((MediaPlayAudioProcessor::PlayMode) i),
                        juce::Rectangle<float> (r.getX() + 17.0f, r.getY(),
                                                r.getWidth() - 21.0f, r.getHeight()),
                        juce::Justification::centredLeft);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int r = rowAt (e.position.y);
        if (r != hoverRow) { hoverRow = r; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override { hoverRow = -1; repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int r = rowAt (e.position.y);
        if (r >= 0 && onModeChosen) onModeChosen (r);
    }

private:
    // 播放模式按钮已迁到侧边栏（频谱左侧），弹窗高度收敛到 166px，
    // 保证向上弹出时不会越过窗口顶部。
    juce::Rectangle<float> rowRect (int i) const
    {
        return { 3.0f, 26.0f + (float) i * 34.0f, (float) getWidth() - 6.0f, 32.0f };
    }

    int rowAt (float y) const
    {
        for (int i = 0; i < 4; ++i)
        {
            const auto r = rowRect (i);
            if (r.contains (r.getCentreX(), y)) return i;
        }
        return -1;
    }

    MediaPlayAudioProcessor& proc;
    int hoverRow = -1;
};

//==============================================================================
// 输出源弹窗：原声 / 伴奏 / 人声 三选一，样式与播放模式弹窗一致。
// 触发按钮只有在「当前曲目已分离」时才 enabled（由 editor 定时同步）。
class SourcePopup : public juce::Component
{
public:
    std::function<void (int)> onSourceChosen;

    explicit SourcePopup (MediaPlayAudioProcessor& p) : proc (p)
    {
        setAlwaysOnTop (true);
    }

    void syncFromProcessor() { repaint(); }

    void paint (juce::Graphics& g) override
    {
        drawVerticalPopupPanel (g, *this, MediaPlayAudioProcessor::zh ("输出源"));

        const int cur = proc.getOutputSource();
        for (int i = 0; i < 3; ++i)
        {
            const auto r = rowRect (i);
            const bool sel = (i == cur), hot = (i == hoverRow);

            if (sel || hot)
            {
                g.setColour (sel ? juce::Colour (0xff123a4d) : juce::Colour (0x22ffffff));
                g.fillRoundedRectangle (r.reduced (3.0f, 2.0f), 6.0f);
            }
            if (sel)
            {
                g.setColour (juce::Colour (0xff29b6f6).withAlpha (0.90f));
                g.drawRoundedRectangle (r.reduced (3.0f, 2.0f), 6.0f, 1.0f);
                g.fillEllipse (r.getX() + 7.0f, r.getCentreY() - 3.0f, 6.0f, 6.0f);
            }

            g.setColour (sel ? juce::Colours::white : juce::Colour (0xffcfe3ee));
            g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 11.5f,
                                                      sel ? juce::Font::bold : juce::Font::plain)));
            g.drawText (MediaPlayAudioProcessor::outputSourceName (i),
                        juce::Rectangle<float> (r.getX() + 18.0f, r.getY(),
                                                r.getWidth() - 22.0f, r.getHeight()),
                        juce::Justification::centredLeft);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int r = rowAt (e.position.y);
        if (r != hoverRow) { hoverRow = r; repaint(); }
    }
    void mouseExit (const juce::MouseEvent&) override { hoverRow = -1; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        const int r = rowAt (e.position.y);
        if (r >= 0 && onSourceChosen) onSourceChosen (r);
    }

private:
    juce::Rectangle<float> rowRect (int i) const
    {
        return { 3.0f, 26.0f + (float) i * 42.0f, (float) getWidth() - 6.0f, 40.0f };
    }
    int rowAt (float y) const
    {
        for (int i = 0; i < 3; ++i)
            if (rowRect (i).contains (rowRect (i).getCentreX(), y)) return i;
        return -1;
    }

    MediaPlayAudioProcessor& proc;
    int hoverRow = -1;
};

//==============================================================================
// 分离状态条（主区顶部）：把「有没有分离、缓存文件到底在哪」直接摆在界面上。
// 旧实现的「预分离」只在内存里留结果、根本没落盘，用户自然找不到任何文件；
// 现在统一写入 <曲目目录>/AI分离输出/，这里显示完整路径并可直接打开目录。
class SepStatusStrip : public juce::Component,
                       public juce::SettableTooltipClient
{
public:
    // SettableTooltipClient 自带 setTooltip() / getTooltip()；
    // 路径过长被省略时，悬停状态条即可看到完整路径
    explicit SepStatusStrip (MediaPlayAudioProcessor& p) : proc (p)
    {
        addAndMakeVisible (openBtn);
        openBtn.onClick = [this]
        {
            auto d = proc.getVocalCacheDir();
            if (d == juce::File()) return;
            if (! d.isDirectory()) d.createDirectory();
            d.startAsProcess();   // 资源管理器里直接看到 _人声.wav / _伴奏.wav
        };
    }

    // 中英切换时重建静态按钮文案与状态串缓存
    void applyLanguage()
    {
        openBtn.setButtonText (MediaPlayAudioProcessor::zh ("打开目录"));
        line1 = {}; line2 = {};
        repaint();
    }

    // 编辑器定时器以 ~4Hz 调用；只在状态串变化时重绘。
    // 严禁在空闲时每 33ms 重建中文 String / Tooltip（宿主堆损坏的老教训）。
    void refresh()
    {
        const bool busy  = proc.isPreSeparating();
        const bool ready = proc.hasVocalSource();
        juce::String a, b;
        if (busy)
        {
            a = MediaPlayAudioProcessor::zh ("音频分离中 ")
              + juce::String (juce::roundToInt (proc.getPreSepProgress() * 100.0f)) + "%";
            b = MediaPlayAudioProcessor::zh ("将写入：") + proc.getVocalCachePathText();
        }
        else if (ready)
        {
            a = MediaPlayAudioProcessor::zh ("✓ 已分离 · 可切换 原声 / 伴奏 / 人声");
            b = MediaPlayAudioProcessor::zh ("缓存：") + proc.getVocalCachePathText();
        }
        else
        {
            a = MediaPlayAudioProcessor::zh ("尚未分离 · 点「分离音频」生成");
            b = MediaPlayAudioProcessor::zh ("将生成到：") + proc.getVocalCachePathText();
        }
        if (a != line1 || b != line2) { line1 = a; line2 = b; repaint(); }

        const auto path = proc.getVocalCachePathText();
        if (path != lastPath) { lastPath = path; setTooltip (path); }
        const bool canOpen = (proc.getVocalCacheDir() != juce::File());
        if (openBtn.isEnabled() != canOpen) openBtn.setEnabled (canOpen);
    }

    void resized() override
    {
        openBtn.setBounds (getWidth() - 86, 3, 82, 28);
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xd8061a24));
        g.fillRoundedRectangle (b, 7.0f);
        g.setColour (juce::Colour (0xff29b6f6).withAlpha (0.28f));
        g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);

        const float textW = b.getWidth() - 96.0f;
        const bool ready = proc.hasVocalSource();
        const bool busy  = proc.isPreSeparating();
        g.setColour (ready ? juce::Colour (0xff8bc34a)
                           : (busy ? juce::Colour (0xffffd54f) : juce::Colour (0xff9dc0d0)));
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 11.5f, juce::Font::bold)));
        g.drawText (line1, juce::Rectangle<float> (12.0f, 1.0f, textW, 16.0f),
                    juce::Justification::centredLeft, true);

        g.setColour (juce::Colour (0xffb9d4e0));
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 10.0f, juce::Font::plain)));
        g.drawText (line2, juce::Rectangle<float> (12.0f, 17.0f, textW, 15.0f),
                    juce::Justification::centredLeft, true);
    }

private:
    MediaPlayAudioProcessor& proc;
    PillButton openBtn { MediaPlayAudioProcessor::zh ("打开目录") };
    juce::String line1, line2, lastPath;
};

//==============================================================================
// AI 分离面板：提取人声 / 提取伴奏 / 全部提取（MDX-Net 离线模型，导出 WAV 文件）
// 来源 = 播放列表选中项（可多选）或当前播放曲目；完成后可打开目录、双击结果试听
class AiPanel : public juce::Component
{
public:
    std::function<juce::Array<juce::File>()> getSelectedFiles;

    explicit AiPanel (MediaPlayAudioProcessor& p)
        : proc (p), exporter (p.getExporter())
    {
        addAndMakeVisible (vocBtn);
        addAndMakeVisible (instrBtn);
        addAndMakeVisible (bothBtn);
        vocBtn.onClick   = [this] { begin (AiExporter::Mode::Vocal); };
        instrBtn.onClick = [this] { begin (AiExporter::Mode::Instr); };
        bothBtn.onClick  = [this] { begin (AiExporter::Mode::Both); };

        sourceLabel.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 12.5f, juce::Font::plain)));
        sourceLabel.setColour (juce::Label::textColourId, juce::Colour (0xffcfe3ee));
        addAndMakeVisible (sourceLabel);

        statusLabel.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 12.0f, juce::Font::plain)));
        statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9adcff));
        addAndMakeVisible (statusLabel);

        // 进度条改为 paint() 自绘：不再挂一个自带 Timer 的 juce::ProgressBar
        // 子控件（VST3 宿主 STA 消息线程上额外的定时重绘是高风险路径）。
        addAndMakeVisible (dirBtn);
        dirBtn.onClick = [this]
        {
            auto d = exporter.getOutputDir();
            if (d.isDirectory()) d.startAsProcess();   // 打开资源管理器
        };

        resultModel.onOpen = [] (const juce::File& f)
        {
            if (f.existsAsFile()) f.startAsProcess();   // 调用系统默认播放器试听
        };
        resultList.setModel (&resultModel);
        resultList.setRowHeight (24);
        resultList.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff0c2b39));
        resultList.setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (resultList);
    }

    // 由编辑器定时器调用：同步进度 / 状态 / 结果列表
    void refresh()
    {
        progressValue = exporter.overallProgress();
        const bool busy = exporter.isBusy();

        // 15:52 两次插件宿主崩溃（WER 0xc0000005，ntdll+0x19daa / 0x1a1cc 的
        // 空链表节点解引用）都发生在「切到 AI 页且没有导出任务」时。此时旧实现
        // 仍每 33ms 重建一遍中文 juce::String、Label 文本、Tooltip 与 ListBox 内容。
        // 空闲时只更新一个 double 并 repaint；导出中把重活降到约 4Hz。
        if (busy)
        {
            if (++refreshTick % 8 != 0) { repaint(); return; }
        }
        else if (initialised && ! wasBusy)
        {
            repaint();
            return;
        }
        if (! initialised)
        {
            initialised = true;
            MediaPlayAudioProcessor::debugLog ("[AI] panel first refresh (idle, throttled)");
        }
        wasBusy = busy;

        const auto outDir = exporter.getOutputDir();
        const auto status = exporter.getStatusText();
        juce::String src = sourceText();
        // 把完整输出目录直接摆在面板上（以前只能靠「查看目录」按钮猜）
        if (outDir != juce::File() && outDir.isDirectory())
            src += zh ("  |  输出：") + outDir.getFullPathName();
        sourceLabel.setText (src, juce::dontSendNotification);
        statusLabel.setText (status, juce::dontSendNotification);

        auto res = exporter.getResults();
        if (res.size() != resultModel.files.size())
        {
            resultModel.files = res;
            resultList.updateContent();
        }
        dirBtn.setEnabled (outDir.isDirectory());
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const float w = (float) getWidth();

        // 背景（与混音台同款）
        juce::ColourGradient bg (juce::Colour (0xff0a2230), 0.0f, 0.0f,
                                 juce::Colour (0xff040d16), w * 0.35f, (float) getHeight(), false);
        g.setGradientFill (bg);
        g.fillAll();

        // 顶部标题条 0..34
        g.setColour (juce::Colour (0xff071a26));
        g.fillRect (0.0f, 0.0f, w, 34.0f);
        g.setColour (juce::Colour (0xff1b3c4c).withAlpha (0.7f));
        g.fillRect (0.0f, 33.0f, w, 1.0f);

        g.setColour (juce::Colour (0xff29b6f6));
        g.fillRoundedRectangle (14.0f, 12.0f, 4.0f, 16.0f, 2.0f);
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 14.0f, juce::Font::bold)));
        g.setColour (juce::Colours::white);
        g.drawText (MediaPlayAudioProcessor::zh ("AI 智能分离 · MDX-Net"),
                    juce::Rectangle<float> (26.0f, 0.0f, 300.0f, 34.0f),
                    juce::Justification::centredLeft);

        // 自绘进度条（与 resized() 的布局一致：x=14, y=104, w=getWidth()-28, h=16）
        const float bx = 14.0f, by = 104.0f, bh = 16.0f;
        const float bw = juce::jmax (0.0f, w - 28.0f);
        g.setColour (juce::Colour (0xff10303e));
        g.fillRoundedRectangle (bx, by, bw, bh, 3.0f);
        const float fw = bw * juce::jlimit (0.0f, 1.0f, (float) progressValue);
        if (fw > 0.0f)
        {
            g.setColour (juce::Colour (0xff29b6f6));
            g.fillRoundedRectangle (bx, by, fw, bh, 3.0f);
        }

        const bool busy = exporter.isBusy();
        g.setColour (busy ? juce::Colour (0xff33d433) : juce::Colour (0xff6f93a5));
        g.fillEllipse (w - 90.0f, 15.0f, 6.0f, 6.0f);
        g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 10.5f, juce::Font::plain)));
        g.setColour (juce::Colour (0xff9dc0d0));
        g.drawText (busy ? MediaPlayAudioProcessor::zh ("导出中") : MediaPlayAudioProcessor::zh ("空闲"),
                    juce::Rectangle<float> (w - 80.0f, 4.0f, 70.0f, 26.0f),
                    juce::Justification::centredLeft);
    }

    // 布局：标题条 34 → 来源 → 三按钮 → 进度条 → 状态 → 查看目录 → 结果列表
    void resized() override
    {
        auto a = getLocalBounds();
        a.removeFromTop (34);                       // 顶部标题条（paint 绘制）
        a.reduce (14, 0);
        sourceLabel.setBounds (a.removeFromTop (20));
        a.removeFromTop (8);
        auto row = a.removeFromTop (32);
        const int bw = (row.getWidth() - 12) / 3;
        vocBtn.setBounds   (row.removeFromLeft (bw)); row.removeFromLeft (6);
        instrBtn.setBounds (row.removeFromLeft (bw)); row.removeFromLeft (6);
        bothBtn.setBounds  (row.removeFromLeft (bw));
        a.removeFromTop (10);
        a.removeFromTop (16);        // 进度条由 paint() 自绘，无子控件
        a.removeFromTop (6);
        statusLabel.setBounds (a.removeFromTop (18));
        a.removeFromTop (8);
        dirBtn.setBounds (a.removeFromTop (28).removeFromLeft (110));
        a.removeFromTop (8);
        resultList.setBounds (a);
    }

private:
    static juce::String zh (const char* s) { return MediaPlayAudioProcessor::zh (s); }

    void begin (AiExporter::Mode m)
    {
        if (exporter.isBusy()) return;

        auto files = getSelectedFiles ? getSelectedFiles() : juce::Array<juce::File>();
        if (files.isEmpty())
        {
            const int cur = proc.getCurrentTrackIndex();
            if (cur >= 0) files.add (proc.getPlaylistFile (cur));
        }
        if (files.isEmpty())
        {
            statusLabel.setText (zh ("请先导入音频并选中（Ctrl+点击可多选）"),
                                 juce::dontSendNotification);
            return;
        }
        exporter.startExport (files, m);
    }

    juce::String sourceText()
    {
        auto sel = getSelectedFiles ? getSelectedFiles() : juce::Array<juce::File>();
        if (sel.size() > 1)
            return zh ("来源：已选 ") + juce::String (sel.size()) + zh (" 首音频");
        if (sel.size() == 1)
            return zh ("来源：") + sel[0].getFileName();
        if (proc.getCurrentTrackIndex() >= 0)
            return zh ("来源：当前曲目 ") + proc.getCurrentTrackTitle();
        return zh ("来源：未选择音频");
    }

    struct ResultModel : juce::ListBoxModel
    {
        juce::Array<juce::File> files;
        std::function<void (const juce::File&)> onOpen;

        int getNumRows() override { return files.size(); }

        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
        {
            g.setColour (juce::Colour (0xff33d433));
            g.fillEllipse (8.0f, h * 0.5f - 2.5f, 5.0f, 5.0f);
            g.setColour (juce::Colour (0xffcfe3ee));
            g.setFont (juce::Font (juce::FontOptions ("Microsoft YaHei UI", 12.0f, juce::Font::plain)));
            g.drawText (files[row].getFileName(),
                        juce::Rectangle<int> (20, 0, w - 26, h),
                        juce::Justification::centredLeft, true);
        }

        void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
        {
            if (onOpen && row >= 0 && row < files.size()) onOpen (files[row]);
        }
    };

    MediaPlayAudioProcessor& proc;
    AiExporter& exporter;

    PillButton vocBtn   { zh ("提取人声") };
    PillButton instrBtn { zh ("提取伴奏") };
    PillButton bothBtn  { zh ("全部提取") };
    PillButton dirBtn   { zh ("查看目录") };
    juce::Label sourceLabel, statusLabel;
    double progressValue = 0.0;
    int refreshTick = 0;
    bool initialised = false;
    bool wasBusy = false;
    ResultModel resultModel;
    juce::ListBox resultList { "", &resultModel };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AiPanel)
};

//==============================================================================
class MediaPlayAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MediaPlayAudioProcessorEditor (MediaPlayAudioProcessor&);
    ~MediaPlayAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;

private:
    void timerCallback() override;
    static juce::String formatTime (double seconds);
    void closeAllPopups();
    void syncPopupChrome();
    void handleGlobalMouseDown (juce::Component* c);
    void applyWindowTitle();
    void applyTab();
    // 中英切换：重建所有静态文案 + 重绘所有自绘面板
    void applyLanguage();

    MediaPlayAudioProcessor& processorRef;

    // 已整合为单一「音频播放」页：频谱 + 歌词 + 分离状态条，默认即显示。
    // 混音台 / AI 批量面板不再挂载、也不再在定时器里刷新（宿主 UI 线程零负担）。
    VisualizerComponent visualizer;
    LyricsOverlay lyricsOverlay;
    PlaylistComponent playlistBox;
    SepStatusStrip sepStatus;

    PillButton openBtn   { MediaPlayAudioProcessor::zh ("导入音频") };
    // 「分离音频」紧随「导入音频」下方（侧边栏），不再占底部按钮行
    PillButton sepBtn    { MediaPlayAudioProcessor::zh ("分离音频") };
    // 旁通：机架效果器语义——只把宿主输入原样送出，不播放插件内置播放器
    PillButton bypassBtn { MediaPlayAudioProcessor::zh ("旁通"), true };
    // 中英文切换：固定文案「中 / EN」，点亮表示当前为 English
    PillButton langBtn   { MediaPlayAudioProcessor::zh ("中 / EN") };

    IconButton prevBtn { IconButton::Prev, true };
    IconButton playBtn { IconButton::Play, true };
    IconButton nextBtn { IconButton::Next, true };
    // 「分离」按钮已移除：S 全部熄灭即原声直通，点亮任意 S 才自动进入分离
    PillButton spectrumBtn { MediaPlayAudioProcessor::zh ("频谱"), true };
    PillButton lyricBtn { MediaPlayAudioProcessor::zh ("歌词"), true };
    PillButton offsetBtn { MediaPlayAudioProcessor::zh ("偏移") };
    LyricOffsetPopup offsetPopup;
    // 音量：与「偏移」同款——底部按钮点击向上弹出垂直调节器
    PillButton volumeBtn { MediaPlayAudioProcessor::zh ("音量") };
    VolumePopup volumePopup;

    // 播放模式：从底部传输区迁到频谱左侧（侧边栏，列表上方）
    PillButton modeBtn { MediaPlayAudioProcessor::zh ("列表循环") };
    PlayModePopup modePopup;

    // 输出源切换（未分离时按钮置灰）；分离入口已移到侧边栏 openBtn 下方
    PillButton sourceBtn { MediaPlayAudioProcessor::zh ("原声") };
    SourcePopup sourcePopup;

    // 侧边栏：当前曲目 + 分离状态（多行小字）
    juce::Label trackLabel;
    int lastMode = -1;
    int sepTick = 0;
    juce::String lastSepBtnText, lastSourceText, lastTrackText;
    int lastLanguage = -1;   // timerCallback 检测宿主状态恢复导致的语言变化

    juce::Label timeLabel;
    juce::Slider seekSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };

    juce::Label appTitle { {}, MediaPlayAudioProcessor::zh ("S-Rack_媒体播放") };

    PlayerLookAndFeel lookAndFeel;
    std::unique_ptr<juce::FileChooser> fileChooser;
    bool seekDragging = false;
    int lastPlaylistSize = -1;
    int lastCurTrack = -1;

    // 诊断自动加载（沙箱测试用）
    bool autoLoaded = false;
    bool seekTested = false;
    double bootMs = juce::Time::getMillisecondCounterHiRes();
    int titleCheckTick = 0;   // 兜底：窗口挂载完成后再校正一次中文标题

    // 点击弹窗/触发按钮以外的区域时，统一收起两个向上弹窗
    struct DismissListener : public juce::MouseListener
    {
        MediaPlayAudioProcessorEditor& owner;
        explicit DismissListener (MediaPlayAudioProcessorEditor& o) : owner (o) {}

        void mouseDown (const juce::MouseEvent& e) override
        {
            owner.handleGlobalMouseDown (e.originalComponent);
        }
    };
    DismissListener dismissListener { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MediaPlayAudioProcessorEditor)
};
