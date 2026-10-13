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
#include "timer.h"
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

/** 目标帧周期 (ms)。LCD 为并口逐像素写入，一帧绘制需十余毫秒，
 *  若不加限制会连续重绘导致画面撕裂闪烁 */
#define FRAME_MS        50

/* ==========================================================================
 * 模块状态
 * ========================================================================== */

static u8  s_display   = OSC_UI_WAVE;   /**< 波形 / 频谱 */
static u8  s_page      = PAGE_TIME;     /**< 当前参数页 */
static u8  s_need_full = 1;             /**< 是否需要重绘波形区与测量栏 */
static u8  s_need_bar  = 1;             /**< 是否需要重绘状态栏 */
static u8  s_erase_all = 1;             /**< 需要整屏重建（切模式/切通道等） */
static u32 s_last_draw = 0;             /**< 上次绘制时刻，用于帧率限制 */

/** 已完成的整帧绘制次数，可通过调试器读取以评估实际帧率 */
volatile u32 g_frame_count = 0;

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
 * @brief  判断波形区某个像素属于哪种背景元素
 * @param  x  横坐标（屏幕绝对坐标）
 * @param  y  纵坐标（屏幕绝对坐标）
 * @retval 该像素应有的背景色
 *
 * @details 网格与触发电平线的位置都是确定的，因此可以随时按坐标
 *          重新算出某点应该是什么颜色，无需缓存整幅背景。
 */
static u16 bg_pixel_color(u16 x, u16 y)
{
    u16 trig_y;

    /* 纵向网格线：每 32 像素一条 */
    if (x != 0 && (x % 32) == 0) return C_GRID;

    /* 横向网格线：每 WAVE_H/8 像素一条 */
    if (y > WAVE_Y0 && ((y - WAVE_Y0) % (WAVE_H / 8)) == 0) return C_GRID;

    /* 纵向中心线加亮 */
    if (y == WAVE_Y0 + WAVE_H / 2) return C_GRID_MID;

    /* 触发电平虚线：每 6 像素一段、每段 4 像素 */
    trig_y = (u16)(WAVE_Y0 + (WAVE_H - 1) -
                   ((u32)osc_trigger_get_level() * (WAVE_H - 1)) / 4095);
    if (y == trig_y && (x % 6) < 4) return C_TRIG;

    return C_BG;
}

/**
 * @brief  绘制波形区背景（整幅）
 * @details 仅在初始化、切换显示模式等需要全屏重建时调用
 */
static void draw_grid(void)
{
    u16 x, y;

    for (y = WAVE_Y0; y < WAVE_Y1; y++)
    {
        for (x = 0; x < SCR_W; x++)
        {
            LCD_Fast_DrawPoint(x, y, bg_pixel_color(x, y));
        }
    }

    POINT_COLOR = C_TEXT;
    BACK_COLOR  = C_BG;
}


/**
 * @brief  绘制触发电平指示线（虚线）
 * @note   触发电平线的颜色已由 bg_pixel_color 统一决定，
 *         因此改变触发电平时只需整幅重建波形区即可，
 *         本函数保留给全屏重建流程调用。
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
 * @brief  把该列对应的采样值转换为屏幕纵坐标
 * @param  x  屏幕横坐标
 * @retval 波形纵坐标
 */
static u16 wave_y_at(u16 x)
{
    u16 i = (u16)((u32)x * (OSC_VIEW_POINTS - 1) / (SCR_W - 1));

    /* 电压越高，屏幕位置越靠上 */
    return (u16)(WAVE_Y0 + (WAVE_H - 1) -
                 ((u32)g_wave[i] * (WAVE_H - 1)) / 4095);
}

/**
 * @brief  绘制波形曲线并擦除上一帧的残留
 *
 * @details 采用**单趟绘制**：从屏幕左侧逐列向右处理，
 *          每一列内先恢复被上一帧波形覆盖的像素，紧接着写入本帧波形。
 *
 *          之所以这样安排：若先整体擦除、再整体重绘，两次操作之间
 *          波形区会短暂呈现"没有波形"的状态，在逐像素写入的并口屏上
 *          会表现为可见闪烁。按列交错后，屏幕上任一时刻要么是完整的
 *          新波形、要么是完整的旧波形，不会出现空白。
 *
 * @note    本帧与上一帧在同列附近可能存在高度差，需要在列内竖直填充，
 *          否则信号陡变时曲线会断裂或留下残点。
 */
