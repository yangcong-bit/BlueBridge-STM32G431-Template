/**
 * @file    adc_app.c
 * @brief   ADC DMA 底层驱动与双重滤波算法实现 (硬件去极值 + 软件滑动窗口)
 */
#include "adc_app.h"
#include "adc.h"           // 引入 hadc1 和 hadc2 硬件句柄
#include "global_system.h" // 引入全局字典，用于上报最终电压

/* ==========================================
 * 底层 DMA 句柄引入
 * @note  CubeMX 只在 adc.c 里【定义】hdma_adc1/hdma_adc2，
 * adc.h 只导出 hadc1/hadc2，因此这里需要按 uart_app.c 的同样写法手动 extern。
 * ========================================== */
extern DMA_HandleTypeDef hdma_adc1;
extern DMA_HandleTypeDef hdma_adc2;

/* ==========================================
 * 私有内存区 (严禁外部 extern)
 * ========================================== */
/** * @brief ADC DMA 双缓冲底层接收数组 
 * [0][x] 对应 ADC1_IN11 (板载 R38 电位器)
 * [1][x] 对应 ADC2_IN15 (板载 R37 电位器)
 */
static uint16_t dma_buff[2][30];

// 定义滑动窗口采集次数
#define ADC_SAMPLE_COUNT 5

/**
 * @brief 内部私有函数：滑动窗口去极值中值平均滤波
 */
static uint16_t ADC_Sliding_Filter(uint8_t channel_id, uint16_t adc_raw_val) 
{
    static uint16_t buf[2][ADC_SAMPLE_COUNT] = {0}; 
    static uint8_t count[2] = {0};
    uint32_t sum = 0;
    uint16_t max = 0, min = 4095;
    
    // 1. 压入新数据
    buf[channel_id][count[channel_id]++] = adc_raw_val;
    if(count[channel_id] >= ADC_SAMPLE_COUNT) count[channel_id] = 0;
    
    // 2. 扫描极值与总和
    for(int i = 0; i < ADC_SAMPLE_COUNT; i++) {
        sum += buf[channel_id][i];
        if(buf[channel_id][i] > max) max = buf[channel_id][i];
        if(buf[channel_id][i] < min) min = buf[channel_id][i];
    }
    
    // 3. 去掉一个最高尖刺，去掉一个最低尖刺，求均值
    sum = sum - max - min;
    return (uint16_t)(sum / (ADC_SAMPLE_COUNT - 2));
}

/**
 * @brief  ADC 与 DMA 底层启动
 * @note   必须在 main 的 while(1) 之前调用
 */
void ADC_Init(void) {
    // 1. 硬件自校准 (极大提高精度，比赛必加)
    HAL_ADCEx_Calibration_Start(&ADC_R38_HANDLE, ADC_SINGLE_ENDED);
    HAL_ADCEx_Calibration_Start(&ADC_R37_HANDLE, ADC_SINGLE_ENDED);
    
    HAL_ADC_Start_DMA(&ADC_R38_HANDLE, (uint32_t*)dma_buff[0], 30);
    HAL_ADC_Start_DMA(&ADC_R37_HANDLE, (uint32_t*)dma_buff[1], 30);

    /* 【性能要点】HAL_ADC_Start_DMA 默认会打开 DMA 的【半传输】与【传输完成】中断。
     * 本工程 ADC 时钟 = PCLK2/2 = 40MHz，12 位转换总时长仅 15 个 ADC 周期，
     * 即约 2.67 MSa/s；30 点环形缓冲意味着每通道约 178k 次中断/秒，
     * 两路合计约 35 万次/秒，会吃掉主循环大半 CPU，时间片调度形同虚设。
     * 这里改为【纯 DMA 静默搬运】：关掉两个中断且不启动 NVIC，CPU 零参与，
     * adc_proc() 每 10ms 直接读取最新 30 点窗口即可。
     * 说明：环形缓冲会被 DMA 边写边读，但电位器经 RC 滤波后 10us 内电平无变化，
     * 且后面还有去极值 + 滑动窗口双重滤波，该"撕裂读"无实际影响。 */
    __HAL_DMA_DISABLE_IT(&hdma_adc1, DMA_IT_HT);
    __HAL_DMA_DISABLE_IT(&hdma_adc1, DMA_IT_TC);
    __HAL_DMA_DISABLE_IT(&hdma_adc2, DMA_IT_HT);
    __HAL_DMA_DISABLE_IT(&hdma_adc2, DMA_IT_TC);
    HAL_NVIC_DisableIRQ(DMA1_Channel1_IRQn);
    HAL_NVIC_DisableIRQ(DMA1_Channel2_IRQn);
    /* 传输错误中断也一并关掉：DMA 只在总线级故障时才会触发它，届时缓冲区会
     * 保持旧值，而读数每 10ms 仍在刷新，属于温和的降级行为；若打开它则需要
     * 额外实现 ErrorCallback，对一个电位器读数而言没有必要。 */
    __HAL_DMA_DISABLE_IT(&hdma_adc1, DMA_IT_TE);
    __HAL_DMA_DISABLE_IT(&hdma_adc2, DMA_IT_TE);
}

/**
 * @brief  ADC 数据处理任务 
 * @note   建议调度周期：10ms~50ms。处理完毕后直接将计算出的物理电压上报给 sys 字典。
 */
void adc_proc(void) {
    uint32_t sum_adc1 = 0, sum_adc2 = 0;
    uint16_t max1 = 0, min1 = 4095;
    uint16_t max2 = 0, min2 = 4095;

    // --- 第一重防护：DMA 硬件级瞬态清洗 ---
    // 1. 遍历 30 个采样点，累加并找出最大最小值
    for(uint8_t i = 0; i < 30; i++) {
        // 统计通道 1 (R38)
        if(dma_buff[0][i] > max1) max1 = dma_buff[0][i];
        if(dma_buff[0][i] < min1) min1 = dma_buff[0][i];
        sum_adc1 += dma_buff[0][i];

        // 统计通道 2 (R37)
        if(dma_buff[1][i] > max2) max2 = dma_buff[1][i];
        if(dma_buff[1][i] < min2) min2 = dma_buff[1][i];
        sum_adc2 += dma_buff[1][i];
    }

    // 2. 获取当前周期内相对干净的原始均值
    uint16_t hw_raw_r38 = (sum_adc1 - max1 - min1) / 28;
    uint16_t hw_raw_r37 = (sum_adc2 - max2 - min2) / 28;

    // --- 第二重防护：时间轴滑动窗口滤波 ---
    // 3. 将硬件均值送入滑动窗口，消除长周期的突发尖刺
    uint16_t final_r38 = ADC_Sliding_Filter(0, hw_raw_r38);
    uint16_t final_r37 = ADC_Sliding_Filter(1, hw_raw_r37);

    // 4. 换算为真实电压并直接同步至全局字典
    sys.r38_voltage = (final_r38 / 4096.0f) * 3.3f; 
    sys.r37_voltage = (final_r37 / 4096.0f) * 3.3f; 
}
