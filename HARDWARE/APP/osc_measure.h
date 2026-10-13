/**
 * @file    osc_measure.h
 * @brief   波形参数自动测量
 * @details 从一屏采样数据中提取常用参数：
 *            - Vmax / Vmin / Vpp ：电压极值与峰峰值
 *            - Vavg              ：平均值（直流分量）
 *            - Vrms              ：真有效值，按 sqrt(mean(v^2)) 计算
 *            - 频率              ：由跨越中点的边沿间隔推算
 *
 * @note    频率测量要求屏幕上至少包含两个完整的同向跨越点，
 *          否则判定为无法测量。
 */

#ifndef __OSC_MEASURE_H
#define __OSC_MEASURE_H

#include "sys.h"

/** ADC 参考电压 (mV) */
#define OSC_VREF_MV     3300

/** 测量结果 */
typedef struct
{
    u16 v_max_mv;       /**< 最大电压 (mV) */
    u16 v_min_mv;       /**< 最小电压 (mV) */
    u16 v_pp_mv;        /**< 峰峰值 (mV) */
    u16 v_avg_mv;       /**< 平均电压 (mV) */
    u16 v_rms_mv;       /**< 有效值电压 (mV) */
    u32 freq_hz;        /**< 信号频率 (Hz)，0 表示无法测量 */
    u8  freq_valid;     /**< 频率是否有效 */
} osc_measure_t;

/** 最近的测量结果 */
extern osc_measure_t g_measure;

/**
 * @brief  对给定的一屏数据执行测量
 * @param  wave  采样数据数组
 * @param  n     采样点数
 * @param  fs    采样率 (Hz)
 */
void osc_measure_run(const u16 *wave, u16 n, u32 fs);

/**
 * @brief  ADC 值转换为电压
 * @param  adc  12 位 ADC 值
 * @retval 电压值 (mV)
 */
u16 osc_adc_to_mv(u16 adc);

#endif
