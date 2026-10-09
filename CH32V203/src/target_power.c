#include "target_power.h"
#include "ch32v20x.h"
#include "ch32v20x_adc.h"
#include "ch32v20x_gpio.h"
#include "ch32v20x_rcc.h"

#define ADC_WAIT_LOOPS   200000U        /* bounded polling: a dead ADC must never hang the DAP command loop */
#define VDDA_MV          3300U
#define DIVIDER_RATIO    2U             /* 100k / 100k */
#define OVERSAMPLE       8U

static uint8_t adc_ready;

static int adc_init(void)
{
    GPIO_InitTypeDef g;
    ADC_InitTypeDef a;
    uint32_t n;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_ADC1, ENABLE);
    RCC_ADCCLKConfig(RCC_PCLK2_Div8);                  /* 144/8 = 18 MHz: above the 14 MHz spec; WCH examples use it at 144 MHz.
                                                          DC reading only; validate on hardware (#23) */

    g.GPIO_Pin = GPIO_Pin_1;
    g.GPIO_Mode = GPIO_Mode_AIN;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &g);

    ADC_DeInit(ADC1);
    ADC_StructInit(&a);
    a.ADC_Mode = ADC_Mode_Independent;
    a.ADC_ScanConvMode = DISABLE;
    a.ADC_ContinuousConvMode = DISABLE;
    a.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    a.ADC_DataAlign = ADC_DataAlign_Right;
    a.ADC_NbrOfChannel = 1;
    ADC_Init(ADC1, &a);
    ADC_RegularChannelConfig(ADC1, ADC_Channel_9, 1, ADC_SampleTime_239Cycles5);   /* 50 k source impedance */
    ADC_Cmd(ADC1, ENABLE);

    ADC_ResetCalibration(ADC1);
    for (n = ADC_WAIT_LOOPS; ADC_GetResetCalibrationStatus(ADC1) && n; n--) {}
    if (!n) { return 0; }
    ADC_StartCalibration(ADC1);
    for (n = ADC_WAIT_LOOPS; ADC_GetCalibrationStatus(ADC1) && n; n--) {}
    return n != 0;
}

uint16_t target_vtref_mv(void)
{
    uint32_t sum = 0, n, i;

    if (!adc_ready) {
        if (!adc_init()) { return VTREF_MV_ERROR; }
        adc_ready = 1;
    }
    for (i = 0; i < OVERSAMPLE; i++) {
        ADC_SoftwareStartConvCmd(ADC1, ENABLE);
        for (n = ADC_WAIT_LOOPS; !ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) && n; n--) {}
        if (!n) { return VTREF_MV_ERROR; }
        sum += ADC_GetConversionValue(ADC1);           /* reading DR clears EOC */
    }
    return (uint16_t)((sum * VDDA_MV * DIVIDER_RATIO) / (OVERSAMPLE * 4095U));
}
