/**
 * @file    freq_app.c
 * @brief   双通道输入捕获中断回调与频率计算核心 (完美处理计数器溢出)
 * @note    已完成底层硬件宏替换解耦。修改引脚或定时器请至 global_system.h 中修改宏。
 */
#include "freq_app.h"
#include "tim.h"
#include "global_system.h" // 上报数据至全局字典，并引入底层硬件映射宏

/* ==========================================================
 * 通道 1 私有状态机变量 (对应 FREQ_CH1)
 * ========================================================== */
#if USE_HW_PWM_CH1 == 1
    static uint32_t ic1_period = 0, ic1_high = 0;
    static uint8_t  ic1_ready  = 0; 
#else
    static uint32_t ic1_val1 = 0, ic1_val2 = 0, ic1_val3 = 0;
    static uint8_t  ic1_state  = 0; 
#endif
static uint32_t ic1_last_tick = 0; 

/* ==========================================================
 * 通道 2 私有状态机变量 (对应 FREQ_CH2)
 * ========================================================== */
#if USE_HW_PWM_CH2 == 1
    static uint32_t ic2_period = 0, ic2_high = 0;
    static uint8_t  ic2_ready  = 0;
#else
    static uint32_t ic2_val1 = 0, ic2_val2 = 0, ic2_val3 = 0;
    static uint8_t  ic2_state  = 0;
#endif
static uint32_t ic2_last_tick = 0; 


/**
 * @brief  输入捕获初始化
 */
void Freq_Init(void) {
    // --- 启动 CH1 监听 ---
    HAL_TIM_IC_Start_IT(&FREQ_CH1_HANDLE, FREQ_CH1_CH_MAIN);
#if USE_HW_PWM_CH1 == 1
    HAL_TIM_IC_Start(&FREQ_CH1_HANDLE, FREQ_CH1_CH_SUB); 
#endif

    // --- 启动 CH2 监听 ---
    HAL_TIM_IC_Start_IT(&FREQ_CH2_HANDLE, FREQ_CH2_CH_MAIN);
#if USE_HW_PWM_CH2 == 1
    HAL_TIM_IC_Start(&FREQ_CH2_HANDLE, FREQ_CH2_CH_SUB); 
#endif
}


/**
 * @brief HAL 库统一的输入捕获中断回调
 */
void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim) {
    
    // ---------------- 抓取 CH1 ----------------
    if (htim->Instance == FREQ_CH1_INST && htim->Channel == FREQ_CH1_ACTIVE) {
        ic1_last_tick = HAL_GetTick(); // 刷新零频保护看门狗
        
#if USE_HW_PWM_CH1 == 1
        ic1_period = HAL_TIM_ReadCapturedValue(htim, FREQ_CH1_CH_MAIN);
        ic1_high   = HAL_TIM_ReadCapturedValue(htim, FREQ_CH1_CH_SUB);
        ic1_ready  = 1;
#else
        // 软件三步曲：上升沿抓首点 -> 下降沿抓高电平 -> 上升沿抓周期
        if (ic1_state == 0) {
            ic1_val1 = HAL_TIM_ReadCapturedValue(htim, FREQ_CH1_CH_MAIN);
            __HAL_TIM_SET_CAPTUREPOLARITY(htim, FREQ_CH1_CH_MAIN, TIM_INPUTCHANNELPOLARITY_FALLING);
            ic1_state = 1; 
        } else if (ic1_state == 1) {
            ic1_val2 = HAL_TIM_ReadCapturedValue(htim, FREQ_CH1_CH_MAIN);
            __HAL_TIM_SET_CAPTUREPOLARITY(htim, FREQ_CH1_CH_MAIN, TIM_INPUTCHANNELPOLARITY_RISING);
            ic1_state = 2; 
        } else if (ic1_state == 2) {
            ic1_val3 = HAL_TIM_ReadCapturedValue(htim, FREQ_CH1_CH_MAIN);
            ic1_state = 3; 
        }
#endif
    }
    
    // ---------------- 抓取 CH2 ----------------
    if (htim->Instance == FREQ_CH2_INST && htim->Channel == FREQ_CH2_ACTIVE) {
        ic2_last_tick = HAL_GetTick(); 
        
#if USE_HW_PWM_CH2 == 1
        ic2_period = HAL_TIM_ReadCapturedValue(htim, FREQ_CH2_CH_MAIN);
        ic2_high   = HAL_TIM_ReadCapturedValue(htim, FREQ_CH2_CH_SUB);
        ic2_ready  = 1;
#else
        if (ic2_state == 0) {
            ic2_val1 = HAL_TIM_ReadCapturedValue(htim, FREQ_CH2_CH_MAIN);
            __HAL_TIM_SET_CAPTUREPOLARITY(htim, FREQ_CH2_CH_MAIN, TIM_INPUTCHANNELPOLARITY_FALLING);
            ic2_state = 1; 
        } else if (ic2_state == 1) {
            ic2_val2 = HAL_TIM_ReadCapturedValue(htim, FREQ_CH2_CH_MAIN);
            __HAL_TIM_SET_CAPTUREPOLARITY(htim, FREQ_CH2_CH_MAIN, TIM_INPUTCHANNELPOLARITY_RISING);
            ic2_state = 2; 
        } else if (ic2_state == 2) {
            ic2_val3 = HAL_TIM_ReadCapturedValue(htim, FREQ_CH2_CH_MAIN);
            ic2_state = 3; 
        }
#endif
    }
}


