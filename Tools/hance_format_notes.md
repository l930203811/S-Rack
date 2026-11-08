# HANCE .hance 模型格式逆向笔记（★ 格式已完全破解，权重提取 100%）

目标：复刻 Acon Digital Remix 的实时分离。引擎=内嵌 Hance 运行时，每轨独立小模型（~1M 参数，这是实时的原因）。
模型文件：C:\ProgramData\Acon Digital\AIModels\{Vocals,Bass,Drums,Piano,Guitar}Separation.hance

## 最终解析器：Tools/parse_hance7.js（权威版本）

Vocals=1,063,240 params；Drums=1,063,240（同构）；Bass=952,648。输出 vocals_weights.bin + vocals_weights.json（35 张量，owner/dims/offset）。

## 文件格式（完整）

### 头部
- `0B 00 00 00` + "HANCE.model" + u32 ver=0x1004 + u32=10
- @0x16: f64 疑似 44096.0（未解）；@0x1D~0x31 结构块；第一节点 @0x32

### 条目判别规则（关键！）
全文件只有两种内容：`[u32 len][ASCII 名]` 条目 和 张量/小结构。
- **ref**：名字后紧跟另一个 `len+名` → 是前一个 def 的引用链成员
- **def（节点定义）**：名字后 u16 tag ∈ {0x02,0x03,0x05,0x06,0x08,0x10,0x11,0x13} 且 tag 后紧跟 `len+名`（≥1 个 ref）
- 陷阱：ref 名字后的张量类型字节 02/03 会被误判为 def tag（如 uconcat5 ref 后跟 [256,256] 张量）→ 必须用「tag 后是否有合法 ref」判别

### 张量编码（4 种，全部确认）
1. `[01][u32 count][u32×count 大端]` — int 数组（prenorm: 8 个值，每个=62 频带相关）
2. `[02][u32 d0][u32 d1][f32×d0×d1 LE]` — rank2
3. `[03][u32 d0][d1][d2][f32×prod]` — rank3
4. `[04][d0][d1][d2][d3][f32×prod]` — rank4（**无独立 rank 字段！** 之前误读的 [3,16,32] 实为 [3,3,16,32] 的 rank4）

### 节点尾部的元数据块（无权重，可跳过）
- `[12 00][02 00][len+名]×2[00]` — 连接表（18 字节头）
- `[00 00][名][00]` 变体
- 各 def 的 tensor 之间/之后散布 18~31 字节此类块

## 网络结构（Vocals，62 频带 × T=448 帧，6 级 U-Net + 通道注意力）

### 数据流
```
input[T=448,F=62] → prenorm → dconv0..5(2D卷积) → 瓶颈注意力 dact5 → uconv5..0 → uact0(输出)
每级: dconvN ↔ dattN(通道注意力) ↔ dactN(门控融合) → dattactN → dattmultN → 下一级
skip: dactN → uconcatN(FiLM门控表) → uattN(通道注意力) → uattmultN
```

### 权重清单（Vocals，@=文件偏移）
| 张量 | 形状 | 参数 | 语义 |
|---|---|---|---|
| prenorm | int[8] | 8 | 62 频带分组参数 |
| dconv0 | [3,4,1,16] | 192 | 2D卷积 kT=3,kF=4, 1→16ch |
| dconv1 | [3,3,16,32] | 4608 | 16→32 |
| dconv2 | [5,3,32,64] | 30720 | 32→64 (kT=5) |
| dconv3 | [2,3,64,128] | 49152 | 64→128 |
| dconv4 | [2,3,128,128] | 98304 | 128→128 |
| dconv5 | [8,1,128,128] | 131072 | 瓶颈 kT=8,kF=1 |
| datt0..4 | [C,C] + [T_l,C] | 256+7168 … | 通道注意力 + 逐位置门控 |
| dact5 | [28,28,128] + [28,128] | 100352+3584 | 瓶颈全时序自注意力 |
| bndense/bnact | (并入 dact5/uconv5 区) | — | BN+激活 |
| uconv5 | [1,1,128,128] | 16384 | 逐点卷积 |
| uconv4 | [2,3,128,256] | 196608 | 256→128(输入=concat 128+128) |
| uconv3 | [2,3,64,256] | 98304 | 256→64 |
| uconv2 | [3,3,32,128] | 36864 | 128→32 |
| uconv1 | [3,3,16,64] | 9216 | 64→16 |
| uconv0 | [3,4,1,32] | 384 | 32→1 输出 |
| uatt4/3 | [256,256] | 65536×2 | 上采样级通道注意力 |
| uatt2/1/0 | [128,128]/[64,64]/[32,32] | | |
| uconcat5..1 | [T_l, C_concat] | 7168~14336 | skip 融合门控表 |
| 时间维 | 448→224→112→56→28(×2级) | | 每级 ÷2 |

