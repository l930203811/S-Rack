#pragma once

#include <atomic>
#include <string>
#include <unordered_map>

//==============================================================================
// S-Rack 轻量 UI 双语层（默认简体中文，可一键切 English）。
//
// 所有界面文字统一经 MediaPlayAudioProcessor::zh() 构造：中文模式原样返回，
// 英文模式按下面的白名单替换。白名单之外的字符串保持中文，这是刻意的：
//   * 目录名「AI分离输出」、文件名「_人声.wav」「_伴奏.wav」「导出结果.txt」
//     绝不能翻译，否则分离结果会写到一个目录、又从另一个目录读取；
//   * AiExporter / VocalCache 写进 mp_debug.log 与「导出结果.txt」的日志、
//     清单文本用各自的 Z()/z()，不经过 zh()，因此始终固定为中文报告。
// 语言为进程级设置，并由宿主状态（getStateInformation）持久化。
//==============================================================================
namespace L10n
{
    // 0 = 简体中文（默认），1 = English
    inline std::atomic<int>& languageRef()
    {
        static std::atomic<int> v { 0 };
        return v;
    }

    inline int  get()       { return languageRef().load (std::memory_order_relaxed); }
    inline void set (int v) { languageRef().store (v == 1 ? 1 : 0, std::memory_order_relaxed); }
    inline bool isEnglish() { return get() == 1; }

    // 返回 nullptr 表示该串不翻译（保持中文）
    inline const char* lookup (const char* cn)
    {
        if (! isEnglish()) return nullptr;

        static const std::unordered_map<std::string, const char*> table =
        {
            // ---- 产品 / 标题 ----
            { "媒体播放",                       "Media Play" },
            { "媒体播放器",                     "Media Player" },
            { "S-Rack_媒体播放",                "S-Rack_Media Play" },

            // ---- 侧边栏 / 主按钮 ----
            { "导入音频",                       "Import Audio" },
            { "分离音频",                       "Separate Audio" },
            { "音频分离",                       "Separate Audio" },   // 旧文案兼容
            { "分离中 ",                        "Separating " },
            { "重新分离",                       "Re-separate" },
            { "旁通",                           "Bypass" },
            { "旁通中 · 仅透传宿主输入",        "Bypassed · host input only" },

            // ---- 输出源 / 播放模式 ----
            { "原声",                           "Original" },
            { "伴奏",                           "Instrumental" },
            { "人声",                           "Vocals" },
            { "输出源",                         "Output" },
            { "播放模式",                       "Play Mode" },
            { "列表循环",                       "Loop All" },
            { "顺序播放",                       "Sequential" },
            { "单曲循环",                       "Loop One" },
            { "随机播放",                       "Shuffle" },

            // ---- 底部开关 ----
            { "频谱",                           "Spectrum" },
            { "歌词",                           "Lyrics" },
            { "偏移",                           "Offset" },
            { "音量",                           "Volume" },

            // ---- 弹窗 ----
            { "歌词偏移",                       "Lyric Offset" },
            { "提前",                           "Earlier" },
            { "延后",                           "Later" },

            // ---- 播放列表 / 文件选择 ----
            { "播放列表为空",                   "Playlist is empty" },
            { "删除选中",                       "Remove Selected" },
            { "清空列表",                       "Clear Playlist" },
            { "选择音频文件（可多选）",         "Select audio files (multiple)" },

            // ---- 状态条 ----
            { "音频分离中 ",                    "Separating audio " },
            { "将写入：",                       "Will write to: " },
            { "将生成到：",                     "Will be saved to: " },
            { "缓存：",                         "Cache: " },
            { " | 缓存：",                      " | Cache: " },
            { "✓ 已分离 · 可切换 原声 / 伴奏 / 人声",
                                                "✓ Separated · Original / Instrumental / Vocals" },
            { "尚未分离 · 点「分离音频」生成",  "Not separated · Click Separate Audio" },
            { "打开目录",                       "Open Folder" },

            // ---- 侧边栏状态多行文本 ----
            { "当前曲目：\n",                   "Track:\n" },
            { "分离中，请稍候…\n",              "Separating, please wait...\n" },
            { "✓ 已分离\n",                     "✓ Separated\n" },
            { "尚未分离\n",                     "Not separated\n" },
            { "输出目录：\n",                   "Output folder:\n" },
            { "\n（与源文件同目录）",           "\n(same folder as source)" },

            // ---- 处理器侧状态（仅 UI 可见部分） ----
            { "分离完成 · 已生成缓存：",        "Separation complete · cache: " },
            { "分离未成功 · ",                  "Separation failed · " },
            { " · 输出目录：",                  " · Output folder: " },
            { "（未加载曲目）",                 "(no track loaded)" },
            { "未加载曲目",                     "No track loaded" },
            { "已分离 · ",                      "Separated · " },
            { "尚未分离 · ",                    "Not separated · " },
            { "上一轮分离正在退出，请稍后重试", "Previous separation is stopping, please retry" },
            { "正在分离 · 将写入：",            "Separating · will write to: " },

            // ---- 旧 AI 面板（已不挂载，保留映射避免遗留代码显示中文） ----
            { "  |  输出：",                    "  |  Output: " },
            { "AI 智能分离 · MDX-Net",          "AI Separation · MDX-Net" },
            { "导出中",                         "Exporting" },
            { "空闲",                           "Idle" },
            { "请先导入音频并选中（Ctrl+点击可多选）",
                                                "Import and select audio first (Ctrl+click for multiple)" },
            { "来源：已选 ",                    "Source: " },
            { " 首音频",                        " files" },
            { "来源：",                         "Source: " },
            { "来源：当前曲目 ",                "Source: current track " },
            { "来源：未选择音频",               "Source: no audio selected" },
            { "提取人声",                       "Extract Vocals" },
            { "提取伴奏",                       "Extract Instrumental" },
            { "全部提取",                       "Extract Both" },
            { "查看目录",                       "View Folder" },
        };

        const auto it = table.find (std::string (cn));
        return it == table.end() ? nullptr : it->second;
    }
}
