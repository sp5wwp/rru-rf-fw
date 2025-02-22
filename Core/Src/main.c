/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2023 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "term.h"
#include "interface_cmds.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define DBG_MSGS									//enable debug messages over DBG_UART
#define IDENT_STR		"Remote Radio Unit (RRU) 420-450 MHz\nFW v1.0.0 by Wojciech SP5WWP"
#define VDDA			(3.24f)						//measured VDDA voltage
#define CC1200_REG_NUM	51							//number of regs used to initialize CC1200s
#define BSB_TX_BUFLEN	(6*960U)					//tx buffer size in samples (240ms at fs=24kHz)
#define BSB_TX_THRESH	(2*960U)					//80ms worth of baseband data (at 24kHz)
#define BSB_RX_BUFLEN	(2*960U)					//how many samples do we need to collect before sending them over CARI?

#define FCORR_STEP		(40.0e6f/262144.0f/8.0f)	//unit value for frequency correction
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
#define FIX_TIMER_TRIGGER(handle_ptr) (__HAL_TIM_CLEAR_FLAG(handle_ptr, TIM_SR_UIF))
#define DBG_HALT while(1)
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;

DAC_HandleTypeDef hdac;

SPI_HandleTypeDef hspi1;

TIM_HandleTypeDef htim6;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart3;
DMA_HandleTypeDef hdma_usart1_tx;
DMA_HandleTypeDef hdma_usart1_rx;

DMA_HandleTypeDef hdma_memtomem_dma2_stream1;
/* USER CODE BEGIN PV */
#include "enums.h"

uint32_t dev_err=(uint32_t)ERR_OK;	//default state - no error

struct trx_data_t
{
	uint8_t name[20];		//chip's name (CC1200, CC1201, unknown ID)
	uint32_t frequency;		//frequency in hertz
	uint8_t pwr;			//power setting (3..63)
	int16_t fcorr;			//frequency correction
	uint8_t pll_locked;		//PLL locked flag
}trx_data[2];

//PA
uint16_t alc_set=0;											//automatic level control setting (nonlinear)
float tx_dbm=0.0f;											//RF power setpoint, dBm

//buffers and interface stuff
volatile uint8_t rxb[1000]={0};								//rx buffer for interface data
volatile uint32_t rx_bc=0;									//UART1 rx byte counter
volatile int8_t tx_bsb_buff[BSB_TX_BUFLEN]={0};				//buffer for transmission
volatile int8_t rx_bsb_buff[BSB_RX_BUFLEN]={0};				//buffer for reception
volatile uint32_t tx_bsb_total_cnt=0;						//how many samples were received (over CARI)
volatile uint32_t tx_bsb_cnt=0;								//how many samples were transmitted
volatile uint32_t rx_bsb_cnt=0;								//how many samples were received (from CC1200)
volatile uint8_t rx_bsb_buff_rdy=0;							//which part of rx_bsb_buff is ready to be sent out (0-none)
volatile uint8_t bsb_tx_cplt=0;								//baseband transmission over RF complete?
volatile uint8_t meas_req=0;								//measurement request received?

volatile enum trx_state_t tx_state=TX_IDLE;					//transmitter state
volatile enum trx_state_t rx_state=RX_IDLE;					//receiver state
volatile uint8_t bsb_trig=0;								//24kHz baseband trigger signal
volatile enum interface_comm_t interface_comm=COMM_IDLE;	//interface comm status

//ADC stuff
uint32_t adc_vals[3]={0};									//raw values read from ADC channels 0, 1, and 2

//AD8318 RF power sensor cal data (consts for now)
//based on 2-point linear approximation
//P(U) = a*U + b + cal_atten + cal_corr
//U[V]		P[dBm]
//0.64		0.0
//1.85		-50.0
const float cal_a		= -41.3223140495868f;	//dBm/V
const float cal_b		=  26.4462809917355f;	//dBm
const float cal_offs	=  62.5f;				//dB
const float cal_corr	=  -3.6f;				//dB
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_DAC_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_TIM6_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void dbg_print(const char* color_code, const char* fmt, ...)
{
	#ifdef DBG_MSGS
	char str[100];
	va_list ap;

	va_start(ap, fmt);
	vsprintf(str, fmt, ap);
	va_end(ap);

	if(color_code!=NULL)
	{
		HAL_UART_Transmit(&huart3, (uint8_t*)color_code, strlen(color_code), 10);
		HAL_UART_Transmit(&huart3, (uint8_t*)str, strlen(str), 100);
		HAL_UART_Transmit(&huart3, (uint8_t*)TERM_DEFAULT, strlen(TERM_DEFAULT), 10);
	}
	else
	{
		HAL_UART_Transmit(&huart3, (uint8_t*)str, strlen(str), 100);
	}
	#endif
}

uint16_t dbm_to_alc(float dbm)
{
	return roundf(43.8033381164565f*dbm+564.564609923722f);
}

//get RF power in dBm
//and temperature in degs C, based on LMT87's output voltage
void get_adc_vals(float* pwr_fwd, float* pwr_ref, float* temp)
{
	uint32_t values[3];
	HAL_ADC_Start_DMA(&hadc1, values, 3);
	HAL_ADC_PollForConversion(&hadc1, 20);

	float u=values[2]/4096.0f*VDDA;
	HAL_Delay(1); //TODO: why is this delay needed to make this func work?

	//dbg_print(TERM_YELLOW, "[DBG] %ld, %ld, %ld\n", values[0], values[1], values[2]);
	//dbg_print(TERM_YELLOW, "[DBG] %1.4f\n", u);

	*pwr_fwd=cal_a*(values[0]/4096.0f*VDDA)+cal_b + cal_offs+cal_corr; //take coupling and attenuation into account
	*pwr_ref=cal_a*(values[1]/4096.0f*VDDA)+cal_b + cal_offs+cal_corr;
	*temp=(-13.582f+sqrtf(184.470724f+0.01732f*(2.2308f-u)*1000.0f))/0.00866f + 30.0f;
}

//set rf power output setpoint
//0 - lowest power, 0xFFF - max power
void set_rf_pwr_setpoint(const uint16_t pwr)
{
	HAL_DAC_SetValue(&hdac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, pwr>0xFFF?0xFFF:pwr);
	HAL_DAC_Start(&hdac, DAC_CHANNEL_1);
}

void set_dac_ch2(uint16_t val)
{
	HAL_DAC_SetValue(&hdac, DAC_CHANNEL_2, DAC_ALIGN_12B_R, val>0xFFF?0xFFF:val);
	HAL_DAC_Start(&hdac, DAC_CHANNEL_2);
}