/**
 * @brief  输入捕获业务结算与零频保护 (核心防溢出算法)
 * @note   建议调度周期：50ms。计算完毕同步至 sys 字典。
 */
void Freq_Proc(void) {
    uint32_t current_tick = HAL_GetTick();

    /* ================= CH1 频率结算 ================= */
#if USE_HW_PWM_CH1 == 1
    if (ic1_ready) {
        if (ic1_period != 0) {
            sys.period_ch1 = ic1_period;
            sys.freq_ch1 = 1000000 / ic1_period; 
            sys.duty_ch1 = ((float)ic1_high / (float)ic1_period) * 100.0f;
        }
        ic1_ready = 0;
    }
#else
    if (ic1_state == 3) {
        // 防溢出算法：如果 v3 >= v1 正常计算；如果 v3 < v1 说明中间溢出过一次。
        // 此处默认以 32位 定时器兼容写法处理，如果使用了 16位 定时器，请注意溢出点。
        uint32_t period_ticks = (ic1_val3 >= ic1_val1) ? (ic1_val3 - ic1_val1) : ((0xFFFFFFFF - ic1_val1) + ic1_val3 + 1);
        uint32_t high_ticks   = (ic1_val2 >= ic1_val1) ? (ic1_val2 - ic1_val1) : ((0xFFFFFFFF - ic1_val1) + ic1_val2 + 1);
        
        if (period_ticks > 0) {
            sys.period_ch1 = period_ticks; 
            sys.freq_ch1 = 1000000 / period_ticks;
            sys.duty_ch1 = ((float)high_ticks / (float)period_ticks) * 100.0f;
        }
        ic1_state = 0; 
    }
#endif
    
    // 【零频看门狗】：超过 1000ms 未触发中断，说明信号被拔掉或死区，强制归零
    if (current_tick - ic1_last_tick > 1000) {
        sys.freq_ch1 = 0; sys.period_ch1 = 0; sys.duty_ch1 = 0.0f;
#if USE_HW_PWM_CH1 == 0
        ic1_state = 0; 
        __HAL_TIM_SET_CAPTUREPOLARITY(&FREQ_CH1_HANDLE, FREQ_CH1_CH_MAIN, TIM_INPUTCHANNELPOLARITY_RISING);
#endif
    }

    /* ================= CH2 频率结算 ================= */
#if USE_HW_PWM_CH2 == 1
    if (ic2_ready) {
        if (ic2_period != 0) {
            sys.period_ch2 = ic2_period;
            sys.freq_ch2 = 1000000 / ic2_period;
            sys.duty_ch2 = ((float)ic2_high / (float)ic2_period) * 100.0f;
        }
        ic2_ready = 0;
    }
#else
    if (ic2_state == 3) {
        // 防溢出算法：如果换成 16 位定时器（如TIM3），请将 0xFFFFFFFF 改为 0xFFFF
        uint32_t period_ticks = (ic2_val3 >= ic2_val1) ? (ic2_val3 - ic2_val1) : ((0xFFFFFFFF - ic2_val1) + ic2_val3 + 1);
        uint32_t high_ticks   = (ic2_val2 >= ic2_val1) ? (ic2_val2 - ic2_val1) : ((0xFFFFFFFF - ic2_val1) + ic2_val2 + 1);
        
        if (period_ticks > 0) {
            sys.period_ch2 = period_ticks; 
            sys.freq_ch2 = 1000000 / period_ticks;
            sys.duty_ch2 = ((float)high_ticks / (float)period_ticks) * 100.0f;
        }
        ic2_state = 0; 
    }
#endif

    // CH2 零频看门狗
    if (current_tick - ic2_last_tick > 1000) {
        sys.freq_ch2 = 0; sys.period_ch2 = 0; sys.duty_ch2 = 0.0f;
#if USE_HW_PWM_CH2 == 0
        ic2_state = 0; 
        __HAL_TIM_SET_CAPTUREPOLARITY(&FREQ_CH2_HANDLE, FREQ_CH2_CH_MAIN, TIM_INPUTCHANNELPOLARITY_RISING);
#endif
    }
}
