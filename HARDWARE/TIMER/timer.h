/**
 * @file    timer.h
 * @brief   系统时基：TIM3 提供 1ms 计数
 * @details 用于按键消抖、长按判定与界面刷新节拍。
 *          本项目为裸机实现，不使用操作系统与图形库。
 */

#ifndef __TIMER_H
#define __TIMER_H

#include "sys.h"

/**
 * @brief  初始化 1ms 时基
 * @note   定时器时钟为 APB1 的 2 倍频（72MHz），
 *         TIM3_Int_Init(1, 7199) 对应 1ms 中断周期
 */
void TIM3_Int_Init(u16 arr, u16 psc);

/**
 * @brief  获取系统运行毫秒数
 * @retval 自上电以来经过的毫秒数
 * @note   32 位计数器约 49.7 天回绕，比较时请使用无符号差值
 */
u32 TIM_GetTick(void);

/**
 * @brief  时基中断服务函数，由 TIM3_IRQHandler 调用
 */
void TIM_TickHandler(void);

#endif