static void draw_waveform(void)
{
    /** 上一帧各列的波形纵坐标，用于精确擦除 */
    static u16 prev_y[SCR_W];

    /** 上一帧各列是否已绘制（首帧为 0，避免擦到随机值） */
    static u8  prev_ok[SCR_W];

    u16 x;

    for (x = 0; x < SCR_W; x++)
    {
        u16 y_new = wave_y_at(x);
        u16 y_link_lo, y_link_hi;   /* 本帧波形在本列占用的纵向区间 */
        u16 y_old_lo, y_old_hi;     /* 上一帧波形在本列附近的纵向区间 */
        u16 y_lo, y_hi;             /* 需要重刷的总区间 */
        u16 y;

        /* ---- 1. 本帧波形区间 ----
         * 与前一列连成斜线：取两列纵坐标之间的整段，保证曲线连续 */
        y_link_lo = y_new;
        y_link_hi = y_new;

        if (x > 0)
        {
            u16 y_pc = wave_y_at((u16)(x - 1));

            if (y_pc < y_link_lo) y_link_lo = y_pc;
            if (y_pc > y_link_hi) y_link_hi = y_pc;
        }

        /* ---- 2. 上一帧波形残留区间 ----
         * 相邻两列的旧坐标都要还原，否则斜线会在列间留下残点 */
        y_old_lo = 0;
        y_old_hi = 0;
        {
            u8  has = 0;

            if (x > 0 && prev_ok[x - 1])
            {
                y_old_lo = prev_y[x - 1];
                y_old_hi = prev_y[x - 1];
                has = 1;
            }

            if (prev_ok[x])
            {
                if (!has)
                {
                    y_old_lo = prev_y[x];
                    y_old_hi = prev_y[x];
                    has = 1;
                }
                else
                {
                    if (prev_y[x] < y_old_lo) y_old_lo = prev_y[x];
                    if (prev_y[x] > y_old_hi) y_old_hi = prev_y[x];
                }
            }

            if (!has)
            {
                /* 首帧：没有旧内容需要擦除 */
                y_old_lo = y_link_lo;
                y_old_hi = y_link_lo;
            }
        }

        /* ---- 3. 重刷区间 = 本帧波形 ∪ 上一帧残留 ----
         * 一次遍历内完成擦除与绘制：落在本帧波形区间的画波形色，
         * 其余恢复为背景色（网格或触发电平线）。
         * 由于同一像素只写入一次，不会出现"先擦后画"的中间空白。 */
        y_lo = (y_link_lo < y_old_lo) ? y_link_lo : y_old_lo;
        y_hi = (y_link_hi > y_old_hi) ? y_link_hi : y_old_hi;

        for (y = y_lo; y <= y_hi; y++)
        {
            LCD_Fast_DrawPoint(x, y,
                (y >= y_link_lo && y <= y_link_hi) ? C_WAVE : bg_pixel_color(x, y));
        }

        prev_y[x]  = y_new;
        prev_ok[x] = 1;
    }

    POINT_COLOR = C_TEXT;
    BACK_COLOR  = C_BG;
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
        s_erase_all = 1;        /* 换显示模式，画面整体重建 */
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

    /* 状态栏只在内容变化时重绘（内容少，代价低） */
    if (s_need_bar)
    {
        s_need_bar = 0;
        draw_status_bar();
    }

    if (!s_need_full) return;

    /* ---- 帧率限制 ----
     * LCD 为并口逐像素写入，整幅波形区重绘需要十余毫秒。
     * 若不限速，主循环会在一次绘制完成前就发起下一次，
     * 画面被反复截断，表现为明显闪烁。 */
    if ((u32)(TIM_GetTick() - s_last_draw) < FRAME_MS) return;

    s_last_draw = TIM_GetTick();
    s_need_full = 0;
    g_frame_count++;

    if (s_display == OSC_UI_WAVE)
    {
        /* 需要整体重建时先重画背景。draw_waveform 内部按列交错完成
         * "擦旧 + 画新"，因此不需要额外的擦除步骤。 */
        if (s_erase_all)
        {
            draw_grid();
            s_erase_all = 0;
        }

        draw_waveform();
    }
    else
    {
        draw_spectrum();
    }

    draw_info_bar();
}
