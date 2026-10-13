/**
 * @file    osc_key.h
 * @brief   按键事件驱动
 * @details 仅用 3 个物理按键实现仪器操作，因此按键层需提供比"按下/抬起"
 *          更丰富的事件语义。本驱动产出四类事件：
 *
 *            - 短按 (SHORT) ：单击，用于切换档位 / 移动光标
 *            - 长按 (LONG)  ：按住超过阈值，用于进入菜单 / 复位
 *            - 连发 (REPEAT)：长按后周期性重复，用于连续调节参数
 *            - 双击 (DOUBLE)：快速连按两次，用于返回上级 / 切换模式
 *
 *          按键分配：
 *            KEY0   -> PC5     下移 / 减小
 *            KEY1   -> PA15    上移 / 增大
 *            WK_UP  -> PA0     确认 / 切换模式
 *
 * @note    KEY_Init() 内部执行 JTAG 释放操作（GPIO_Remap_SWJ_JTAGDisable），
 *          必须在 LCD_Init() 之前调用，否则 LCD 数据线 PB3/PB4 不可用。
 */

#ifndef __OSC_KEY_H
#define __OSC_KEY_H

#include "sys.h"

/** 按键编号 */
#define KEY_ID_0        0       /**< KEY0  (PC5)  */
#define KEY_ID_1        1       /**< KEY1  (PA15) */
#define KEY_ID_WKUP     2       /**< WK_UP (PA0)  */
#define KEY_ID_MAX      3

/** 事件类型（可组合，scan 返回值为各键事件的按位或） */
#define KEY_EV_NONE      0x00
#define KEY_EV_SHORT     0x01   /**< 短按 */
#define KEY_EV_LONG      0x02   /**< 长按（一次） */
#define KEY_EV_REPEAT    0x04   /**< 长按连发 */
#define KEY_EV_DOUBLE    0x08   /**< 双击 */

/** 时序参数 (ms) */
#define KEY_DEBOUNCE_MS     20  /**< 消抖时间 */
#define KEY_LONG_MS        600  /**< 长按判定阈值 */
#define KEY_REPEAT_MS      150  /**< 长按后连发间隔 */
#define KEY_DOUBLE_MS      300  /**< 双击判定窗口 */

/**
 * @brief  按键 GPIO 初始化
 * @note   同时释放 PB3/PB4 的 JTAG 功能，必须在 LCD 初始化之前调用
 */
void KEY_Init(void);

/**
 * @brief  按键扫描任务
 * @note   建议调用周期 5 ~ 10ms
 */
void KEY_Scan(void);

/**
 * @brief  查询指定按键的待处理事件
 * @param  id  按键编号（KEY_ID_x）
 * @retval 事件位掩码（KEY_EV_x），无事件返回 KEY_EV_NONE
 * @note   读取后事件被清除，同一事件只会被读取一次
 */
u8 KEY_GetEvent(u8 id);

/**
 * @brief  查询指定按键当前是否处于按下状态
 * @param  id  按键编号
 * @retval 1 表示按下
 */
u8 KEY_IsDown(u8 id);

#endif
