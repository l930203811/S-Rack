cmake_minimum_required(VERSION 3.22)
set(CMAKE_CXX_STANDARD 17)
project(SRack VERSION 1.0.0 LANGUAGES C CXX)

add_subdirectory(ThirdParty/juce-8.0.15)

# 品牌名 / 应用名统一为 S-Rack。
# BUNDLE_ID 与 PLUGIN_CODE 刻意保留旧值：VST3 靠 class ID 识别插件，
# 这样宿主里已保存的旧工程仍能在重新扫描后找回同一个插件。
# target 名 VstMediaPlayer 也保留，仅作内部构建标识（build.bat / 产物目录不变）。
juce_add_plugin(VstMediaPlayer
    PRODUCT_NAME "S-Rack"
    COMPANY_NAME "SKCore"
    BUNDLE_ID "com.skcore.mediaplay"
    PLUGIN_MANUFACTURER_CODE Skcd
    PLUGIN_CODE MdPl
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_CATEGORIES Fx
    # Windows exe / VST3 bundle icon (JUCE generates icon.ico from these)
    ICON_BIG "Assets/S-Rack-icon.png"
    ICON_SMALL "Assets/S-Rack-icon-small.png"
)

target_sources(VstMediaPlayer PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/VisualizerComponent.cpp
    Source/Lyrics.h
    Source/StemSeparator.h
    Source/StemPanel.h
    Source/SeparationEngine.h
    Source/HanceEngine.h
    ThirdParty/kissfft/kiss_fft.c
    ThirdParty/kissfft/kiss_fftr.c
)

juce_generate_juce_header(VstMediaPlayer)

# 源码为 UTF-8（含中文界面文本），强制 MSVC 按 UTF-8 解析
target_compile_options(VstMediaPlayer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_compile_definitions(VstMediaPlayer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_USE_MP3AUDIOFORMAT=1
)

target_link_libraries(VstMediaPlayer PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
)

# Windows Media Foundation 解码（MP4/M4A/AAC/WMA 兜底）
if (WIN32)
    target_link_libraries(VstMediaPlayer PRIVATE mfplat mfuuid mfreadwrite)
endif()

# MDX-Net 后台人声分离（onnxruntime + kissfft）
set(ORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/onnxruntime-win-x64-1.17.1")
target_include_directories(VstMediaPlayer PRIVATE
    ThirdParty/kissfft
    ${ORT_DIR}/include
)
target_link_libraries(VstMediaPlayer PRIVATE ${ORT_DIR}/lib/onnxruntime.lib)

# 运行期部署：onnxruntime.dll 与模型随输出复制（Standalone exe 同目录 / VST3 bundle Contents\x86_64-win / 系统安装目录）
set(VST3_INSTALL_DIR "C:/Program Files/Common Files/VST3/S-Rack.vst3/Contents/x86_64-win")
add_custom_command(TARGET VstMediaPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_DIR}/lib/onnxruntime.dll"
        "$<TARGET_FILE_DIR:VstMediaPlayer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Kim_Vocal_2.onnx"
        "$<TARGET_FILE_DIR:VstMediaPlayer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_DIR}/lib/onnxruntime.dll"
        "$<TARGET_FILE_DIR:VstMediaPlayer>/../../VstMediaPlayer_artefacts/Release/VST3/S-Rack.vst3/Contents/x86_64-win"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Kim_Vocal_2.onnx"
        "$<TARGET_FILE_DIR:VstMediaPlayer>/../../VstMediaPlayer_artefacts/Release/VST3/S-Rack.vst3/Contents/x86_64-win"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_DIR}/lib/onnxruntime.dll"
        "${VST3_INSTALL_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Kim_Vocal_2.onnx"
        "${VST3_INSTALL_DIR}"
    # Standalone exe 同目录（exe 加载 onnxruntime.dll、引擎查找模型都需要）
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_DIR}/lib/onnxruntime.dll"
        "$<TARGET_FILE_DIR:VstMediaPlayer>/Standalone"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Kim_Vocal_2.onnx"
        "$<TARGET_FILE_DIR:VstMediaPlayer>/Standalone"
    # Hance 实时复刻引擎权重（Remix 逆向提取，见 Tools/hance_format_notes.md）
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Tools/vocals_weights.bin"
        "$<TARGET_FILE_DIR:VstMediaPlayer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Tools/vocals_weights.bin"
        "$<TARGET_FILE_DIR:VstMediaPlayer>/../../VstMediaPlayer_artefacts/Release/VST3/S-Rack.vst3/Contents/x86_64-win"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Tools/vocals_weights.bin"
        "${VST3_INSTALL_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Tools/vocals_weights.bin"
        "$<TARGET_FILE_DIR:VstMediaPlayer>/Standalone"
)

# 命令行音频解码工具（分析用，输出 48kHz 立体声交错 float32）
juce_add_console_app(AudioDump
    PRODUCT_NAME "AudioDump"
    COMPANY_NAME "SKCore"
    BUNDLE_ID "com.skcore.audiodump")

target_sources(AudioDump PRIVATE
    Tools/AudioDump.cpp
)

if (WIN32)
    target_link_libraries(AudioDump PRIVATE mfplat mfuuid mfreadwrite)
endif()

juce_generate_juce_header(AudioDump)

target_compile_definitions(AudioDump PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
)

target_compile_options(AudioDump PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_link_libraries(AudioDump PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
)

# MDX-Net 离线分离验证工具（onnxruntime + kissfft）
set(ORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/onnxruntime-win-x64-1.17.1")

juce_add_console_app(SepVerify
    PRODUCT_NAME "SepVerify"
    COMPANY_NAME "SKCore"
    BUNDLE_ID "com.skcore.sepverify")

target_sources(SepVerify PRIVATE
    Tools/SepVerify.cpp
    ThirdParty/kissfft/kiss_fft.c
    ThirdParty/kissfft/kiss_fftr.c
)

target_include_directories(SepVerify PRIVATE
    ThirdParty/kissfft
    ${ORT_DIR}/include
)

target_compile_definitions(SepVerify PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

target_compile_options(SepVerify PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_link_libraries(SepVerify PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    ${ORT_DIR}/lib/onnxruntime.lib
)

# 运行期需要 exe 同目录有 onnxruntime.dll
add_custom_command(TARGET SepVerify POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_DIR}/lib/onnxruntime.dll"
        "$<TARGET_FILE_DIR:SepVerify>"
)