//calculate SWR based on fwd and ref power
//returns 0.0 when input data is invalid
float calc_swr(float fwd, float ref)
{
	//in an ideal world this would be physically impossible
	//but there's always some measurement error that could cause this
	if(ref>=fwd)
		return INFINITY;

	//dBm to watts
	fwd=powf(10.0f, fwd/10.0f-3.0f);
	ref=powf(10.0f, ref/10.0f-3.0f);

	float s=sqrtf(ref/fwd);
	float swr=(1.0f+s)/(1.0f-s);

	return swr;
}

//enable RF PA (assert one of two enable signals)
void rf_pa_en(uint8_t en)
{
	if(en)
		PA_EN_GPIO_Port->BSRR=(uint32_t)PA_EN_Pin;
	else
		PA_EN_GPIO_Port->BSRR=(uint32_t)PA_EN_Pin<<16;
}

void set_TP(enum tp_t tp, uint8_t state)
{
	if(tp==TP1)
	{
		if(state)
			DBG_TP1_GPIO_Port->BSRR=(uint32_t)DBG_TP1_Pin;
		else
			DBG_TP1_GPIO_Port->BSRR=(uint32_t)DBG_TP1_Pin<<16;
	}
	else if(tp==TP2)
	{
		if(state)
			DBG_TP2_GPIO_Port->BSRR=(uint32_t)DBG_TP2_Pin;
		else
			DBG_TP2_GPIO_Port->BSRR=(uint32_t)DBG_TP2_Pin<<16;
	}
}

void set_CS(enum trx_t trx, uint8_t state)
{
	if(trx==CHIP_RX)
	{
		if(state)
			RX_nCS_GPIO_Port->BSRR=(uint32_t)RX_nCS_Pin;
		else
			RX_nCS_GPIO_Port->BSRR=(uint32_t)RX_nCS_Pin<<16;
	}
	else if(trx==CHIP_TX)
	{
		if(state)
			TX_nCS_GPIO_Port->BSRR=(uint32_t)TX_nCS_Pin;
		else
			TX_nCS_GPIO_Port->BSRR=(uint32_t)TX_nCS_Pin<<16;
	}
}

uint8_t trx_readreg(enum trx_t trx, const uint16_t addr)
{
	uint8_t txd[3]={addr>>8, addr&0xFF, 0};
	uint8_t rxd[3]={0, 0, 0};

	set_CS(trx, 0);
	if(txd[0]==0)
	{
		txd[1]|=0x80;
		HAL_SPI_TransmitReceive(&hspi1, &txd[1], rxd, 2, 10);
		set_CS(trx, 1);
		return rxd[1];
	}
	else
	{
		txd[0]|=0x80;
		HAL_SPI_TransmitReceive(&hspi1, txd, rxd, 3, 10);
		set_CS(trx, 1);
		return rxd[2];
	}
}

uint8_t trx_writereg(enum trx_t trx, const uint16_t addr, const uint8_t val)
{
	uint8_t txd[3]={addr>>8, addr&0xFF, val};
	uint8_t rxd[3]={0, 0, 0};

	set_CS(trx, 0);
	if(txd[0]==0)
	{
		HAL_SPI_TransmitReceive(&hspi1, &txd[1], rxd, 2, 10);
	}
	else
	{
		HAL_SPI_TransmitReceive(&hspi1, txd, rxd, 3, 10);
	}
	set_CS(trx, 1);

	return rxd[0]>>7; //bit 7 is the `CHIP_RDYn` and should be low for normal operation
}

void trx_writecmd(enum trx_t trx, uint8_t addr)
{
	uint8_t txd=addr;

	set_CS(trx, 0);
	HAL_SPI_Transmit(&hspi1, &txd, 1, 10);
	set_CS(trx, 1);
}

uint8_t read_pn(enum trx_t trx)
{
	return trx_readreg(trx, 0x2F8F);
}

uint8_t read_status(enum trx_t trx)
{
	uint8_t txd=STR_SNOP; //no operation strobe
	uint8_t rxd=0;

	set_CS(trx, 0);
	HAL_SPI_TransmitReceive(&hspi1, &txd, &rxd, 1, 10);
	set_CS(trx, 1);

	return rxd;
}

void detect_ic(uint8_t* rx, uint8_t* tx)
{
	uint8_t rxid=read_pn(CHIP_RX);
	uint8_t txid=read_pn(CHIP_TX);

	if(rxid==0x20)
		sprintf((char*)rx, "CC1200");
	else if(rxid==0x21)
		sprintf((char*)rx, "CC1201");
	else
		sprintf((char*)rx, "unknown ID (0x%02X)", rxid);

	if(txid==0x20)
		sprintf((char*)tx, "CC1200");
	else if(txid==0x21)
		sprintf((char*)tx, "CC1201");
	else
		sprintf((char*)tx, "unknown ID (0x%02X)", txid);
}

void config_ic(enum trx_t trx, uint8_t* settings)
{
	for(uint8_t i=0; i<CC1200_REG_NUM; i++)
	{
		//dbg_print(TERM_YELLOW, "[%03d] 0x%02X 0x%02X 0x%02X\n",
		//		i*3, settings[i*3], settings[i*3+1], settings[i*3+2]);
		set_CS(trx, 0);
		if(settings[i*3])
			HAL_SPI_Transmit(&hspi1, &settings[i*3], 3, 10);
		else
			HAL_SPI_Transmit(&hspi1, &settings[i*3+1], 2, 10);
		set_CS(trx, 1);
		//HAL_Delay(10);
	}
}

