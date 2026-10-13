/**
 * @file    osc_ui.c
 * @brief   仪器界面实现
 * @details 配色沿用深色仪器风格：
 *            背景   #0A0F14   近黑
 *            网格   #1E2E3C   暗青灰
 *            波形   #00E5FF   青色霓虹
 *            触发电平 #FFB300 琥珀色
 *            正文   #D8E4EE / 次要文字 #5A6B7A
 *
 *          刷新策略：
 *            状态栏与测量栏仅在参数或数据变化时重绘；
 *            波形区每帧重绘（清背景 + 网格 + 波形），
 *            这是示波器类界面的常规做法——网格重绘量远小于
 *            逐像素追踪新旧波形差异所需的判断开销。
 */

#include "osc_ui.h"
#include "lcd.h"
#include "osc_adc.h"
#include "osc_trigger.h"
#include "osc_measure.h"
#include "osc_fft.h"
#include "osc_key.h"
#include <stdio.h>

/* ==========================================================================
 * 配色
 * ========================================================================== */

/** RGB888 转 LCD 使用的 16 位 RGB565 */
#define RGB565(r, g, b)  ((u16)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define C_BG        RGB565(0x0A, 0x0F, 0x14)
#define C_BAR       RGB565(0x0D, 0x14, 0x1B)
#define C_GRID      RGB565(0x1E, 0x2E, 0x3C)
#define C_GRID_MID  RGB565(0x2A, 0x3E, 0x50)
#define C_WAVE      RGB565(0x00, 0xE5, 0xFF)
#define C_WAVE_DIM  RGB565(0x00, 0x7A, 0x8A)
#define C_TRIG      RGB565(0xFF, 0xB3, 0x00)
#define C_TEXT      RGB565(0xD8, 0xE4, 0xEE)
#define C_DIM       RGB565(0x5A, 0x6B, 0x7A)

/* ==========================================================================
 * 版面参数
 * ========================================================================== */

#define SCR_W           320
#define SCR_H           240

#define BAR_H           28                      /**< 顶部状态栏高度 */
#define WAVE_Y0         BAR_H                   /**< 波形区上边界 */
#define WAVE_Y1         214                     /**< 波形区下边界 */
#define WAVE_H          (WAVE_Y1 - WAVE_Y0)     /**< 波形区高度 */
#define INFO_Y          WAVE_Y1                 /**< 测量栏起始行 */

/** 参数页定义 */
#define PAGE_TIME       0       /**< 时基 */
#define PAGE_CH         1       /**< 通道 */
#define PAGE_TRIG       2       /**< 触发电平 */
#define PAGE_MODE       3       /**< 采集模式与边沿 */
#define PAGE_COUNT      4

/* ==========================================================================
 * 模块状态
 * ========================================================================== */

static u8  s_display   = OSC_UI_WAVE;   /**< 波形 / 频谱 */
static u8  s_page      = PAGE_TIME;     /**< 当前参数页 */
static u8  s_need_full = 1;             /**< 是否需要重绘波形区与测量栏 */
static u8  s_need_bar  = 1;             /**< 是否需要重绘状态栏 */

/* ==========================================================================
 * 基础绘制辅助
 * ========================================================================== */

/**
 * @brief  显示一行 ASCII 字符串
 */
static void ui_text(u16 x, u16 y, const char *s)
{
    LCD_ShowString(x, y, 240, 16, 12, (u8 *)s);
}

/**
 * @brief  绘制状态栏背景
 */
static void draw_bar_bg(void)
{
    LCD_Fill(0, 0, SCR_W - 1, BAR_H - 2, C_BAR);
    LCD_Fill(0, BAR_H - 1, SCR_W - 1, BAR_H - 1, C_GRID);
    POINT_COLOR = C_TEXT;
    BACK_COLOR  = C_BAR;
}

/**
 * @brief  绘制波形区网格
 * @details 横向 10 格、纵向 8 格，中心线加亮作为零电平参考
 */
