/**
 * @file    osc_trigger.c
 * @brief   边沿触发与波形捕获实现
 */

#include "osc_trigger.h"

/** 一屏波形数据 */
u16 g_wave[OSC_VIEW_POINTS];

/** 本屏数据是否来自有效触发 */
u8 g_wave_valid = 0;

/** 触发模块状态 */
static u8  s_mode  = OSC_TRIG_AUTO;
static u8  s_edge  = OSC_EDGE_RISING;
static u16 s_level = 2048;          /**< 默认触发电平取量程中点 */
static u8  s_armed = 1;             /**< 单次触发武装标志 */

void osc_trigger_init(void)
{
    u16 i;

    for (i = 0; i < OSC_VIEW_POINTS; i++) g_wave[i] = 0;

    s_mode      = OSC_TRIG_AUTO;
    s_edge      = OSC_EDGE_RISING;
    s_level     = 2048;
    s_armed     = 1;
    g_wave_valid = 0;
}

void osc_trigger_set_mode(u8 mode)
{
    if (mode >= OSC_TRIG_MODE_COUNT) return;

    s_mode  = mode;
    s_armed = 1;        /* 切换模式后重新武装，便于单次触发使用 */
}

u8 osc_trigger_get_mode(void)
{
    return s_mode;
}

void osc_trigger_toggle_edge(void)
{
    s_edge = (s_edge == OSC_EDGE_RISING) ? OSC_EDGE_FALLING : OSC_EDGE_RISING;
}

u8 osc_trigger_get_edge(void)
{
    return s_edge;
}

void osc_trigger_set_level(u16 level)
{
    if (level > 4095) level = 4095;
    s_level = level;
}

u16 osc_trigger_get_level(void)
{
    return s_level;
}

void osc_trigger_arm(void)
{
    s_armed = 1;
}

u8 osc_trigger_is_armed(void)
{
    return s_armed;
}

/**
 * @brief  在环形缓冲中向后搜索触发点
 * @param  anchor  搜索起点（通常是当前写指针）
 * @retval 触发点在环形缓冲中的索引；未找到返回 0xFFFFFFFF
 *
 * @details 从 anchor 向"过去"的方向逐点比较，寻找跨越触发电平的边沿。
 *          由于环长 2048 是 2 的幂，索引递增使用按位与即可。
 */
static u32 trigger_search(u32 anchor)
{
    const u16 *ring = osc_adc_get_ring();
    u32 i;
    u32 idx_cur, idx_prev;

    /* 最多回溯半个缓冲区：再多就说明信号周期远大于当前时基，
     * 此时对齐与否对观察没有帮助，不如让画面自由滚动 */
    for (i = 0; i < (OSC_RING_SIZE / 2); i++)
    {
        idx_cur  = (anchor + OSC_RING_SIZE - i) & (OSC_RING_SIZE - 1);
        idx_prev = (idx_cur + OSC_RING_SIZE - 1) & (OSC_RING_SIZE - 1);

        if (s_edge == OSC_EDGE_RISING)
        {
            /* 上一点低于电平、当前点不低于电平 => 上升沿 */
            if (ring[idx_prev] < s_level && ring[idx_cur] >= s_level)
            {
                return idx_cur;
            }
        }
        else
        {
            /* 上一点高于电平、当前点不高于电平 => 下降沿 */
            if (ring[idx_prev] > s_level && ring[idx_cur] <= s_level)
            {
                return idx_cur;
            }
        }
    }

    return 0xFFFFFFFFUL;
}

/**
 * @brief  从环形缓冲拷贝一屏数据到 g_wave
 * @param  start  起始索引
 *
 * @details 起点向前偏移 OSC_PRETRIG_POINTS，使触发点落在屏幕靠左位置，
 *          从而能够观察到触发之前的一段波形（预触发）。
 */
static void wave_copy(u32 start)
{
    const u16 *ring = osc_adc_get_ring();
    u32 base = (start + OSC_RING_SIZE - OSC_PRETRIG_POINTS) & (OSC_RING_SIZE - 1);
    u16 i;

    for (i = 0; i < OSC_VIEW_POINTS; i++)
    {
        g_wave[i] = ring[(base + i) & (OSC_RING_SIZE - 1)];
    }
}

u8 osc_trigger_capture(void)
{
    u32 anchor = osc_adc_get_write_pos();
    u32 trig;

    /* 单次模式：已捕获过则保持画面不变 */
    if (s_mode == OSC_TRIG_SINGLE && !s_armed) return 0;

    trig = trigger_search(anchor);

    if (trig != 0xFFFFFFFFUL)
    {
        /* 找到触发点，以它为基准对齐显示 */
        wave_copy(trig);
        g_wave_valid = 1;

        if (s_mode == OSC_TRIG_SINGLE) s_armed = 0;

        return 1;
    }

    /* 未找到触发点 */
    if (s_mode == OSC_TRIG_AUTO)
    {
        /* 自动模式：自由滚动，从写指针前方取一屏，保证画面持续更新 */
        wave_copy(anchor);
        g_wave_valid = 0;
        return 1;
    }

    /* 正常模式与单次模式：保持上一屏画面 */
    return 0;
}