void config_rf(enum trx_t trx, struct trx_data_t trx_data)
{
	static uint8_t cc1200_rx_settings[CC1200_REG_NUM*3] =
	{
		0x00, 0x01, 0x08,
		0x00, 0x03, 0x09,
		0x00, 0x08, 0x1F,
		0x00, 0x0A, 0xAD, //deviation - a little bit under 3.3kHz full scale
		0x00, 0x0B, 0x00, //deviation
		0x00, 0x0C, 0x5D,
		0x00, 0x0D, 0x00,
		0x00, 0x0E, 0x8A,
		0x00, 0x0F, 0xCB,
		0x00, 0x10, 0xAC, //RX filter BW - 9.5kHz
		0x00, 0x11, 0x00,
		0x00, 0x12, 0x45,
		0x00, 0x13, 0x43, //symbol rate 2 - 1.5k sym/s
		0x00, 0x14, 0xA9, //symbol rate 1
		0x00, 0x15, 0x2A, //symbol rate 0
		0x00, 0x16, 0x37, //AGC_REF - AGC Reference Level Configuration
		0x00, 0x17, 0xEC,
		0x00, 0x19, 0x11,
		0x00, 0x1B, 0x51,
		0x00, 0x1C, 0x87,
		0x00, 0x1D, 0x00,
		0x00, 0x20, 0x14,
		0x00, 0x26, 0x03,
		0x00, 0x27, 0x00,
		0x00, 0x28, 0x20,
		0x00, 0x2B, 0x03, //output power - 0x03..0x3F (doesn't matter for RX)
		0x00, 0x2E, 0xFF,
		0x2F, 0x00, 0x1C,
		0x2F, 0x01, 0x02, //AFC, 0x22 - on, 0x02 - off
		0x2F, 0x04, 0x00, //ECG_CFG - External Clock Frequency Configuration - irrelevant
		0x2F, 0x05, 0x09, //16x upsampler, CFM enable
		0x2F, 0x0C, 0x57, //frequency - round((float)435000000/5000000*(1<<16))=0x570000
		0x2F, 0x0D, 0x00, //frequency
		0x2F, 0x0E, 0x00, //frequency
		0x2F, 0x10, 0xEE,
		0x2F, 0x11, 0x10,
		0x2F, 0x12, 0x07,
		0x2F, 0x13, 0xAF, //FS loop bandwidth - 500kHz RX/TX
		0x2F, 0x16, 0x40,
		0x2F, 0x17, 0x0E,
		0x2F, 0x19, 0x03,
		0x2F, 0x1B, 0x33,
		0x2F, 0x1D, 0x17,
		0x2F, 0x1F, 0x00,
		0x2F, 0x20, 0x6E,
		0x2F, 0x21, 0x1C,
		0x2F, 0x22, 0xAC,
		0x2F, 0x27, 0xB5,
		0x2F, 0x32, 0x0E,
		0x2F, 0x36, 0x03,
		0x2F, 0x91, 0x08
	};

	static uint8_t cc1200_tx_settings[CC1200_REG_NUM*3] =
	{
		0x00, 0x01, 0x08,
		0x00, 0x03, 0x09,
		0x00, 0x08, 0x1F,
		0x00, 0x0A, 0xAD, //deviation - a little bit under 3.3kHz full scale
		0x00, 0x0B, 0x00, //deviation
		0x00, 0x0C, 0x5D,
		0x00, 0x0D, 0x00,
		0x00, 0x0E, 0x8A,
		0x00, 0x0F, 0xCB,
		0x00, 0x10, 0xAC, //RX filter BW - 9.5kHz (doesn't matter for TX)
		0x00, 0x11, 0x00,
		0x00, 0x12, 0x45,
		0x00, 0x13, 0x43, //symbol rate 2 - 1.5k symb/s
		0x00, 0x14, 0xA9, //symbol rate 1
		0x00, 0x15, 0x2A, //symbol rate 0
		0x00, 0x16, 0x37,
		0x00, 0x17, 0xEC,
		0x00, 0x19, 0x11,
		0x00, 0x1B, 0x51,
		0x00, 0x1C, 0x87,
		0x00, 0x1D, 0x00,
		0x00, 0x20, 0x14,
		0x00, 0x26, 0x03,
		0x00, 0x27, 0x00,
		0x00, 0x28, 0x20,
		0x00, 0x2B, 0x03, //output power - 0x03..0x3F
		0x00, 0x2E, 0xFF,
		0x2F, 0x00, 0x1C,
		0x2F, 0x01, 0x22,
		0x2F, 0x04, 0x00, //ECG_CFG - External Clock Frequency Configuration - irrelevant
		0x2F, 0x05, 0x09, //16x upsampler, CFM enable
		0x2F, 0x0C, 0x57, //frequency - round((float)435000000/5000000*(1<<16))=0x570000
		0x2F, 0x0D, 0x00, //frequency
		0x2F, 0x0E, 0x00, //frequency
		0x2F, 0x10, 0xEE,
		0x2F, 0x11, 0x10,
		0x2F, 0x12, 0x07,
		0x2F, 0x13, 0xAF, //FS loop bandwidth - 500kHz RX/TX
		0x2F, 0x16, 0x40,
		0x2F, 0x17, 0x0E,
		0x2F, 0x19, 0x03,
		0x2F, 0x1B, 0x33,
		0x2F, 0x1D, 0x17,
		0x2F, 0x1F, 0x00,
		0x2F, 0x20, 0x6E,
		0x2F, 0x21, 0x1C,
		0x2F, 0x22, 0xAC,
		0x2F, 0x27, 0xB5,
		0x2F, 0x32, 0x0E,
		0x2F, 0x36, 0x03,
		0x2F, 0x91, 0x08
	};

	uint32_t freq_word=roundf((float)trx_data.frequency/5000000.0*((uint32_t)1<<16));
	//dbg_print(0, "freq_word=%04lX\n", freq_word);

	if(trx==CHIP_RX)
	{
		cc1200_rx_settings[32*3-1]=(freq_word>>16)&0xFF;
		cc1200_rx_settings[33*3-1]=(freq_word>>8)&0xFF;
		cc1200_rx_settings[34*3-1]=freq_word&0xFF;
		config_ic(trx, cc1200_rx_settings);
		//trx_writereg(trx, 0x0001, 29);		//IOCFG2, GPIO2 - CLKEN_CFM
	}
	else if(trx==CHIP_TX)
	{
		uint8_t tx_pwr=trx_data.pwr;
		if(tx_pwr>0x3F) tx_pwr=0x3F;
		if(tx_pwr<0x03) tx_pwr=0x03;
		cc1200_tx_settings[26*3-1]=tx_pwr;
		cc1200_tx_settings[32*3-1]=(freq_word>>16)&0xFF;
		cc1200_tx_settings[33*3-1]=(freq_word>>8)&0xFF;
		cc1200_tx_settings[34*3-1]=freq_word&0xFF;
		config_ic(trx, cc1200_tx_settings);
		trx_writereg(trx, 0x0001, 30);		//IOCFG2, GPIO2 - CFM_TX_DATA_CLK
	}

	//frequency correction
	trx_writereg(trx, 0x2F0A, (uint16_t)trx_data.fcorr>>8);
	trx_writereg(trx, 0x2F0B, (uint16_t)trx_data.fcorr&0xFF);
	//disable address autoincrement in burst mode (default - enabled)
	//trx_writereg(trx, 0x2F06, 0);
	//KVCO high resolution enable
	trx_writereg(trx, 0x2F14, 1<<6);
}