static void draw_grid(void)
{
    u8 i;

    LCD_Fill(0, WAVE_Y0, SCR_W - 1, WAVE_Y1 - 1, C_BG);

    POINT_COLOR = C_GRID;
    for (i = 1; i < 10; i++)
    {
        LCD_DrawLine((u16)(i * 32), WAVE_Y0, (u16)(i * 32), (u16)(WAVE_Y1 - 1));
    }
    for (i = 1; i < 8; i++)
    {
        u16 y = (u16)(WAVE_Y0 + i * (WAVE_H / 8));

        LCD_DrawLine(0, y, SCR_W - 1, y);
    }

    POINT_COLOR = C_GRID_MID;
    LCD_DrawLine(0, (u16)(WAVE_Y0 + WAVE_H / 2), SCR_W - 1, (u16)(WAVE_Y0 + WAVE_H / 2));

    POINT_COLOR = C_TEXT;
    BACK_COLOR  = C_BG;
}

/**
 * @brief  绘制触发电平指示线（虚线）
 */
static void draw_trigger_line(void)
{
    u16 level = osc_trigger_get_level();
    u16 y = (u16)(WAVE_Y0 + (WAVE_H - 1) - ((u32)level * (WAVE_H - 1)) / 4095);
    u16 x;

    POINT_COLOR = C_TRIG;
    for (x = 0; x + 3 < SCR_W; x += 6)
    {
        LCD_DrawLine(x, y, (u16)(x + 3), y);
    }
    POINT_COLOR = C_TEXT;
}

/**
 * @brief  绘制波形曲线
 * @details 采样值 0 ~ 4095 线性映射到波形区高度，
 *          相邻点之间连线以保证曲线连续
 */
static void draw_waveform(void)
{
    u16 i;
    u16 y0, y1, x;

    POINT_COLOR = C_WAVE;

    for (i = 1; i < OSC_VIEW_POINTS; i++)
    {
        /* 电压越高在屏幕上越靠上 */
        y0 = (u16)(WAVE_Y0 + (WAVE_H - 1) - ((u32)g_wave[i - 1] * (WAVE_H - 1)) / 4095);
        y1 = (u16)(WAVE_Y0 + (WAVE_H - 1) - ((u32)g_wave[i]     * (WAVE_H - 1)) / 4095);

        /* 320 个采样点铺满屏幕宽度 */
        x = (u16)((u32)(i - 1) * SCR_W / OSC_VIEW_POINTS);

        LCD_DrawLine(x, y0, (u16)(x + 1), y1);
    }

    POINT_COLOR = C_TEXT;
}

/**
 * @brief  绘制频谱柱状图
 */
static void draw_spectrum(void)
{
    const u8 *mag = osc_fft_get_magnitude();
    u8  col;
    u16 bar_w = (u16)(SCR_W / FFT_DISPLAY_COLS);

    LCD_Fill(0, WAVE_Y0, SCR_W - 1, WAVE_Y1 - 1, C_BG);

    for (col = 0; col < FFT_DISPLAY_COLS; col++)
    {
        /* 每个显示列覆盖若干个 FFT 频点，取其中最大值 */
        u16 bin0 = (u16)((u32)col * FFT_BINS / FFT_DISPLAY_COLS);
        u16 bin1 = (u16)((u32)(col + 1) * FFT_BINS / FFT_DISPLAY_COLS);
        u8  peak = 0;
        u16 b;

        for (b = bin0; b < bin1 && b < FFT_BINS; b++)
        {
            if (mag[b] > peak) peak = mag[b];
        }

        if (peak > 0)
        {
            u16 h = (u16)((u32)peak * (WAVE_H - 4) / 100);
            u16 x = (u16)(col * bar_w);

            LCD_Fill(x, (u16)(WAVE_Y1 - 1 - h), (u16)(x + bar_w - 2), (u16)(WAVE_Y1 - 2),
                     (peak > 60) ? C_WAVE : C_WAVE_DIM);
        }
    }

    POINT_COLOR = C_GRID_MID;
    LCD_DrawLine(0, (u16)(WAVE_Y1 - 1), SCR_W - 1, (u16)(WAVE_Y1 - 1));
    POINT_COLOR = C_TEXT;
    BACK_COLOR  = C_BG;
}

/* ==========================================================================
 * 状态栏与测量栏
 * ========================================================================== */

/**
 * @brief  绘制顶部状态栏
 */
