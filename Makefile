# ============================================================
#  03-Oscilloscope —— 迷你数字示波器（STM32F103RC + 2.8寸TFT）
#  纯裸机实现，不使用图形库，按键操作
#
#  用法：build.bat           增量构建
#        build.bat clean     清理 build\（改了 .h 之后必须这样）
#        build.bat DEBUG=1   带调试信息
#
#  注意：不要给 make 加 -j（ARMCC 内联汇编用固定临时文件名，并行会互相覆盖）
# ============================================================

# ---------------- 工具链（Keil MDK ARMCC v5） ----------------
KEIL    := D:/apps/keil5MDK
ARMBIN  := $(KEIL)/ARM/ARMCC/bin
CC      := $(ARMBIN)/armcc.exe
AS      := $(ARMBIN)/armasm.exe
LD      := $(ARMBIN)/armlink.exe
ELF     := $(ARMBIN)/fromelf.exe

# ---------------- 工程路径 ----------------
ROOT    := D:/Workspace/LVGL/03-Oscilloscope
BUILD   := $(ROOT)/build
OBJDIR  := $(BUILD)/obj
TARGET  := $(BUILD)/Oscilloscope

# ---------------- 编译选项 ----------------
DEFS    := -DSTM32F10X_HD -DUSE_STDPERIPH_DRIVER

INCS    := -I$(ROOT)/USER \
           -I$(ROOT)/CORE \
           -I$(ROOT)/SYSTEM/delay \
           -I$(ROOT)/SYSTEM/sys \
           -I$(ROOT)/SYSTEM/usart \
           -I$(ROOT)/HARDWARE/LED \
           -I$(ROOT)/HARDWARE/LCD \
           -I$(ROOT)/HARDWARE/KEY \
           -I$(ROOT)/HARDWARE/TIMER \
           -I$(ROOT)/HARDWARE/APP \
           -I$(ROOT)/STM32F10x_FWLib/inc

# DEBUG=1 时加入调试信息(-g)：断点可对应源码行、可查看变量
# 用法:  build.bat DEBUG=1
ifeq ($(DEBUG),1)
DBGFLAG := -g
endif

CFLAGS  := --cpu Cortex-M3 --c99 -c --split_sections $(DBGFLAG)
AFLAGS  := --cpu Cortex-M3 $(DBGFLAG)

# 分散加载文件：三行关键规则缺一不可
#   *.o (RESET, +First)  向量表必须在镜像最开头
#   *(InRoot$$Sections)  C 库启动代码
#   .ANY (+RW +ZI)       读写/零初始化数据放入 RAM
LFLAGS  := --cpu Cortex-M3 --scatter $(ROOT)/OBJ/Oscilloscope.sct \
           --strict --info sizes --info totals

# ---------------- 源文件 ----------------
SRCS_C := \
  $(ROOT)/USER/main.c \
  $(ROOT)/USER/stm32f10x_it.c \
  $(ROOT)/USER/system_stm32f10x.c \
  $(ROOT)/HARDWARE/LED/led.c \
  $(ROOT)/HARDWARE/LCD/lcd.c \
  $(ROOT)/HARDWARE/KEY/osc_key.c \
  $(ROOT)/HARDWARE/TIMER/timer.c \
  $(ROOT)/HARDWARE/APP/osc_adc.c \
  $(ROOT)/HARDWARE/APP/osc_trigger.c \
  $(ROOT)/HARDWARE/APP/osc_measure.c \
  $(ROOT)/HARDWARE/APP/osc_fft.c \
  $(ROOT)/HARDWARE/APP/osc_ui.c \
  $(ROOT)/SYSTEM/delay/delay.c \
  $(ROOT)/SYSTEM/sys/sys.c \
  $(ROOT)/SYSTEM/usart/usart.c \
  $(ROOT)/CORE/core_cm3.c \
  $(ROOT)/STM32F10x_FWLib/src/stm32f10x_gpio.c \
  $(ROOT)/STM32F10x_FWLib/src/stm32f10x_rcc.c \
  $(ROOT)/STM32F10x_FWLib/src/stm32f10x_usart.c \
  $(ROOT)/STM32F10x_FWLib/src/misc.c \
  $(ROOT)/STM32F10x_FWLib/src/stm32f10x_tim.c \
  $(ROOT)/STM32F10x_FWLib/src/stm32f10x_adc.c \
  $(ROOT)/STM32F10x_FWLib/src/stm32f10x_dma.c

SRCS_S := $(ROOT)/CORE/startup_stm32f10x_hd.s

# ---------------- 目标文件 ----------------
# 链接顺序：startup 必须放第一个（承载 RESET 向量表）
OBJS := $(patsubst $(ROOT)/%.s,$(OBJDIR)/%.o,$(SRCS_S)) \
        $(patsubst $(ROOT)/%.c,$(OBJDIR)/%.o,$(SRCS_C))

OBJDIRS := $(sort $(dir $(OBJS)))

# ---------------- 目录准备（cmd.exe 只认反斜杠，故做替换） ----------------
OBJDIRS_WIN := $(subst /,\,$(OBJDIRS))
SENTINEL    := $(BUILD)/.dirs

.PHONY: all clean size

all: $(TARGET).hex

$(SENTINEL):
	@if not exist "$(subst /,\,$(BUILD))" mkdir "$(subst /,\,$(BUILD))"
	@$(foreach d,$(OBJDIRS_WIN),if not exist "$(d)" mkdir "$(d)" &)
	@echo done > $(SENTINEL)

$(TARGET).hex: $(TARGET).axf
	$(ELF) --i32combined --output $@ $<

$(TARGET).axf: $(OBJS) | $(SENTINEL)
	@echo "Linking $@ ..."
	$(LD) $(LFLAGS) -o $@ $(OBJS)

$(OBJDIR)/%.o: $(ROOT)/%.c | $(SENTINEL)
	$(CC) $(CFLAGS) $(DEFS) $(INCS) -o $@ $<

$(OBJDIR)/%.o: $(ROOT)/%.s | $(SENTINEL)
	$(AS) $(AFLAGS) $(INCS) -o $@ $<

size: $(TARGET).axf
	$(ELF) --text -c $<

clean:
	@if exist "$(subst /,\,$(BUILD))" rmdir /S /Q "$(subst /,\,$(BUILD))"
	@echo "cleaned $(BUILD)"
