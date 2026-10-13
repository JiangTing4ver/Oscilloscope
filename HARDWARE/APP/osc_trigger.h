/**
 * @file    osc_trigger.h
 * @brief   边沿触发与波形捕获
 * @details 触发机制说明：
 *
 *          DMA 以循环模式持续填充环形缓冲，数据永不停歇。上层不再
 *          "等待触发再采样"，而是反过来：在已采集的数据中**向后搜索**
 *          一个满足触发条件的边沿点，以该点为基准截取一屏数据。
 *
 *          这样做的好处：
 *            - 不会漏掉触发点（数据一直存在缓冲里）
 *            - 无需停止/重启 DMA，切换时基与通道时波形不中断
 *            - 实现简单可靠，且天然具备"预触发"能力（可显示触发点之前的数据）
 *
 *          触发模式：
 *            - OSC_TRIG_AUTO   ：找到边沿则对齐显示；找不到则自由滚动显示
 *                                （对应示波器的 Auto 档，无信号也能看到基线）
 *            - OSC_TRIG_NORMAL ：仅在找到边沿时更新画面，否则冻结上一屏
 *                                （对应 Normal 档，便于观察偶发信号）
 *            - OSC_TRIG_SINGLE ：捕获到一个边沿后停止更新，直到手动重新武装
 *                                （对应 Single 档，用于抓单次脉冲）
 */

#ifndef __OSC_TRIGGER_H
#define __OSC_TRIGGER_H

#include "sys.h"
#include "osc_adc.h"

/** 触发边沿类型 */
#define OSC_EDGE_RISING     0       /**< 上升沿 */
#define OSC_EDGE_FALLING    1       /**< 下降沿 */
#define OSC_EDGE_COUNT      2

/** 触发模式 */
#define OSC_TRIG_AUTO       0       /**< 自动：无触发时自由滚动 */
#define OSC_TRIG_NORMAL     1       /**< 正常：无触发时冻结画面 */
#define OSC_TRIG_SINGLE     2       /**< 单次：捕获一次后停止 */
#define OSC_TRIG_MODE_COUNT 3

/** 触发点之前保留的采样点数，用于显示预触发部分 */
#define OSC_PRETRIG_POINTS  32

/** 波形缓冲长度（一屏），由 OSC_VIEW_POINTS 决定 */
extern u16 g_wave[OSC_VIEW_POINTS];

/** 本屏数据是否来自有效触发 */
extern u8  g_wave_valid;

/**
 * @brief  初始化触发模块
 */
void osc_trigger_init(void);

/**
 * @brief  设置触发模式
 * @param  mode  OSC_TRIG_x
 */
void osc_trigger_set_mode(u8 mode);

/**
 * @brief  获取当前触发模式
 */
u8 osc_trigger_get_mode(void);

/**
 * @brief  切换触发边沿
 */
void osc_trigger_toggle_edge(void);

/**
 * @brief  获取当前触发边沿
 */
u8 osc_trigger_get_edge(void);

/**
 * @brief  设置触发电平
 * @param  level  12 位 ADC 值，范围 0 ~ 4095
 */
void osc_trigger_set_level(u16 level);

/**
 * @brief  获取当前触发电平
 */
u16 osc_trigger_get_level(void);

/**
 * @brief  重新武装单次触发
 * @note   仅在 OSC_TRIG_SINGLE 模式下有意义
 */
void osc_trigger_arm(void);

/**
 * @brief  获取单次触发的武装状态
 * @retval 1 表示已武装（等待触发中）
 */
u8 osc_trigger_is_armed(void);

/**
 * @brief  执行一次捕获
 * @retval 1 表示本次更新了波形数据，0 表示画面保持不变
 *
 * @details 建议在每个主循环周期调用。函数内部会读取环形缓冲的当前写指针，
 *          在其前方搜索触发边沿，并将一屏数据拷贝到 g_wave。
 *
 * @note    环形缓冲长度为 2 的整数次幂，因此取模运算退化为按位与，
 *          拷贝过程无需分支判断即可处理回绕。
 */
u8 osc_trigger_capture(void);

#endif
