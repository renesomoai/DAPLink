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
static uint8_t diag_stage;                /* 0 ok, 1 reset-calibration timeout, 2 calibration timeout, 3 conversion timeout */

uint8_t target_vtref_diag(uint8_t *out)   /* [stage, ADC STATR lo, CTLR2 byte0, CTLR2 byte1, CTLR2 byte2, CTLR1 byte3] */
{
    out[0] = diag_stage;
    out[1] = (uint8_t)ADC1->STATR;
    out[2] = (uint8_t)ADC1->CTLR2;
    out[3] = (uint8_t)(ADC1->CTLR2 >> 8);
    out[4] = (uint8_t)(ADC1->CTLR2 >> 16);
    out[5] = (uint8_t)(ADC1->CTLR1 >> 24);
    return 6;
}

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
    a.ADC_OutputBuffer = ADC_OutputBuffer_Disable;    /* NOT set by ADC_StructInit on V20x: stack garbage would be OR-ed into CTLR1 */
    a.ADC_Pga = ADC_Pga_1;
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
    if (!n) { diag_stage = 1; return 0; }
    ADC_StartCalibration(ADC1);
    for (n = ADC_WAIT_LOOPS; ADC_GetCalibrationStatus(ADC1) && n; n--) {}
    if (!n) { diag_stage = 2; }
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
        if (!n) { diag_stage = 3; return VTREF_MV_ERROR; }
        sum += ADC_GetConversionValue(ADC1);           /* reading DR clears EOC */
    }
    diag_stage = 0;
    return (uint16_t)((sum * VDDA_MV * DIVIDER_RATIO) / (OVERSAMPLE * 4095U));
}