### 卷积权重布局
[kT, kF, C_in, C_out]（待实现时用数值实验确认 in/out 轴序；uconv4 [2,3,128,256] 若 in=128 则 out=256→后接 concat；若 [out,in] 则 in=256 更合理）

## 未解（非阻塞）
- dactN 第二定义（tag 0x1002/0x2002/0x4002/0x8002）逐通道激活增益 ~240 参数未提取
- 各激活函数具体形式（0x11=输出激活，可能 sigmoid/ReLU）
- STFT 窗长/_hop_/bin→62 频带映射（模型头 0x16 f64=44096 可能相关：448 帧×~98 hop?）
- [T,C] 门控表的确切应用方式（乘性 FiLM 最可能）

## Remix.vst3 探查结论（2026-11-08 更新，重大突破）
- 真实路径：`C:\Program Files\Common Files\VST3\Acon Digital\Remix.vst3`（20,854,784 B；注意 `VST3\Remix\` 目录只有卸载器，二进制在 `Acon Digital\` 子目录）
- mcp_rea 分析器（Ghidra/Hopper/IDA）本机均不可用；改用 PowerShell 提取 MSVC RTTI 字符串成功

### Hance 运行时完整图层类（RTTI `neural@acon` 命名空间）
ArithmeticLayer, BaseLayer, BaseQueuedLayer, ConcatLayer, ControlLayer, ConvLayer, DenseLayer,
ELULayer, FillMaskLayer, FixationLayer, InputLayer, LeakyReLULayer, LogLayer, NeuralModel,
NormLayer, OutputLayer, PreNormLayer, ReLULayer, SelfAttentionLayer, SeparableChannelConvLayer,
SigmoidLayer, SoftplusLayer, TransposedConvLayer, TensorData<float>

### def tag → 图层映射（dump_graph.js 输出确认）
- 0x02 = ConvLayer（下采样，TF 布局 [kT,kF,Cin,Cout]）
- 0x03 = TransposedConvLayer（上采样，TF 布局 [kT,kF,Cout,Cin]；uconv4 [2,3,128,256]=256入128出，与 concat 尺寸完美吻合）
- 0x13 = SeparableChannelConvLayer（dattN/uattN：逐点 [Cout,Cin] + 逐通道时间核 [T_l,Cout]）
- 0x05 / 0x06 = 无参数激活层（dactN/dattactN/uactN/bnact；dact5 名下的 [28,28,128]+[28,128] 实为瓶颈 Dense/SelfAttention 权重，bndense 是其输出张量名）
- 0x11 = OutputLayer（uact0，掩码输出，大概率 sigmoid）
- 0x10 = PreNormLayer（权重=8×u32(BE)=62，非频带表）
- 全网络无偏置、无 norm 参数（scan_gaps.js 证实权重外没有任何小数组）

### 图连接语义（refs=[输入, 消费者/输出张量名]）
- dattmultN / uconcatN / uattmultN / bndense / uact0 = 张量名（连接名），非图层
- skip 路径：dattmultN(T_l,C) → 与上采样输出 concat 成 uconcatN(T_l,2C) → uattN 分离卷积 → uattmultN → uconvN
  （uatt3 w=[256,256]+[56,256] 与 concat(128+128)@T=56 完全吻合，已验证）
- 连接表元数据块（18B 头 + 名字对）携带 concat 的第二输入

### 维度推演（'same' padding 假设，待数值标定）
- 时间 T：448 →224→112→56→28（dconv0..3 stride2；dconv4/5 stride1）
- 频带 F：62→31→16→8→4→2（全部 stride2；dconv5 kF=1 保持 2）
- 瓶颈：T=28, F=2, C=128 + Dense [28→28]×128ch + [28,128]
- 上采样：uconv4/3 (kT=2,s2) (28,2)→(56,4)→(112,8)；uconv2→(224,16)；uconv1→(448,31)；uconv0 sT=1,sF=2→(448,62)

### 仍未解（需行为标定）
- 激活函数选择（ELU vs LeakyReLU；运行时两者都有）
- STFT 窗长/hop/bin→62 频带映射（头 f64@0x15=44096.0 未破；448×98.43≈44096）
- prenorm 8×62 的作用、dattactN 的具体算子（激活 or Arithmetic 加法）
- LogLayer 存在 → 输入可能是 log 幅度谱

### 工具新增
- Tools/dump_graph.js — 图结构 dump（def/tag/refs/张量）
- Tools/scan_gaps.js — 缺口小数组扫描（已证实无遗漏权重）

## 工具
- parse_hance7.js — 权威解析器（--out 导出 bin+json）
- scan_tensors.js / dump_hance2-4.js — 调试 dump
- parse_hance4/5/6.js — 历史版本（有 rank 误读 bug，勿用）
