/**
 * @file    osc_measure.c
 * @brief   波形参数自动测量实现
 */

#include "osc_measure.h"

osc_measure_t g_measure;

u16 osc_adc_to_mv(u16 adc)
{
    return (u16)(((u32)adc * OSC_VREF_MV) / 4095);
}

/**
 * @brief  基于中点跨越间隔估算频率
 * @param  wave  采样数据
 * @param  n     点数
 * @param  fs    采样率 (Hz)
 * @retval 频率 (Hz)，0 表示无法测量
 *
 * @details 以 (Vmax + Vmin) / 2 作为判决电平，统计同向跨越点的平均间隔，
 *          该间隔即为信号周期对应的采样点数。
 *
 *          为避免噪声在判决电平附近造成多次误跨越，要求跨越时具有
 *          明确的电平跳变（前一点在电平以下、当前点在电平以上）。
 */
static u32 measure_freq(const u16 *wave, u16 n, u32 fs)
{
    u16 vmax = wave[0];
    u16 vmin = wave[0];
    u16 mid;
    u32 first = 0xFFFFFFFFUL;
    u32 last  = 0xFFFFFFFFUL;
    u16 count = 0;
    u16 i;

    for (i = 0; i < n; i++)
    {
        if (wave[i] > vmax) vmax = wave[i];
        if (wave[i] < vmin) vmin = wave[i];
    }

    if (vmax <= vmin) return 0;

    /* 幅度门限：信号必须明显超过噪声，否则不报告频率。
     *
     * 缺少这道判断时，接 GND 或接 3.3V 这类直流输入会给出虚假读数：
     * 中点取 (Vmax+Vmin)/2，即使只有 1 LSB 的噪声也必然存在跨越中点
     * 的抖动，计数器会把这些抖动当成周期，算出毫无意义的"频率"。
     *
     * 门限取 24 LSB，约合 19 mV（3.3V / 4095 x 24），
     * 高于实测噪声（约 1 LSB），又远低于任何有意义的信号幅度。 */
    if ((u16)(vmax - vmin) < OSC_FREQ_MIN_PP) return 0;

    mid = (u16)((vmax + vmin) / 2);

    for (i = 1; i < n; i++)
    {
        /* 上升跨越中点 */
        if (wave[i - 1] < mid && wave[i] >= mid)
        {
            if (first == 0xFFFFFFFFUL) first = i;
            last = i;
            count++;
        }
    }

    /* 至少需要两次跨越才能确定一个周期 */
    if (count < 2 || last <= first) return 0;

    /* 周期点数 = 首末跨越间隔 / 间隔个数 */
    {
        u32 period_pts = (last - first) / (u32)(count - 1);

        if (period_pts == 0) return 0;

        return fs / period_pts;
    }
}

void osc_measure_run(const u16 *wave, u16 n, u32 fs)
{
    u32 sum = 0;
    u32 sum_sq = 0;
    u16 vmax = wave[0];
    u16 vmin = wave[0];
    u16 i;

    if (n == 0) return;

    for (i = 0; i < n; i++)
    {
        u32 v = wave[i];

        if (v > vmax) vmax = v;
        if (v < vmin) vmin = v;

        sum    += v;
        sum_sq += v * v;
    }

    g_measure.v_max_mv = osc_adc_to_mv(vmax);
    g_measure.v_min_mv = osc_adc_to_mv(vmin);
    g_measure.v_pp_mv  = (u16)(g_measure.v_max_mv - g_measure.v_min_mv);
    g_measure.v_avg_mv = osc_adc_to_mv((u16)(sum / n));

    /* 有效值：先对 ADC 值求均方根，再换算为电压。
     * 直接对电压值开方可减少一次除法带来的精度损失。 */
    {
        u32 mean_sq = sum_sq / n;
        u32 rms_adc = 0;

        /* 整数平方根（逐位逼近，避免引入浮点运算与 math 库） */
        {
            u32 bit = 1UL << 30;
            u32 res = 0;
            u32 num = mean_sq;

            while (bit > num) bit >>= 2;

            while (bit != 0)
            {
                if (num >= res + bit)
                {
                    num -= res + bit;
                    res = (res >> 1) + bit;
                }
                else
                {
                    res >>= 1;
                }
                bit >>= 2;
            }
            rms_adc = res;
        }

        g_measure.v_rms_mv = osc_adc_to_mv((u16)rms_adc);
    }

    /* 频率测量 */
    {
        u32 f = measure_freq(wave, n, fs);

        g_measure.freq_hz    = f;
        g_measure.freq_valid = (f != 0) ? 1 : 0;
    }
}