static void draw_status_bar(void)
{
    static const char *page_name[PAGE_COUNT] = { "TIME", "CH", "TRIG", "MODE" };
    static const char *trig_name[OSC_TRIG_MODE_COUNT] = { "AUTO", "NORM", "SING" };

    char buf[48];
    const char *unit;
    u32 per_div;

    draw_bar_bg();

    ui_text(4, 7, (s_display == OSC_UI_WAVE) ? "WAVE" : "FFT");

    if (s_display == OSC_UI_WAVE)
    {
        switch (s_page)
        {
            case PAGE_TIME:
                per_div = osc_adc_time_per_div(&unit);
                sprintf(buf, "%s %lu%s/div", page_name[s_page], (unsigned long)per_div, unit);
                break;

            case PAGE_CH:
                sprintf(buf, "%s %s", page_name[s_page], osc_adc_channel_name());
                break;

            case PAGE_TRIG:
            {
                u16 mv = osc_adc_to_mv(osc_trigger_get_level());

                sprintf(buf, "%s %u.%02uV", page_name[s_page],
                        (unsigned)(mv / 1000), (unsigned)((mv % 1000) / 10));
                break;
            }

            default:
                sprintf(buf, "%s %s %s", page_name[s_page],
                        (osc_trigger_get_edge() == OSC_EDGE_RISING) ? "UP" : "DN",
                        trig_name[osc_trigger_get_mode()]);
                break;
        }
    }
    else
    {
        sprintf(buf, "Fs %lukHz", (unsigned long)(osc_adc_sample_rate() / 1000));
    }

    ui_text(48, 7, buf);

    /* 右侧运行指示：单次模式捕获完成后显示 HOLD */
    if (s_display == OSC_UI_WAVE)
    {
        if (osc_trigger_get_mode() == OSC_TRIG_SINGLE && !osc_trigger_is_armed())
        {
            POINT_COLOR = C_TRIG;
            ui_text(252, 7, "HOLD");
        }
        else
        {
            POINT_COLOR = g_wave_valid ? C_WAVE : C_DIM;
            ui_text(256, 7, "RUN");
        }
    }
    else
    {
        POINT_COLOR = C_DIM;
        ui_text(268, 7, "FFT");
    }

    POINT_COLOR = C_TEXT;
}

/**
 * @brief  绘制底部测量栏
 */
static void draw_info_bar(void)
{
    char buf[64];

    LCD_Fill(0, INFO_Y, SCR_W - 1, SCR_H - 1, C_BAR);
    LCD_Fill(0, INFO_Y, SCR_W - 1, INFO_Y, C_GRID);
    BACK_COLOR = C_BAR;

    if (s_display == OSC_UI_WAVE)
    {
        char freq[24];

        if (g_measure.freq_valid)
        {
            if (g_measure.freq_hz >= 1000)
            {
                sprintf(freq, "%lu.%02lukHz", (unsigned long)(g_measure.freq_hz / 1000),
                        (unsigned long)((g_measure.freq_hz % 1000) / 10));
            }
            else
            {
                sprintf(freq, "%luHz", (unsigned long)g_measure.freq_hz);
            }
        }
        else
        {
            sprintf(freq, "----");
        }

        sprintf(buf, "Vpp %u.%02uV  %s  Vrms %u.%02uV",
                (unsigned)(g_measure.v_pp_mv / 1000),
                (unsigned)((g_measure.v_pp_mv % 1000) / 10),
                freq,
                (unsigned)(g_measure.v_rms_mv / 1000),
                (unsigned)((g_measure.v_rms_mv % 1000) / 10));

        POINT_COLOR = C_TEXT;
        ui_text(4, (u16)(INFO_Y + 6), buf);
    }
    else
    {
        u32 dom = osc_fft_dominant_freq(osc_adc_sample_rate());

        if (dom >= 1000)
        {
            sprintf(buf, "PEAK %lu.%02lukHz", (unsigned long)(dom / 1000),
                    (unsigned long)((dom % 1000) / 10));
        }
        else
        {
            sprintf(buf, "PEAK %luHz", (unsigned long)dom);
        }

        POINT_COLOR = C_WAVE;
        ui_text(4, (u16)(INFO_Y + 6), buf);

        POINT_COLOR = C_DIM;
        ui_text(160, (u16)(INFO_Y + 6), "bias 1.65V");
        POINT_COLOR = C_TEXT;
    }
}

