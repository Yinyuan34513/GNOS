# DRM 三方对决：GNOS vs Uinxed(移植源) vs Linux

GNOS 的 DRM 其实是**两套并存**：

## 参战方

| | GNOS legacy/ | GNOS ported/ | Linux 主线 DRM |
|---|---|---|---|
| 来源 | 自研 | 移植自 Uinxed-Kernel (ViudiraTech, Apache 2.0) | 原版 |
| 行数 | 1,311 | 13,962 | 核心 84,584 / 全树含驱动 **419,876** |
| 硬件 | bochs VBE_DISPI（QEMU 标准显卡） | 同样只有 VBE/fb 后端 | 数百种真实 GPU |
| 能力 | dumb buffer + 模式设置 + event/poll | atomic KMS、GEM、vblank、rbtree、idr、modeset lock——一套迷你 DRM core API | 全部 |

## 吐槽点

**GNOS legacy/（1.3k 行）**
- 聪明的务实主义：探测 `VBE_ID` 寄存器在 0xB0C0–0xB0C5 就上，
  不在就 "staying out of the way"——知道自己是 QEMU 里的王子
- dumb buffer + mmap 直通，labwc/pixman 这种软件渲染栈刚好够用
- 但它不是真 DRM：没有 GEM、没有 atomic， Weston 这类挑食的合成器直接拒收

**GNOS ported/（14k 行）**
- 从另一个教学内核（Uinxed）整体搬来一套"Linux DRM core 缩略版"
- 连 rbtree.c、intrusive_list.c 都一起搬了——为了 atomic 接口
  把数据结构课作业也抄了一遍
- drm_vsnprintf.c、drm_libc.c：给 freestanding 内核补 C 库的创可贴
  层层叠加，是移植的真实代价
- 讽刺的是：atomic 路径配的还是 VBE 这个不支持 page flip 的硬件，
  属于"法拉利发动机装在手扶拖拉机上"

**LuminaOS（对照组：0 行 DRM）**
- 直接 VGA 裸画窗口，272 行 desktop.c 解决战斗
- 没有 /dev/dri，没有合成器协议，但用户看到的效果和 GNOS 在 QEMU 里
  的软件渲染殊途同归
- 目录里倒是躺着 5.5G 的 linux-src 参考——拥有最强 DRM 却一行不用

**Linux 主线（42 万行）**
- GNOS 两套加起来 15k ≈ Linux 全树的 3.6%
- 但 Linux 那 40 万行里 90% 是真实 GPU 驱动；核心 8.4 万行对教学内核
  本来就是降维打击，没人真的从头写它

## 结论

- 论**诚实**：legacy/ 最诚实——QEMU 显卡就写 QEMU 驱动
- 论**野心**：ported/ 有完整 atomic API 形状，未来接真 GPU 驱动有接口可用
- 论**效率**：LuminaOS 用 0 行 DRM 达成了相同的视觉效果
- 一句话：GNOS 先造了个玩具枪（legacy），又买了把模型枪（ported），
  对面 LuminaOS 徒手画了个准星，而 Linux 是军火库本身。
