/**
 * @file    osc_adc.c
 * @brief   ADC 高速采样驱动实现
 * @details 时基档位设计依据：
 *
 *              ADC 时钟            = PCLK2 / 6 = 12MHz
 *              单次转换时间        = (采样时间 + 12.5) / ADC时钟
 *              ADC_SampleTime_1Cycles5 时约 1.17us
 *
 *          因此采样周期必须大于约 1.2us，最高采样率限制为 500ksps。
 *          时基表统一采用 TIM2 预分频 8（计数频率 9MHz），
 *          换算关系为：采样周期 = (ARR + 1) / 9MHz。
 *
 *          每个档位在屏幕上显示 320 个点、横向分为 10 格，
 *          因此"每格时间 = 32 / 采样率"。
 */

#include "osc_adc.h"
#include "stm32f10x_adc.h"
#include "stm32f10x_dma.h"
#include "stm32f10x_tim.h"

/* ==========================================================================
 * 采样通道配置（PA1 ~ PA7 对应 ADC1_IN1 ~ ADC1_IN7）
 * 注意：PA0 已被 WK_UP 按键占用，因此 ADC1_IN0 不可用
 * ========================================================================== */

#define OSC_CH_COUNT    7

static const u8  s_ch_adc[OSC_CH_COUNT] = { ADC_Channel_1, ADC_Channel_2, ADC_Channel_3,
                                            ADC_Channel_4, ADC_Channel_5, ADC_Channel_6,
                                            ADC_Channel_7 };
static const u16 s_ch_pin[OSC_CH_COUNT] = { GPIO_Pin_1, GPIO_Pin_2, GPIO_Pin_3,
                                            GPIO_Pin_4, GPIO_Pin_5, GPIO_Pin_6,
                                            GPIO_Pin_7 };
static const char *s_ch_name[OSC_CH_COUNT] = { "PA1", "PA2", "PA3", "PA4", "PA5", "PA6", "PA7" };

/* ==========================================================================
 * 时基档位表
 * ========================================================================== */

typedef struct
{
    u16 psc;            /**< 定时器预分频值 */
    u16 arr;            /**< 定时器自动重装载值 */
    u32 fs;             /**< 采样率 (Hz) */
} timebase_t;

static const timebase_t s_tb[] =
{
    {  8,    8,  1000000 },     /* 1.00 MHz  -> 32 us/div */
    {  8,   17,   500000 },     /* 500 kHz   -> 64 us/div */
    {  8,   44,   200000 },     /* 200 kHz   -> 160 us/div */
    {  8,   89,   100000 },     /* 100 kHz   -> 320 us/div */
    {  8,  179,    50000 },     /* 50 kHz    -> 640 us/div */
    {  8,  899,    10000 },     /* 10 kHz    -> 3.2 ms/div */
    { 89,  899,     1000 },     /* 1 kHz     -> 32 ms/div */
};

#define TB_COUNT    (sizeof(s_tb) / sizeof(s_tb[0]))

/** 屏幕横向显示 320 点，分为 10 格 */
#define POINTS_PER_DIV  32

/* ==========================================================================
 * 模块状态
 * ========================================================================== */

/** DMA 目标环形缓冲，需 2 的整数次幂长度 */
static u16 s_ring[OSC_RING_SIZE];

static u8 s_tb_idx  = 3;        /* 默认 100kHz，适合观察常见信号 */
static u8 s_ch_idx  = 0;        /* 默认 PA1 */

/* ==========================================================================
 * 初始化
 * ========================================================================== */

/**
 * @brief  配置采样所用的 GPIO 为模拟输入
 */
static void adc_gpio_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    u8 i;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    /* 全部通道统一配置为模拟输入：
     * - 模拟模式关闭引脚的数字输入通道，避免引入额外噪声
     * - 同时配置全部通道，切换通道时无需重新初始化 GPIO */
    for (i = 0; i < OSC_CH_COUNT; i++)
    {
        GPIO_InitStructure.GPIO_Pin  = s_ch_pin[i];
        GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
        GPIO_Init(GPIOA, &GPIO_InitStructure);
    }
}

/**
 * @brief  配置 DMA1 通道 1（ADC1 专用）
 */
static void adc_dma_init(void)
{
    DMA_InitTypeDef DMA_InitStructure;

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    DMA_DeInit(DMA1_Channel1);

    DMA_InitStructure.DMA_PeripheralBaseAddr = (u32)&ADC1->DR;      /* 源：ADC 数据寄存器 */
    DMA_InitStructure.DMA_MemoryBaseAddr     = (u32)s_ring;         /* 目标：环形缓冲 */
    DMA_InitStructure.DMA_DIR                = DMA_DIR_PeripheralSRC;
    DMA_InitStructure.DMA_BufferSize         = OSC_RING_SIZE;
    DMA_InitStructure.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    DMA_InitStructure.DMA_MemoryDataSize     = DMA_MemoryDataSize_HalfWord;
    DMA_InitStructure.DMA_Mode               = DMA_Mode_Circular;   /* 循环模式，永不停止 */
    DMA_InitStructure.DMA_Priority           = DMA_Priority_High;
    DMA_InitStructure.DMA_M2M                = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel1, &DMA_InitStructure);

    DMA_Cmd(DMA1_Channel1, ENABLE);
}

/**
 * @brief  配置 ADC1：由 TIM2 事件触发，转换完成后请求 DMA
 */
