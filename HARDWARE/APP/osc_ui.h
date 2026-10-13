/**
 * @file    osc_ui.h
 * @brief   仪器界面（裸机直绘，不依赖图形库）
 * @details 320 x 240 版面划分：
 *
 *              y=0    ┌──────────────────────────────────────────┐
 *                     │ WAVE  100kHz  PA1  TRIG:↑  AUTO     [RUN] │ 状态栏 28px
 *              y=28   ├──────────────────────────────────────────┤
 *                     │        ·    ·    ·    ·    ·             │
 *                     │   ─────╱╲──────╱╲──────╱╲───────         │ 波形区 186px
 *                     │        ·    ·    ·    ·    ·             │
 *              y=214  ├──────────────────────────────────────────┤
 *                     │ Vpp 3.28V  1.00kHz  Vrms 1.15V           │ 测量栏 26px
 *              y=240  └──────────────────────────────────────────┘
 *
 *          界面分为四个可切换的参数页：
 *            MODE_TIME   调节时基（KEY0/KEY1 切换档位）
 *            MODE_CH     选择输入通道 PA1 ~ PA7
 *            MODE_TRIG   调节触发电平（长按连发快速调节）
 *            MODE_MODE   切换采集模式（自动 / 正常 / 单次）与边沿
 *
 * @note    全部绘制直接写入 LCD 显存，不引入 LVGL，
 *          既节省 Flash 空间，也便于精确控制局部刷新。
 */

#ifndef __OSC_UI_H
#define __OSC_UI_H

#include "sys.h"

/** 界面显示模式 */
#define OSC_UI_WAVE         0       /**< 波形显示 */
#define OSC_UI_SPECTRUM     1       /**< 频谱显示 */
#define OSC_UI_MODE_COUNT   2

/**
 * @brief  初始化界面
 * @note   内部完成 LCD 初始化、清屏与静态元素绘制
 */
void osc_ui_init(void);

/**
 * @brief  刷新动态内容
 * @note   建议每个主循环周期调用一次
 */
void osc_ui_update(void);

/**
 * @brief  按键事件处理
 * @note   由主循环从按键驱动取出事件后转发至此，
 *         界面模块负责解释事件语义并更新仪器参数
 */
void osc_ui_handle_key(void);

#endif