void interface_resp(enum cari_cmd_t cmd, const uint8_t resp)
{
	static uint8_t tmp[4]={0,0,0,0};
	tmp[0]=cmd;
	tmp[1]=4;
	tmp[2]=0;
	tmp[3]=resp;
	HAL_UART_Transmit_DMA(&huart1, tmp, 4);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
	TIM_TypeDef* tim = htim->Instance;

	//USART1 timeout timer
	if(tim==TIM6)
	{
		HAL_TIM_Base_Stop_IT(&htim6);

		if(interface_comm==COMM_IDLE)
		{
			interface_comm=COMM_TOT; //set the TOT flag
		}
	}
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
	if(GPIO_Pin==TX_TRIG_Pin)
	{
		set_TP(TP1, 1);
		if(rx_state==RX_ACTIVE)
		{
			//set_TP(TP1, 1);
			rx_bsb_buff[rx_bsb_cnt]=trx_readreg(CHIP_RX, 0x2F7D);
			rx_bsb_cnt++;

			if(rx_bsb_cnt>=BSB_RX_BUFLEN)
			{
				rx_bsb_cnt=0;
				rx_bsb_buff_rdy=2;
			}
			else if(rx_bsb_cnt==BSB_RX_BUFLEN/2)
			{
				rx_bsb_buff_rdy=1;
			}
			//set_TP(TP1, 0);
		}

		if(tx_state==TX_ACTIVE)
		{
			//set_TP(TP2, 1);
			if(bsb_tx_cplt==0)
			{
				trx_writereg(CHIP_TX, 0x2F7E, tx_bsb_buff[tx_bsb_cnt%BSB_TX_BUFLEN]);
				tx_bsb_cnt++;

				if(tx_bsb_cnt>=tx_bsb_total_cnt)
					bsb_tx_cplt=1;
			}
			//set_TP(TP2, 0);
		}
		set_TP(TP1, 0);
	}
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	USART_TypeDef* iface = huart->Instance;

	if(iface==USART1)
	{
		/*rx_bc++;
		//check frame's validity
		if(rx_bc>=3 && rx_bc==*((uint16_t*)&rxb[1]))
		{
			HAL_TIM_Base_Stop_IT(&htim6);
			rx_bc=0;
			interface_comm=COMM_RDY;
			return;
		}

		//handle normal comm conditions and overflow
		if(rx_bc<sizeof(rxb)) //all normal - proceed
		{
			HAL_UART_Receive_IT(&huart1, (uint8_t*)&rxb[rx_bc], 1);
			//reset timeout timer
			TIM6->CNT=0;
			FIX_TIMER_TRIGGER(&htim6);
			HAL_TIM_Base_Start_IT(&htim6);
		}
		else //overflow
		{
			interface_comm=COMM_OVF; //set overflow flag, shouldn't normally happen
		}*/
		if(interface_comm==COMM_NEXT)
		{
			HAL_UART_Receive_DMA(&huart1, (uint8_t*)rxb, 3);
			interface_comm=COMM_RDY;
		}
		else if(interface_comm==COMM_IDLE)
		{
			if((rxb[0]==CMD_PING || rxb[0]>=CMD_GET_IDENT) && rxb[1]==3)
			{
				HAL_UART_Receive_DMA(&huart1, (uint8_t*)rxb, 3);
				interface_comm=COMM_RDY;
			}
			else
			{
				HAL_UART_Receive_DMA(&huart1, (uint8_t*)&rxb[3], *((uint16_t*)&rxb[1])-3);
				interface_comm=COMM_NEXT;
			}
		}
	}
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_DAC_Init();
  MX_SPI1_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  MX_TIM6_Init();
  /* USER CODE BEGIN 2 */
  //enable FPU
  #if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
	SCB->CPACR |= ((3UL << 10*2)|(3UL << 11*2));  /* set CP10 and CP11 Full Access */
  #endif

  //default settings
  set_rf_pwr_setpoint(0);
  set_dac_ch2(0); //can be used for debugging
  rf_pa_en(0);
  set_CS(CHIP_RX, 1);
  set_CS(CHIP_TX, 1);
  set_TP(TP1, 0);
  set_TP(TP2, 0);

  HAL_Delay(100);
  trx_writecmd(CHIP_RX, STR_SRES);
  trx_writecmd(CHIP_TX, STR_IDLE);
  dbg_print(0, TERM_CLR); //clear console and print out ident string
  dbg_print(TERM_GREEN, IDENT_STR); dbg_print(0,  "\n");

  HAL_Delay(10);
  detect_ic(trx_data[CHIP_RX].name, trx_data[CHIP_TX].name);
  dbg_print(0, "RX IC: %s\nTX IC: %s\n", trx_data[CHIP_RX].name, trx_data[CHIP_TX].name);

  trx_data[CHIP_RX].frequency=433475000;				//default
  trx_data[CHIP_TX].frequency=435000000;
  trx_data[CHIP_RX].fcorr=0;
  trx_data[CHIP_TX].fcorr=0;
  trx_data[CHIP_TX].pwr=63;								//3 to 63
  tx_dbm=30.00f;										//30dBm (1W) default
  alc_set=dbm_to_alc(tx_dbm);							//convert to DAC value

  dbg_print(0, "Starting TRX config...");
  config_rf(CHIP_RX, trx_data[CHIP_RX]);
  config_rf(CHIP_TX, trx_data[CHIP_TX]);
  trx_writereg(CHIP_TX, 0x2F7E, 0); //zero frequency deviation at TX idle
  dbg_print(TERM_GREEN, " done\n");

  HAL_Delay(50);
  trx_writecmd(CHIP_RX, STR_SCAL);
  trx_writecmd(CHIP_TX, STR_SCAL);

  HAL_Delay(50);
  trx_data[CHIP_RX].pll_locked = ((trx_readreg(CHIP_RX, 0x2F8D)^0x80)&0x81)==0x81; //FSCAL_CTRL=1 and FSCAL_CTRL_NOT_USED=0
  trx_data[CHIP_TX].pll_locked = ((trx_readreg(CHIP_TX, 0x2F8D)^0x80)&0x81)==0x81;
  dbg_print(0, "RX PLL");
  trx_data[CHIP_RX].pll_locked ? dbg_print(TERM_GREEN, " locked\n") : dbg_print(TERM_RED, " unlocked\n");
  dbg_print(0, "TX PLL");
  trx_data[CHIP_TX].pll_locked ? dbg_print(TERM_GREEN, " locked\n") : dbg_print(TERM_RED, " unlocked\n");

  if(!trx_data[CHIP_RX].pll_locked)
  {
	  dbg_print(TERM_RED, "ERROR %d: %s\n", ERR_RX_PLL, errstrings[ERR_RX_PLL]);
	  dev_err|=(1UL<<ERR_RX_PLL);
  }
  if(!trx_data[CHIP_TX].pll_locked)
  {
	  dbg_print(TERM_RED, "ERROR %d: %s\n", ERR_TX_PLL, errstrings[ERR_TX_PLL]);
	  dev_err|=(1UL<<ERR_TX_PLL);
  }
  if(strstr((char*)trx_data[CHIP_RX].name, "unknown")!=NULL)
  {
  	  dbg_print(TERM_RED, "ERROR %d: %s\n", ERR_RX_SPI, errstrings[ERR_RX_SPI]);
  	  dev_err|=(1UL<<ERR_RX_SPI);
  }
  if(strstr((char*)trx_data[CHIP_TX].name, "unknown")!=NULL)
  {
  	  dbg_print(TERM_RED, "ERROR %d: %s\n", ERR_TX_SPI, errstrings[ERR_TX_SPI]);
  	  dev_err|=(1UL<<ERR_TX_SPI);
  }

  trx_writecmd(CHIP_RX, STR_SRX);
  trx_writecmd(CHIP_TX, STR_STX);

  //dbg_print(TERM_YELLOW, "[DBG] TX status %01X\n", trx_readreg(CHIP_TX, STR_SNOP)>>4);
  //dbg_print(TERM_YELLOW, "[DBG] UART1 BRR: %08X\n", huart1.Instance->BRR);

  //enable interface comms over UART1
  //memset((uint8_t*)rxb, 0, sizeof(rxb));
  HAL_UART_Receive_DMA(&huart1, (uint8_t*)rxb, 3);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  /*while(0) //PA test
  {
	  float fwd=0.0, ref=0.0, temp=0.0, swr=0.0;

	  set_rf_pwr_setpoint(0);
	  HAL_Delay(1000);
	  //get_rf_pwr_dbm(&fwd, &ref);
	  //swr=calc_swr(fwd, ref);
	  //dbg_print(0, "FWD=%2.1fdBm REF=%2.1fdBm, SWR=%-.--f\n", fwd, ref);

	  set_rf_pwr_setpoint(2500);
	  HAL_Delay(5000);
	  get_adc_vals(&fwd, &ref, &temp);
	  swr=calc_swr(fwd, ref);
	  if(fwd>=27.0)
		  dbg_print(0, "FWD=%2.1fdBm REF=%2.1fdBm SWR=%2.2f T=%3.1f\n", fwd, ref, swr, temp);
	  else
		  dbg_print(0, "FWD=--.-dBm REF=--.-fdBm SWR=--.- T=%3.1f\n", temp);
  }

  while(0) //PA test 2
  {
	  trx_writecmd(CHIP_TX, STR_IDLE);
	  HAL_Delay(10);
	  trx_data[CHIP_TX].frequency=438812500;
	  //trx_data[CHIP_TX].pwr=63;
	  config_rf(CHIP_TX, trx_data[CHIP_TX]);
	  HAL_Delay(10);
	  trx_writecmd(CHIP_TX, STR_STX);
	  //set_rf_pwr_setpoint(1200); //14.35dBm
	  //set_rf_pwr_setpoint(1500); //21.85-
	  //set_rf_pwr_setpoint(1700); //26
	  //set_rf_pwr_setpoint(2000); //33.6-
	  //set_rf_pwr_setpoint(2200); //37.6
	  //set_rf_pwr_setpoint(2300); //39.8
	  //set_rf_pwr_setpoint(2500); //45.15-
	  //set_rf_pwr_setpoint(2600); //46.1
	  //set_rf_pwr_setpoint(dbm_to_alc(37));

	  rf_pa_en(1);

	  while(1)
	  {
		  //trx_data[CHIP_TX].pwr=63;
		  //trx_writereg(CHIP_TX, 0x002B, trx_data[CHIP_TX].pwr);
		  HAL_Delay(1000);

		  //trx_data[CHIP_TX].pwr=3;
		  //trx_writereg(CHIP_TX, 0x002B, trx_data[CHIP_TX].pwr);
		  HAL_Delay(1000);
	  }
  }

  while(0) //temp test
  {
	  float temp=0.0;

	  get_adc_vals(NULL, NULL, &temp);
	  dbg_print(0, "T=%3.1f\n", temp);
	  HAL_Delay(500);
  }

  while(0) //RX test
  {
	  uint8_t val=trx_readreg(CHIP_RX, 0x2F7D);
	  //HAL_UART_Transmit(&huart3, &val, 1, 10);
	  set_dac_ch2((int8_t)val*31+2048);
  }

  while(0) //CC1200 TX output test
  {
	  trx_data[CHIP_TX].fcorr=roundf(-4000.0f/FCORR_STEP);
	  trx_writereg(CHIP_TX, 0x2F0A, (uint16_t)trx_data[CHIP_TX].fcorr>>8);
	  trx_writereg(CHIP_TX, 0x2F0B, (uint16_t)trx_data[CHIP_TX].fcorr&0xFF);

	  while(1)
	  {
		  trx_writecmd(CHIP_TX, STR_STX);
		  set_rf_pwr_setpoint(dbm_to_alc(30));
		  rf_pa_en(1);
		  HAL_Delay(1000);

		  set_rf_pwr_setpoint(0);
		  rf_pa_en(0);
		  trx_writecmd(CHIP_TX, STR_IDLE);
		  HAL_Delay(1000);
	  }
  }

  while(0) //CC1200 PLL test
  {
	  set_rf_pwr_setpoint(dbm_to_alc(47));

	  while(1)
	  {
		  //for(uint32_t f=430e6; f<=440e6; f+=1e6)
		  for(uint32_t f=435e6; f<=440e6; )
		  {
			  rf_pa_en(0);
			  uint32_t freq_word=roundf((float)f/5000000.0*((uint32_t)1<<16));
			  trx_writecmd(CHIP_TX, STR_IDLE);
			  trx_writereg(CHIP_TX, 0x2F0C, (freq_word>>16)&0xFF);
			  trx_writereg(CHIP_TX, 0x2F0D, (freq_word>>8)&0xFF);
			  trx_writereg(CHIP_TX, 0x2F0E, freq_word&0xFF);
			  HAL_Delay(10);
			  trx_writecmd(CHIP_TX, STR_STX);
			  HAL_Delay(10);
			  rf_pa_en(1);
			  //dbg_print(0, "f=%ld sta=%d\n", f, trx_readreg(CHIP_TX, STR_SNOP)>>4);
			  HAL_Delay(1000);
			  //for(int8_t i=-63; i<=63; i++)
			  while(1)
			  {
				  //trx_writereg(CHIP_TX, 0x2F7E, 256-63);
				  //HAL_Delay(10);
				  //trx_writereg(CHIP_TX, 0x2F7E, 63);
				  //HAL_Delay(10);
			  }
		  }
	  }
  }*/

  while(1)
  {
	  if(interface_comm==COMM_RDY) //if a valid interface frame is detected
	  {
		  //dbg_print(0, "type 0x%02X\tlen %02d\n", rxb[0], rxb[1]);

		  uint32_t freq;
		  static uint8_t ident[128];
		  static uint8_t resp[15]; //response buffer

		  switch(rxb[0])
		  {
		  	  //PING
		  	  case CMD_PING:
		  		  dbg_print(0, "[INTRFC_CMD] PING\n");
		  		  resp[0]=CMD_PING;
		  		  *((uint16_t*)&resp[1])=7;
		  		  memcpy(&resp[3], (uint8_t*)&dev_err, sizeof(uint32_t));
		  		  HAL_UART_Transmit_DMA(&huart1, resp, 7); //error code, 0 for OK
		  	  break;

		  	  //SET commands
		  	  case CMD_SET_RX_FREQ:
		  		  memcpy((uint8_t*)&freq, (uint8_t*)&rxb[3], sizeof(uint32_t)); //we take 4 least significant bytes
		  		  if(freq>=420e6 && freq<=450e6)
		  		  {
		  			  dbg_print(0, "[INTRFC_CMD] RX %ld Hz\n", freq);
		  			  trx_writecmd(CHIP_RX, STR_IDLE);
		  			  HAL_Delay(10);
		  			  //reconfig RX
		  			  trx_data[CHIP_RX].frequency=freq;
		  			  uint32_t freq_word=roundf((float)freq/5000000.0*((uint32_t)1<<16));
		  			  trx_writereg(CHIP_RX, 0x2F0C, (freq_word>>16)&0xFF);
		  			  trx_writereg(CHIP_RX, 0x2F0D, (freq_word>>8)&0xFF);
		  			  trx_writereg(CHIP_RX, 0x2F0E, freq_word&0xFF);
		  			  trx_writecmd(CHIP_RX, STR_SRX);
		  			  HAL_Delay(20);
		  			  if(trx_readreg(CHIP_RX, STR_SNOP)>>4==STATE_RX) //PLL locked and state=RX?
		  				  interface_resp(CMD_SET_RX_FREQ, ERR_OK); //OK
		  			  else
		  				interface_resp(CMD_SET_RX_FREQ, ERR_RX_PLL); //PLL lock error
		  		  }
		  		  else
		  		  {
		  			  dbg_print(TERM_YELLOW, "[INTRFC_CMD] requested RX frequency of %ld Hz is out of range\n", freq);
		  			  interface_resp(CMD_SET_RX_FREQ, ERR_RANGE); //ERR
		  		  }
		  	  break;

		  	  case CMD_SET_TX_FREQ:
		  		  memcpy((uint8_t*)&freq, (uint8_t*)&rxb[3], sizeof(uint32_t)); //we take 4 least significant bytes
		  		  if(freq>=420e6 && freq<=450e6)
		  		  {
		  			  dbg_print(0, "[INTRFC_CMD] TX %ld Hz\n", freq);
		  			  trx_writecmd(CHIP_TX, STR_IDLE);
		  			  HAL_Delay(10);
		  			  //reconfig TX
		  			  trx_data[CHIP_TX].frequency=freq;
		  			  uint32_t freq_word=roundf((float)freq/5000000.0*((uint32_t)1<<16));
		  			  trx_writereg(CHIP_TX, 0x2F0C, (freq_word>>16)&0xFF);
		  			  trx_writereg(CHIP_TX, 0x2F0D, (freq_word>>8)&0xFF);
		  			  trx_writereg(CHIP_TX, 0x2F0E, freq_word&0xFF);
		  			  trx_writecmd(CHIP_TX, STR_STX);
		  			  HAL_Delay(20);
		  			  if(trx_readreg(CHIP_TX, STR_SNOP)>>4==STATE_TX) //PLL locked and state=TX?
		  				  interface_resp(CMD_SET_TX_FREQ, ERR_OK); //OK
		  			  else
		  				  interface_resp(CMD_SET_TX_FREQ, ERR_TX_PLL); //PLL lock error
		  		  }
		  		  else
		  		  {
		  			  dbg_print(TERM_YELLOW, "[INTRFC_CMD] requested TX frequency of %ld Hz is out of range\n", freq);
		  			  interface_resp(CMD_SET_TX_FREQ, ERR_RANGE); //ERR
		  		  }
			  break;

		  	  case CMD_SET_TX_POWER:
				  if(rxb[3]*0.25f>=30.0f && rxb[3]*0.25f<=47.75) //about 1 to 60W out
				  {
					  tx_dbm=rxb[3]*0.25f;
					  dbg_print(0, "[INTRFC_CMD] TX PWR %2.2f dBm\n", tx_dbm);
					  alc_set=dbm_to_alc(tx_dbm);
					  interface_resp(CMD_SET_TX_POWER, ERR_OK); //OK
				  }
				  else
				  {
					  //no change, return error code
					  dbg_print(TERM_YELLOW, "[INTRFC_CMD] requested output power is out of range\n");
					  interface_resp(CMD_SET_TX_POWER, ERR_RANGE); //ERR
				  }
			  break;

		  	  case CMD_SET_RX_FREQ_CORR:
		  		  trx_data[CHIP_RX].fcorr=roundf((trx_data[CHIP_RX].frequency/1.0e6f)*(*((float*)&rxb[3]))/FCORR_STEP);
		  		  trx_writereg(CHIP_RX, 0x2F0A, (uint16_t)trx_data[CHIP_RX].fcorr>>8);
		  		  trx_writereg(CHIP_RX, 0x2F0B, (uint16_t)trx_data[CHIP_RX].fcorr&0xFF);
		  		  dbg_print(0, "[INTRFC_CMD] RX frequency correction: %3.1f ppm (%d)\n", *((float*)&rxb[3]), trx_data[CHIP_RX].fcorr);
		  		  interface_resp(CMD_SET_RX_FREQ_CORR, ERR_OK); //OK
			  break;

		  	  case CMD_SET_TX_FREQ_CORR:
		  		  trx_data[CHIP_TX].fcorr=roundf((trx_data[CHIP_TX].frequency/1.0e6f)*(*((float*)&rxb[3]))/FCORR_STEP);
		  		  trx_writereg(CHIP_TX, 0x2F0A, (uint16_t)trx_data[CHIP_TX].fcorr>>8);
		  		  trx_writereg(CHIP_TX, 0x2F0B, (uint16_t)trx_data[CHIP_TX].fcorr&0xFF);
		  		  dbg_print(0, "[INTRFC_CMD] TX frequency correction: %3.1f ppm (%d)\n", *((float*)&rxb[3]), trx_data[CHIP_TX].fcorr);
		  		  interface_resp(CMD_SET_TX_FREQ_CORR, ERR_OK); //OK
			  break;

		  	  case CMD_SET_AFC:
		  		  dbg_print(0, "[INTRFC_CMD] RX AFC");
		  		  if(rxb[3])
		  		  {
		  			  trx_writereg(CHIP_RX, 0x2F01, 0x22);
		  			  dbg_print(TERM_GREEN, " enabled\n");
		  		  }
		  		  else
		  		  {
		  			  trx_writereg(CHIP_RX, 0x2F01, 0x02);
		  			  dbg_print(TERM_GREEN, " disabled\n");
		  		  }
		  		  interface_resp(CMD_SET_AFC, ERR_OK); //OK
			  break;

		  	  case CMD_SET_RX:
		  		  if(rxb[3]) //start
		  		  {
		  			  if(rx_state==RX_IDLE && dev_err==ERR_OK)
		  			  {
		  				  rx_bsb_cnt=0;
		  				  memset((uint8_t*)rx_bsb_buff, 0, BSB_RX_BUFLEN);
		  				  dbg_print(0, "[INTRFC_CMD] RX start\n");
		  				  rx_state=RX_ACTIVE;
		  			  }
		  			  else
		  			  {
		  				  interface_resp(CMD_SET_RX, dev_err);
		  			  }
		  		  }
		  		  else //stop
		  		  {
		  			  rx_state=RX_IDLE;
		  			  rx_bsb_buff_rdy=0;
		  			  //give it some time to finish pending transfers
		  			  HAL_Delay(40);
		  			  interface_resp(CMD_SET_RX, 0); //OK
		  			  dbg_print(0, "[INTRFC_CMD] RX stop\n");
		  		  }
			  break;

		  	  case CMD_STREAM_DATA:
		  		  //dbg_print(0, "[INTRFC_CMD] Stream data received (%d bytes)\n", *((uint16_t*)&rxb[1]));
		  		  uint32_t len=*((uint16_t*)&rxb[1])-3;
		  		  //if((tx_bsb_total_cnt%BSB_TX_BUFLEN)+len<=BSB_TX_BUFLEN)
		  			  memcpy((uint8_t*)&tx_bsb_buff[tx_bsb_total_cnt%BSB_TX_BUFLEN], (uint8_t*)&rxb[3], len);
		  		  //else
		  		  //{
		  			  //uint32_t left=(tx_bsb_total_cnt%BSB_TX_BUFLEN)+len-BSB_TX_BUFLEN;
		  			  //memcpy((uint8_t*)&tx_bsb_buff[tx_bsb_total_cnt%BSB_TX_BUFLEN], (uint8_t*)&rxb[3], len-left);
		  			  //memcpy((uint8_t*)tx_bsb_buff, (uint8_t*)&rxb[3+len-left], left);
		  		  //}
		  		  tx_bsb_total_cnt+=len;
		  		  if(tx_bsb_total_cnt>=BSB_TX_THRESH && tx_state==TX_IDLE)
		  		  {
		  			  //set tx CC1200 power to max and frequency deviation to zero
		  			  //trx_data[CHIP_TX].pwr=63;
		  			  //trx_writereg(CHIP_TX, 0x002B, 63);
		  			  //trx_writereg(CHIP_TX, 0x2F7E, 0);
		  			  set_rf_pwr_setpoint(alc_set);
		  			  rf_pa_en(1);
		  			  tx_state=TX_ACTIVE;
		  		  }
		  	  break;

		  	  //GET commands
		  	  case CMD_GET_IDENT:
		  		  dbg_print(0, "[INTRFC_CMD] GET_IDENT\n");
				  //reply with RRU's IDENT string
				  sprintf((char*)&ident[3], IDENT_STR);
				  ident[0]=CMD_GET_IDENT;
				  *((uint16_t*)&ident[1])=strlen((char*)IDENT_STR)+3; //total length
				  HAL_UART_Transmit_DMA(&huart1, ident, *((uint16_t*)&ident[1]));
			  break;

		  	  case CMD_GET_CAPS:
		  		  dbg_print(0, "[INTRFC_CMD] GET_CAPS\n");
				  //so far the RRU can do FM only, duplex
				  interface_resp(CMD_GET_CAPS, (1<<CAP_FM)|(1<<CAP_DUPLEX));
			  break;

		  	  case CMD_GET_RX_FREQ:
		  		  dbg_print(0, "[INTRFC_CMD] GET_RX_FREQ\n");
		  		  resp[0]=CMD_GET_RX_FREQ;
		  		  *((uint16_t*)&resp[1])=sizeof(uint64_t)+3;
		  		  memcpy(&resp[3], (uint8_t*)&trx_data[CHIP_RX].frequency, sizeof(uint32_t));
		  		  memset(&resp[7], 0, 4); //MSB to 0
		  		  HAL_UART_Transmit_DMA(&huart1, resp, *((uint16_t*)&resp[1]));
			  break;

		  	  case CMD_GET_TX_FREQ:
		  		  dbg_print(0, "[INTRFC_CMD] GET_TX_FREQ\n");
		  		  resp[0]=CMD_GET_TX_FREQ;
		  		  *((uint16_t*)&resp[1])=sizeof(uint64_t)+3;
		  		  memcpy(&resp[3], (uint8_t*)&trx_data[CHIP_TX].frequency, sizeof(uint32_t));
		  		  memset(&resp[7], 0, 4); //MSB to 0
		  		  HAL_UART_Transmit_DMA(&huart1, resp, *((uint16_t*)&resp[1]));
			  break;

		  	  case CMD_GET_MEAS:
		  		  //dbg_print(0, "[INTRFC_CMD] GET_TX_FREQ\n");
		  		  meas_req=1; //set request flag, process when ready (UART not busy)
			  break;

		  	  case 0x20:
		  		  ;//set_rf_pwr_setpoint(0);
			  break;

		  	  default:
		  		  ;
		  	  break;
		  }

		  interface_comm=COMM_IDLE;
	  }
	  /*else if(interface_comm==COMM_TOT || interface_comm==COMM_OVF)
	  {
		  HAL_TIM_Base_Stop_IT(&htim6);
		  HAL_UART_AbortReceive_IT(&huart1);
		  rx_bc=0;
		  HAL_UART_Receive_IT(&huart1, (uint8_t*)rxb, 1);
		  interface_comm=COMM_IDLE;
	  }*/

	  //baseband transmission complete
	  if(bsb_tx_cplt)
	  {
		  //debug
		  //set_TP(TP2, 1);

		  //interface_resp(CMD_SET_TX_START, 0); //OK - end of transmission

		  trx_writereg(CHIP_TX, 0x2F7E, 0); //zero frequency deviation at TX idle
		  set_rf_pwr_setpoint(0);
		  //HAL_Delay(50);
		  rf_pa_en(0);
		  //trx_data[CHIP_TX].pwr=3; //set tx cc2100 power to the minimum at TX idle
		  //trx_writereg(CHIP_TX, 0x002B, 3);

		  tx_state=TX_IDLE;
		  tx_bsb_cnt=0;
		  tx_bsb_total_cnt=0;
		  bsb_tx_cplt=0;

		  //dbg_print(0, "[SELF] TX -> end\n"); //takes time!

		  //debug
		  //set_TP(TP2, 0);
	  }

	  //rx baseband buffer ready to be sent over UART
	  if(rx_bsb_buff_rdy)
	  {
		  //make global?
		  volatile static uint8_t h_buff[3+BSB_RX_BUFLEN/2]={CMD_STREAM_DATA, (BSB_RX_BUFLEN/2+3)&0xFF, (BSB_RX_BUFLEN/2+3)>>8};

		  while(huart1.gState==HAL_UART_STATE_BUSY_TX); //wait while busy

		  if(rx_bsb_buff_rdy==1)
		  {
			  //debug
			  //set_TP(TP1, 1);

			  memcpy((uint8_t*)&h_buff[3], (uint8_t*)&rx_bsb_buff[0], BSB_RX_BUFLEN/2);
			  HAL_UART_Transmit_DMA(&huart1, (uint8_t*)h_buff, 3+BSB_RX_BUFLEN/2);

			  //debug
			  //set_TP(TP1, 0);
		  }
		  else //implicit rx_bsb_buff_rdy==2
		  {
			  //debug
			  //set_TP(TP2, 1);

			  memcpy((uint8_t*)&h_buff[3], (uint8_t*)&rx_bsb_buff[BSB_RX_BUFLEN/2], BSB_RX_BUFLEN/2);
			  HAL_UART_Transmit_DMA(&huart1, (uint8_t*)h_buff, 3+BSB_RX_BUFLEN/2);

			  //debug
			  //set_TP(TP2, 0);
		  }

		  rx_bsb_buff_rdy=0;
	  }

	  if(meas_req==1 && huart1.gState!=HAL_UART_STATE_BUSY_TX)
	  {
		  //process pending stuff
		  static uint8_t resp[6];
  		  resp[0]=CMD_GET_MEAS;
  		  *((uint16_t*)&resp[1])=3+3;
  		  resp[3]=0x01;
  		  resp[4]=0x02;
  		  resp[5]=0x03;
  		  HAL_UART_Transmit_DMA(&huart1, resp, *((uint16_t*)&resp[1]));
		  meas_req=0;
	  }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 6;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = ENABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 3;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SEQ_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_1;
  sConfig.Rank = 2;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_2;
  sConfig.Rank = 3;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief DAC Initialization Function
  * @param None
  * @retval None
  */
static void MX_DAC_Init(void)
{

  /* USER CODE BEGIN DAC_Init 0 */

  /* USER CODE END DAC_Init 0 */

  DAC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN DAC_Init 1 */

  /* USER CODE END DAC_Init 1 */

  /** DAC Initialization
  */
  hdac.Instance = DAC;
  if (HAL_DAC_Init(&hdac) != HAL_OK)
  {
    Error_Handler();
  }

  /** DAC channel OUT1 config
  */
  sConfig.DAC_Trigger = DAC_TRIGGER_SOFTWARE;
  sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
  if (HAL_DAC_ConfigChannel(&hdac, &sConfig, DAC_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }

  /** DAC channel OUT2 config
  */
  if (HAL_DAC_ConfigChannel(&hdac, &sConfig, DAC_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN DAC_Init 2 */

  /* USER CODE END DAC_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief TIM6 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM6_Init(void)
{

  /* USER CODE BEGIN TIM6_Init 0 */

  /* USER CODE END TIM6_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM6_Init 1 */

  /* USER CODE END TIM6_Init 1 */
  htim6.Instance = TIM6;
  htim6.Init.Prescaler = 10-1;
  htim6.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim6.Init.Period = 4200-1;
  htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim6) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim6, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM6_Init 2 */

  /* USER CODE END TIM6_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 460800;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 460800;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * Enable DMA controller clock
  * Configure DMA for memory to memory transfers
  *   hdma_memtomem_dma2_stream1
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* Configure DMA request hdma_memtomem_dma2_stream1 on DMA2_Stream1 */
  hdma_memtomem_dma2_stream1.Instance = DMA2_Stream1;
  hdma_memtomem_dma2_stream1.Init.Channel = DMA_CHANNEL_0;
  hdma_memtomem_dma2_stream1.Init.Direction = DMA_MEMORY_TO_MEMORY;
  hdma_memtomem_dma2_stream1.Init.PeriphInc = DMA_PINC_ENABLE;
  hdma_memtomem_dma2_stream1.Init.MemInc = DMA_MINC_ENABLE;
  hdma_memtomem_dma2_stream1.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_memtomem_dma2_stream1.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
  hdma_memtomem_dma2_stream1.Init.Mode = DMA_NORMAL;
  hdma_memtomem_dma2_stream1.Init.Priority = DMA_PRIORITY_LOW;
  hdma_memtomem_dma2_stream1.Init.FIFOMode = DMA_FIFOMODE_ENABLE;
  hdma_memtomem_dma2_stream1.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_FULL;
  hdma_memtomem_dma2_stream1.Init.MemBurst = DMA_MBURST_SINGLE;
  hdma_memtomem_dma2_stream1.Init.PeriphBurst = DMA_PBURST_SINGLE;
  if (HAL_DMA_Init(&hdma_memtomem_dma2_stream1) != HAL_OK)
  {
    Error_Handler( );
  }

  /* DMA interrupt init */
  /* DMA2_Stream0_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 3, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
  /* DMA2_Stream2_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream2_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream2_IRQn);
  /* DMA2_Stream7_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream7_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream7_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
/* USER CODE BEGIN MX_GPIO_Init_1 */
/* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, DBG_TP1_Pin|DBG_TP2_Pin|PA_EN_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, RX_nCS_Pin|TX_nCS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pins : DBG_TP1_Pin DBG_TP2_Pin RX_nCS_Pin TX_nCS_Pin
                           PA_EN_Pin */
  GPIO_InitStruct.Pin = DBG_TP1_Pin|DBG_TP2_Pin|RX_nCS_Pin|TX_nCS_Pin
                          |PA_EN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : TX_TRIG_Pin RX_TRIG_Pin */
  GPIO_InitStruct.Pin = TX_TRIG_Pin|RX_TRIG_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI9_5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

/* USER CODE BEGIN MX_GPIO_Init_2 */
  //HAL_NVIC_DisableIRQ(EXTI9_5_IRQn); //immediately disable it, as we don't need it right away
/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  DBG_TP2_GPIO_Port->BSRR=(uint32_t)DBG_TP2_Pin;
  while (1)
  {
	  ;
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