static void adc_config_init(void)
{
    ADC_InitTypeDef ADC_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);

    /* ADC 时钟 = PCLK2 / 6 = 12MHz（数据手册要求不超过 14MHz） */
    RCC_ADCCLKConfig(RCC_PCLK2_Div6);

    ADC_DeInit(ADC1);

    ADC_InitStructure.ADC_Mode               = ADC_Mode_Independent;
    ADC_InitStructure.ADC_ScanConvMode       = DISABLE;
    ADC_InitStructure.ADC_ContinuousConvMode = DISABLE;     /* 由外部事件触发单次转换 */
    ADC_InitStructure.ADC_ExternalTrigConv   = ADC_ExternalTrigConv_T2_CC2;
    ADC_InitStructure.ADC_DataAlign          = ADC_DataAlign_Right;
    ADC_InitStructure.ADC_NbrOfChannel       = 1;
    ADC_Init(ADC1, &ADC_InitStructure);

    /* 采样时间取最短，以获得最高采样率 */
    ADC_RegularChannelConfig(ADC1, s_ch_adc[s_ch_idx], 1, ADC_SampleTime_1Cycles5);

    /* 转换完成后产生 DMA 请求 */
    ADC_DMACmd(ADC1, ENABLE);

    /* 使能外部触发 */
    ADC_ExternalTrigConvCmd(ADC1, ENABLE);

    /* 上电并执行自校准 */
    ADC_Cmd(ADC1, ENABLE);
    ADC_ResetCalibration(ADC1);
    while (ADC_GetResetCalibrationStatus(ADC1));
    ADC_StartCalibration(ADC1);
    while (ADC_GetCalibrationStatus(ADC1));

    ADC_SoftwareStartConvCmd(ADC1, ENABLE);
}

/**
 * @brief  配置 TIM2 产生 ADC 触发信号
 * @param  idx  时基档位索引
 *
 * @details TIM2 使用"比较事件"作为触发源（ADC_ExternalTrigConv_T2_CC2）：
 *          计数到达 CCR2 时产生比较事件，将其映射到 TRGO 输出。
 *          CCR2 设为固定小值，使触发时刻在每个计数周期内保持稳定。
 */
static void tim2_config(u8 idx)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
    TIM_OCInitTypeDef       TIM_OCInitStructure;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);

    TIM_TimeBaseStructure.TIM_Period        = s_tb[idx].arr;
    TIM_TimeBaseStructure.TIM_Prescaler     = s_tb[idx].psc;
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_CounterMode   = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM2, &TIM_TimeBaseStructure);

    /* 比较通道 2 仅用于产生触发事件，不输出到引脚 */
    TIM_OCInitStructure.TIM_OCMode      = TIM_OCMode_PWM1;
    TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Disable;
    TIM_OCInitStructure.TIM_Pulse       = 1;
    TIM_OCInitStructure.TIM_OCPolarity  = TIM_OCPolarity_High;
    TIM_OC2Init(TIM2, &TIM_OCInitStructure);

    /* 将比较通道 2 的参考信号映射为 TRGO 输出 */
    TIM_SelectOutputTrigger(TIM2, TIM_TRGOSource_OC2Ref);

    TIM_Cmd(TIM2, ENABLE);
}

void osc_adc_init(void)
{
    adc_gpio_init();
    adc_config_init();
    adc_dma_init();
    tim2_config(s_tb_idx);
}

/* ==========================================================================
 * 运行时配置
 * ========================================================================== */

void osc_adc_set_timebase(u8 idx)
{
    if (idx >= TB_COUNT) idx = TB_COUNT - 1;

    s_tb_idx = idx;
    tim2_config(idx);
}

u8 osc_adc_get_timebase(void)
{
    return s_tb_idx;
}

u8 osc_adc_timebase_count(void)
{
    return (u8)TB_COUNT;
}

u32 osc_adc_sample_rate(void)
{
    return s_tb[s_tb_idx].fs;
}

u32 osc_adc_time_per_div(const char **unit)
{
    /* 每格时间 = 每格点数 / 采样率 = 32 / fs 秒 */
    u32 us_per_div = (u32)((32ULL * 1000000ULL) / s_tb[s_tb_idx].fs);

    if (us_per_div >= 1000000UL)
    {
        *unit = "s";
        return us_per_div / 1000000UL;
    }
    if (us_per_div >= 1000UL)
    {
        *unit = "ms";
        return us_per_div / 1000UL;
    }

    *unit = "us";
    return us_per_div;
}

void osc_adc_set_channel(u8 idx)
{
    if (idx >= OSC_CH_COUNT) idx = OSC_CH_COUNT - 1;
    if (idx == s_ch_idx) return;

    s_ch_idx = idx;

    /* 运行中切换通道：重新配置规则通道即可，DMA 无需改动。
     * 切换后的头几个采样值可能来自旧通道，属正常现象。 */
    ADC_RegularChannelConfig(ADC1, s_ch_adc[s_ch_idx], 1, ADC_SampleTime_1Cycles5);
}

u8 osc_adc_get_channel(void)
{
    return s_ch_idx;
}

u8 osc_adc_channel_count(void)
{
    return OSC_CH_COUNT;
}

const char *osc_adc_channel_name(void)
{
    return s_ch_name[s_ch_idx];
}

/* ==========================================================================
 * 数据访问
 * ========================================================================== */

const u16 *osc_adc_get_ring(void)
{
    return s_ring;
}

u32 osc_adc_get_write_pos(void)
{
    /* DMA 剩余传输数量递减，据此反推已写入的位置
     * 通道 1 的剩余计数寄存器为 DMA1_CNDTR1 */
    u32 remain = DMA_GetCurrDataCounter(DMA1_Channel1);

    return (OSC_RING_SIZE - remain) & (OSC_RING_SIZE - 1);
}
