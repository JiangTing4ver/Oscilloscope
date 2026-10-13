/**
 * @file    osc_fft.h
 * @brief   频谱分析（基 2 时间抽取 FFT）
 * @details 用于观察信号的频率成分，典型用途是音频频谱显示。
 *
 *          实现要点：
 *            - 点数固定为 256（2 的整数次幂），避免动态内存分配；
 *            - 旋转因子查表，表长仅为 N/4，减少 Flash 占用；
 *            - 输入加汉宁窗，抑制因截断造成的频谱泄漏；
 *            - 全部采用定点运算，不依赖 math 库与浮点单元。
 *
 * @warning 输入信号需叠加直流偏置（约 1.65V）后再接入，
 *          否则负半周会被 ADC 钳位在 0V，频谱将严重失真。
 */

#ifndef __OSC_FFT_H
#define __OSC_FFT_H

#include "sys.h"

/** FFT 点数 */
#define FFT_N           256

/** 可显示的频点数（只取前半频谱，即低于奈奎斯特频率的部分） */
#define FFT_BINS        (FFT_N / 2)

/** 频谱柱状图的显示列数 */
#define FFT_DISPLAY_COLS 64

/**
 * @brief  对一屏采样数据执行频谱分析
 * @param  wave  采样数据数组
 * @param  n     参与运算的点数，不足 FFT_N 时补零
 *
 * @details 结果以 0 ~ 100 的归一化幅度存放在 g_fft_mag 中，
 *          已按对数刻度压缩，便于直接绘制柱状图。
 */
void osc_fft_run(const u16 *wave, u16 n);

/**
 * @brief  获取指定频点对应的频率
 * @param  bin      频点索引 0 ~ FFT_BINS-1
 * @param  fs       采样率 (Hz)
 * @retval 频率 (Hz)
 */
u32 osc_fft_bin_freq(u16 bin, u32 fs);

/**
 * @brief  获取归一化频谱幅度数组
 * @retval 指向 FFT_BINS 个元素（取值 0 ~ 100）
 */
const u8 *osc_fft_get_magnitude(void);

/**
 * @brief  获取主频（幅度最大的频点对应的频率）
 * @param  fs  采样率 (Hz)
 * @retval 主频 (Hz)，无有效峰值时返回 0
 */
u32 osc_fft_dominant_freq(u32 fs);

#endif