/* ==========================================================================
 * 对外接口
 * ========================================================================== */

void osc_ui_init(void)
{
    LCD_Init();
    LCD_Display_Dir(1);         /* 横屏，2.8 寸屏为 320 x 240 */
    LCD_Clear(C_BG);

    draw_grid();
    draw_trigger_line();

    s_need_full = 1;
    s_need_bar  = 1;
}

void osc_ui_handle_key(void)
{
    /* 一次性取出三个按键的事件：取出即清零，因此必须先保存再判断 */
    u8 ev0 = KEY_GetEvent(KEY_ID_0);
    u8 ev1 = KEY_GetEvent(KEY_ID_1);
    u8 evw = KEY_GetEvent(KEY_ID_WKUP);

    u8 act_up   = (ev1 & (KEY_EV_SHORT | KEY_EV_LONG | KEY_EV_REPEAT)) ? 1 : 0;
    u8 act_down = (ev0 & (KEY_EV_SHORT | KEY_EV_LONG | KEY_EV_REPEAT)) ? 1 : 0;
    u8 fast     = ((ev0 | ev1) & KEY_EV_REPEAT) ? 1 : 0;

    /* ---- WK_UP：短按切换参数页；长按切换波形/频谱；双击重新武装单次触发 ---- */
    if (evw & KEY_EV_DOUBLE)
    {
        osc_trigger_arm();
        s_need_bar = 1;
    }
    else if (evw & KEY_EV_LONG)
    {
        s_display   = (u8)((s_display + 1) % OSC_UI_MODE_COUNT);
        s_need_full = 1;
    }
    else if (evw & KEY_EV_SHORT)
    {
        s_page     = (u8)((s_page + 1) % PAGE_COUNT);
        s_need_bar = 1;
    }

    if (!act_up && !act_down) return;

    /* ---- KEY1 增大 / KEY0 减小，作用于当前参数页 ---- */
    switch (s_page)
    {
        case PAGE_TIME:
        {
            u8 n = osc_adc_timebase_count();
            u8 idx = osc_adc_get_timebase();

            if (act_up) idx = (idx == 0) ? 0 : (u8)(idx - 1);   /* 档位 0 为最快 */
            else        idx = (u8)((idx + 1) % n);

            osc_adc_set_timebase(idx);
            break;
        }

        case PAGE_CH:
        {
            u8 n = osc_adc_channel_count();
            u8 ch = osc_adc_get_channel();

            if (act_up) ch = (u8)((ch + n - 1) % n);
            else        ch = (u8)((ch + 1) % n);

            osc_adc_set_channel(ch);
            break;
        }

        case PAGE_TRIG:
        {
            u16 lvl  = osc_trigger_get_level();
            u16 step = fast ? 64 : 16;

            if (act_up)
            {
                lvl = ((u32)lvl + step > 4095) ? 4095 : (u16)(lvl + step);
            }
            else
            {
                lvl = (lvl < step) ? 0 : (u16)(lvl - step);
            }

            osc_trigger_set_level(lvl);
            break;
        }

        default:    /* PAGE_MODE：KEY1 切换采集模式，KEY0 切换触发边沿 */
            if (act_up)
            {
                osc_trigger_set_mode((u8)((osc_trigger_get_mode() + 1) % OSC_TRIG_MODE_COUNT));
            }
            else
            {
                osc_trigger_toggle_edge();
            }
            break;
    }

    s_need_bar  = 1;
    s_need_full = 1;
}

void osc_ui_update(void)
{
    /* ---- 采集一屏数据 ---- */
    if (osc_trigger_capture())
    {
        if (s_display == OSC_UI_WAVE)
        {
            osc_measure_run(g_wave, OSC_VIEW_POINTS, osc_adc_sample_rate());
        }
        else
        {
            osc_fft_run(g_wave, OSC_VIEW_POINTS);
        }

        s_need_full = 1;
        s_need_bar  = 1;
    }

    if (s_need_bar)
    {
        s_need_bar = 0;
        draw_status_bar();
    }

    if (!s_need_full) return;
    s_need_full = 0;

    if (s_display == OSC_UI_WAVE)
    {
        draw_grid();
        draw_trigger_line();
        draw_waveform();
    }
    else
    {
        draw_spectrum();
    }

    draw_info_bar();
}
