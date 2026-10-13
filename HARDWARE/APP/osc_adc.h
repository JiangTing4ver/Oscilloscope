/**
 * @file    osc_adc.h
 * @brief   ADC 高速采样驱动（TIM2 触发 + DMA 搬运）
 * @details 采样链路：
 *
 *              PA1~PA7 ──> ADC1 ──转换完成──> DMA 请求 ──> 环形缓冲
 *                            ^
 *                            │ 外部触发
 *                        TIM2 TRGO
 *
 *          工作方式说明：
 *            - TIM2 以固定频率产生 TRGO 信号，每个信号触发一次 ADC 转换，
 *              因此"触发频率 = 采样率"，由定时器分频精确决定；
 *            - ADC 转换完成后自动发出 DMA 请求，将结果搬运到环形缓冲，
 *              整个过程 CPU 不参与，得以同时完成波形渲染；
 *            - DMA 配置为循环模式，缓冲永不停止刷新；
 *            - 上层通过 osc_adc_get_ring() 读取"当前写指针"，据此判断
 *              哪些数据是本次新采集的。
 *
 * @note    采样率上限受 ADC 转换时间限制。ADC 时钟 12MHz、
 *          采样时间 1.5 周期时单次转换约 1.17us，
 *          因此最高采样率限制在 500ksps，见 osc_adc.c 的时基表。
 */

#ifndef __OSC_ADC_H
#define __OSC_ADC_H

#include "sys.h"

/** 环形缓冲采样点数，必须为 2 的整数次幂（便于取模寻址） */
#define OSC_RING_SIZE       2048

/** 屏幕可显示的采样点数（横向 320 像素，每像素最多对应 1 个点） */
#define OSC_VIEW_POINTS     320

/**
 * @brief  初始化采样链路
 * @note   内部完成 ADC1、DMA1 与 TIM2 的配置，并使能连续采集
 */
void osc_adc_init(void);

/**
 * @brief  设置采样率档位
 * @param  idx  时基表索引，超出范围时自动截断
 * @note   切换档位会重新配置 TIM2 分频，DMA 传输过程不中断
 */
void osc_adc_set_timebase(u8 idx);

/**
 * @brief  获取当前时基档位索引
 */
u8 osc_adc_get_timebase(void);

/**
 * @brief  获取时基档位总数
 */
u8 osc_adc_timebase_count(void);

/**
 * @brief  获取当前采样率
 * @retval 采样率 (Hz)
 */
u32 osc_adc_sample_rate(void);

/**
 * @brief  获取每格（横向 1/10 屏宽）对应的时间
 * @retval 时间值，单位由 unit 输出
 * @param  unit  输出参数，接收单位字符串（"us" / "ms" / "s"）
 * @retval 数值部分
 */
u32 osc_adc_time_per_div(const char **unit);

/**
 * @brief  选择模拟输入通道
 * @param  idx  通道索引 0 ~ 6，对应 PA1 ~ PA7
 */
void osc_adc_set_channel(u8 idx);

/**
 * @brief  获取当前通道索引
 */
u8 osc_adc_get_channel(void);

/**
 * @brief  获取通道总数
 */
u8 osc_adc_channel_count(void);

/**
 * @brief  获取当前通道对应的引脚名称，如 "PA1"
 */
const char *osc_adc_channel_name(void);

/**
 * @brief  获取环形缓冲首地址
 * @retval 指向 OSC_RING_SIZE 个采样值的数组
 */
const u16 *osc_adc_get_ring(void);

/**
 * @brief  获取环形缓冲当前的写入位置
 * @retval 0 ~ OSC_RING_SIZE-1
 * @note   由于 DMA 在后台持续写入，该值随时可能变化
 */
u32 osc_adc_get_write_pos(void);

#endif
