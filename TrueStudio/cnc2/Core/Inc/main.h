/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2020 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under Ultimate Liberty license
  * SLA0044, the "License"; You may not use this file except in compliance with
  * the License. You may obtain a copy of the License at:
  *                             www.st.com/SLA0044
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"
#include "stm32h7xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LED0_Pin GPIO_PIN_0
#define LED0_GPIO_Port GPIOC
#define LED1_Pin GPIO_PIN_1
#define LED1_GPIO_Port GPIOC
#define RES0_Pin GPIO_PIN_2
#define RES0_GPIO_Port GPIOC
#define RES1_Pin GPIO_PIN_3
#define RES1_GPIO_Port GPIOC
#define RES2_Pin GPIO_PIN_4
#define RES2_GPIO_Port GPIOC
#define RES3_Pin GPIO_PIN_5
#define RES3_GPIO_Port GPIOC
#define OK_220V_Pin GPIO_PIN_0
#define OK_220V_GPIO_Port GPIOG
#define ALT_IRQ_Pin GPIO_PIN_1
#define ALT_IRQ_GPIO_Port GPIOG
#define KEY_INC_Pin GPIO_PIN_2
#define KEY_INC_GPIO_Port GPIOG
#define KEY_DEC_Pin GPIO_PIN_3
#define KEY_DEC_GPIO_Port GPIOG
#define KEY_DRUM_Pin GPIO_PIN_4
#define KEY_DRUM_GPIO_Port GPIOG
#define KEY_PUMP_Pin GPIO_PIN_5
#define KEY_PUMP_GPIO_Port GPIOG
#define KEY_WIRE_Pin GPIO_PIN_6
#define KEY_WIRE_GPIO_Port GPIOG
/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
