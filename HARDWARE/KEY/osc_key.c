/**
 * @file    osc_key.c
 * @brief   按键事件驱动实现
 * @details 状态机设计：每个按键维护"原始电平 -> 消抖 -> 事件生成"三级处理。
 *
 *              原始电平
 *                 │ 连续 KEY_DEBOUNCE_MS 保持稳定
 *                 v
 *              稳定电平 ──按下──> 记录按下时刻，启动长按计时
 *                 │
 *                 ├── 按住超过 KEY_LONG_MS ──> 产生 LONG，随后每 KEY_REPEAT_MS 产生 REPEAT
 *                 │
 *                 └── 在 KEY_LONG_MS 内抬起 ──> 产生 SHORT
 *                                                  │ 若距上次 SHORT 在 KEY_DOUBLE_MS 之内
 *                                                  └──> 追加产生 DOUBLE
 *
 * @note    事件存放在每个按键的待处理变量中，由上层读取后清零，
 *          因此上层即使扫描周期较慢也不会漏掉事件。
 */

#include "osc_key.h"
#include "timer.h"

/** 按键硬件描述 */
typedef struct
{
    GPIO_TypeDef *port;     /**< GPIO 端口 */
    u16           pin;      /**< 引脚 */
    u8            active;   /**< 有效电平：0 表示低电平按下 */
} key_hw_t;

/** 按键硬件表，顺序与 KEY_ID_x 对应 */
static const key_hw_t s_hw[KEY_ID_MAX] =
{
    { GPIOC, GPIO_Pin_5,  0 },      /* KEY0  : 低电平有效 */
    { GPIOA, GPIO_Pin_15, 0 },      /* KEY1  : 低电平有效 */
    { GPIOA, GPIO_Pin_0,  1 },      /* WK_UP : 高电平有效 */
};

/** 按键运行时状态 */
typedef struct
{
    u8  stable;         /**< 消抖后的稳定电平（1 = 按下） */
    u8  last_raw;       /**< 上次原始电平 */
    u32 raw_change_ms;  /**< 原始电平最近一次变化的时刻 */
    u32 press_ms;       /**< 本次按下的时刻 */
    u32 repeat_ms;      /**< 上次连发的时刻 */
    u8  long_fired;     /**< 本次按住是否已产生过长按事件 */
    u32 last_short_ms;  /**< 上次短按时刻，用于双击判定；0 表示无待配对短按 */
    u8  pending;        /**< 待处理事件位掩码 */
} key_state_t;

static key_state_t s_key[KEY_ID_MAX];

/* 前置声明：KEY_Init 需要读取初始电平，而函数定义位于其之后 */
static u8 key_read_raw(u8 id);

void KEY_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    u8 i;

    /* 使能 GPIOA / GPIOC 时钟与复用功能时钟（引脚重映射需要 AFIO） */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC |
                           RCC_APB2Periph_AFIO, ENABLE);

    /* 关闭 JTAG、保留 SWD，将 PA15 / PB3 / PB4 释放为普通 GPIO。
     * 本项目 LCD 使用 PB0~PB15 作为 16 位数据总线，
     * 其中 PB3/PB4 默认功能为 JTDO / NJTRST，不释放会导致显示花屏。 */
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);

    /* KEY1 (PA15)：上拉输入 */
    GPIO_InitStructure.GPIO_Pin  = GPIO_Pin_15;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* KEY0 (PC5)：上拉输入 */
    GPIO_InitStructure.GPIO_Pin  = GPIO_Pin_5;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    /* WK_UP (PA0)：下拉输入，高电平有效 */
    GPIO_InitStructure.GPIO_Pin  = GPIO_Pin_0;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPD;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* 初始化状态，避免上电瞬间产生误事件 */
    for (i = 0; i < KEY_ID_MAX; i++)
    {
        s_key[i].stable        = 0;
        s_key[i].last_raw      = key_read_raw(i);   /* 以当前实际电平为基准 */
        s_key[i].raw_change_ms = TIM_GetTick();
        s_key[i].press_ms      = 0;
        s_key[i].repeat_ms     = 0;
        s_key[i].long_fired    = 0;
        s_key[i].last_short_ms = 0;
        s_key[i].pending       = KEY_EV_NONE;
    }
}

/**
 * @brief  读取按键原始电平
 * @param  id  按键编号
 * @retval 1 表示按下
 */
static u8 key_read_raw(u8 id)
{
    u8 level = GPIO_ReadInputDataBit(s_hw[id].port, s_hw[id].pin);

    return (level == s_hw[id].active) ? 1 : 0;
}

void KEY_Scan(void)
{
    u32 now = TIM_GetTick();
    u8  i;

    for (i = 0; i < KEY_ID_MAX; i++)
    {
        u8 raw = key_read_raw(i);

        /* ---- 第一级：消抖 ----
         * 原始电平只要发生变化就重新计时；只有连续稳定超过消抖阈值，
         * 才认为电平真正改变 */
        if (raw != s_key[i].last_raw)
        {
            s_key[i].last_raw      = raw;
            s_key[i].raw_change_ms = now;
            continue;
        }

        if ((now - s_key[i].raw_change_ms) < KEY_DEBOUNCE_MS) continue;

        if (raw != s_key[i].stable)
        {
            /* ---- 第二级：稳定电平发生变化 ---- */
            s_key[i].stable = raw;

            if (raw)
            {
                /* 按下：记录时刻，准备长按计时 */
                s_key[i].press_ms   = now;
                s_key[i].repeat_ms  = now;
                s_key[i].long_fired = 0;
            }
            else if (!s_key[i].long_fired)
            {
                /* 在长按阈值内抬起：判定为短按 */
                s_key[i].pending |= KEY_EV_SHORT;

                /* 双击判定：与上一次短按的间隔落在窗口内 */
                if (s_key[i].last_short_ms != 0 &&
                    (now - s_key[i].last_short_ms) <= KEY_DOUBLE_MS)
                {
                    s_key[i].pending      |= KEY_EV_DOUBLE;
                    s_key[i].last_short_ms = 0;     /* 消费掉，避免三连击重复判定 */
                }
                else
                {
                    s_key[i].last_short_ms = now;
                }
            }
        }

        /* ---- 第三级：长按与连发（仅在按住期间产生） ---- */
        if (!s_key[i].stable) continue;
        if ((now - s_key[i].press_ms) < KEY_LONG_MS) continue;

        if (!s_key[i].long_fired)
        {
            s_key[i].long_fired = 1;
            s_key[i].repeat_ms  = now;
            s_key[i].pending   |= KEY_EV_LONG;
        }
        else if ((now - s_key[i].repeat_ms) >= KEY_REPEAT_MS)
        {
            s_key[i].repeat_ms = now;
            s_key[i].pending  |= KEY_EV_REPEAT;
        }
    }
}

u8 KEY_GetEvent(u8 id)
{
    u8 ev;

    if (id >= KEY_ID_MAX) return KEY_EV_NONE;

    ev = s_key[id].pending;
    s_key[id].pending = KEY_EV_NONE;

    return ev;
}

u8 KEY_IsDown(u8 id)
{
    if (id >= KEY_ID_MAX) return 0;
    return s_key[id].stable;
}
