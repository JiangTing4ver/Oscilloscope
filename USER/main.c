/**
 * @file    main.c
 * @brief   迷你数字示波器主程序
 * @details 目标平台：正点原子 MiniSTM32 V3（STM32F103RCT6）+ 2.8 寸 TFT
 *
 *          采样链路：
 *              PA1~PA7 -> ADC1 -> DMA1_CH1 -> 环形缓冲（循环模式）
 *                          ^
 *                          | 外部触发
 *                      TIM2 TRGO
 *
 *          软件分层：
 *              HARDWARE/       LCD、按键、LED、定时器等底层驱动
 *              HARDWARE/APP/   osc_adc      采样链路
 *                              osc_trigger  边沿触发与波形捕获
 *                              osc_measure  参数自动测量
 *                              osc_fft      频谱分析
 *                              osc_ui       界面与交互
 *              main.c          初始化与主循环调度
 *
 *          调度方式：不依赖操作系统。主循环依次执行
 *          "按键扫描 -> 按键处理 -> 界面刷新"，其中界面刷新内部
 *          会完成一次波形捕获、测量与绘制。
 *
 *          相关文档：README.md、docs/USER_GUIDE.md、docs/DEVELOPER_GUIDE.md
 */

#include "delay.h"
#include "sys.h"
#include "usart.h"
#include "lcd.h"
#include "led.h"
#include "timer.h"
#include "osc_key.h"
#include "osc_adc.h"
#include "osc_trigger.h"
#include "osc_ui.h"

int main(void)
{
    /* ---------------- 1. 系统层初始化 ---------------- */
    delay_init();                                   /* SysTick 延时 */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2); /* 2 位抢占，2 位响应 */
    uart_init(9600);                                /* USART1，用于调试输出 */
    TIM3_Int_Init(1, 7199);                         /* 1ms 时基，供按键计时使用 */

    /* ---------------- 2. GPIO 初始化 ----------------
     * KEY_Init() 内部执行 GPIO_Remap_SWJ_JTAGDisable，将 PA15/PB3/PB4
     * 从 JTAG 功能释放为普通 GPIO。TFT 使用 PB0~PB15 作为 16 位数据总线，
     * 其中 PB3/PB4 默认为 JTDO/NJTRST，因此必须在本步先行释放，
     * 否则屏幕会出现花屏。 */
    LED_Init();
    KEY_Init();

    printf("\r\n===== Mini Oscilloscope boot =====\r\n");

    /* ---------------- 3. 采样链路初始化 ---------------- */
    osc_adc_init();
    osc_trigger_init();

    /* ---------------- 4. 界面初始化 ---------------- */
    osc_ui_init();

    printf("LCD ID=0x%04X  %dx%d\r\n", lcddev.id, lcddev.width, lcddev.height);
    printf("Sample rate = %lu Hz, channel = %s\r\n",
           (unsigned long)osc_adc_sample_rate(), osc_adc_channel_name());

    /* ---------------- 5. 主循环 ---------------- */
    while (1)
    {
        /* 按键扫描周期约 5ms，满足 20ms 消抖要求 */
        KEY_Scan();

        /* 处理按键事件，更新仪器参数 */
        osc_ui_handle_key();

        /* 捕获一屏数据并刷新界面 */
        osc_ui_update();

        delay_ms(5);
    }
}
