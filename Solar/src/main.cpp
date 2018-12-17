
/*
 *	IoT Solar Tracker
 *
 *	by Pavel Dounaev, Miikka Käkelä & Lauri Solin
 *
 */

#if defined (__USE_LPCOPEN)
#if defined(NO_BOARD_LIB)
#include "chip.h"
#else
#include "board.h"
#endif
#endif

#include "stdlib.h"
#include <string>
#include "MAXIM1249.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "DigitalIoPin.h"
#include <deque>

#define DEAD_ZONE_V 200
#define DEAD_ZONE_H 200

volatile uint32_t RIT_count;
SemaphoreHandle_t sbRIT;
bool move = false;

/*Creating DigitalIoPins*/
DigitalIoPin* STEP;
DigitalIoPin* DIR;
DigitalIoPin* h_pin1;
DigitalIoPin* h_pin2;

extern "C" { void RIT_IRQHandler(void)
{
	// This used to check if a context switch is required
	portBASE_TYPE xHigherPriorityWoken = pdFALSE;
	// Tell timer that we have processed the interrupt.
	// Timer then removes the IRQ until next match occurs
	Chip_RIT_ClearIntStatus(LPC_RITIMER);
	// clear IRQ flag

	if(RIT_count > 0) {
		RIT_count--;
		move = !(bool)move;
		STEP->write(move);
	}
	else {
		Chip_RIT_Disable(LPC_RITIMER);
		// disable timer
		// Give semaphore and set context switch flag if a higher priority task was woken up
		xSemaphoreGiveFromISR(sbRIT, &xHigherPriorityWoken);
	}
	// End the ISR and (possibly) do a context switch
	portEND_SWITCHING_ISR(xHigherPriorityWoken);
}
}

void RIT_start(int count, int us) {

	uint64_t cmp_value;

	// Determine approximate compare value based on clock rate and passed interval
	cmp_value = (uint64_t) Chip_Clock_GetSystemClockRate() * (uint64_t) us / 1000000;

	// disable timer during configuration
	Chip_RIT_Disable(LPC_RITIMER);
	RIT_count = count;
	// enable automatic clear on when compare value==timer value
	// this makes interrupts trigger periodically
	Chip_RIT_EnableCompClear(LPC_RITIMER);
	// reset the counter
	Chip_RIT_SetCounter(LPC_RITIMER, 0);
	Chip_RIT_SetCompareValue(LPC_RITIMER, cmp_value);
	// start counting
	Chip_RIT_Enable(LPC_RITIMER);
	// Enable the interrupt signal in NVIC (the interrupt controller)
	NVIC_EnableIRQ(RITIMER_IRQn);

	// wait for ISR to tell that we're done
	if(xSemaphoreTake(sbRIT, portMAX_DELAY) == pdTRUE) {
		// Disable the interrupt signal in NVIC (the interrupt controller)
		NVIC_DisableIRQ(RITIMER_IRQn);
	}
	else {
		// unexpected error
	}
}
/* Sets up system hardware */
static void prvSetupHardware(void){

	SystemCoreClockUpdate();
	Board_Init();

	// initialize RIT (= enable clocking etc.)
	Chip_RIT_Init(LPC_RITIMER);

	// set the priority level of the interrupt
	// The level must be equal or lower than the maximum priority specified in FreeRTOS config
	// Note that in a Cortex-M3 a higher number indicates lower interrupt priority
	NVIC_SetPriority( RITIMER_IRQn, 8 + 1 );

	/* Initial LED0 state is on */
	Board_LED_Set(0, true);
}

// calculates delay based on pps
int calc_delay(int pps){

	// 1s = 1000000us
	const int mil_us = 1000000;

	return (mil_us / 2) / pps;
}


/*Calculates the running average*/
uint16_t getRunAverage(std::deque<uint16_t>ldr_dq, uint16_t ldr_val){

	std::deque<uint16_t>::iterator it;
	const int BUFFER_SIZE = 50;
	int sum = 0;

	if(ldr_dq.size() < BUFFER_SIZE){

		ldr_dq.push_back(ldr_val);
	}
	else{
		ldr_dq.pop_front();
		ldr_dq.push_back(ldr_val);
	}

	for(auto it = ldr_dq.begin(); it != ldr_dq.end(); ++it){
		sum += *it;
	}

	return sum / ldr_dq.size();
}

/*Task that reads ADC values*/
static void vTask_ADC(void* pvPrameters){

	SPI spi;							/*SPI communication class*/
	MAXIM1249 maxim(&spi);				/*ADC(MAXIM 1249) class*/

	std::deque<uint16_t>north_ldr_dq;
	std::deque<uint16_t>south_ldr_dq;
	std::deque<uint16_t>west_ldr_dq;
	std::deque<uint16_t>east_ldr_dq;

	uint16_t north_ldr = 0;
	uint16_t south_ldr = 0;
	uint16_t west_ldr = 0;
	uint16_t east_ldr = 0;

	char uart_buff[30] = {0};			/*contains ldr values(used for debugging)*/
	const int PPS = 400;				/*pulses per second - "speed of the motor"*/
	const int steps = 200;

	while(1){

		north_ldr = getRunAverage(north_ldr_dq, maxim.readChannel(0));
		east_ldr = getRunAverage(east_ldr_dq, maxim.readChannel(1));
		west_ldr = getRunAverage(south_ldr_dq, maxim.readChannel(2));
		south_ldr = getRunAverage(west_ldr_dq,maxim.readChannel(3));


		/*calculate steps if difference between ldrs exceeds DIFF_LIM*/
		if(abs(north_ldr - south_ldr) > DEAD_ZONE_V){

			/*if north_ldr gets more light than south_ldr move down*/
			if(north_ldr > south_ldr){
				DIR->write(false);
			}
			else{
				DIR->write(true);
			}
			RIT_start(steps, calc_delay(PPS));
		}
		if(abs(east_ldr - west_ldr) > DEAD_ZONE_H){

			if(east_ldr > west_ldr){

				h_pin1->write(true);
				h_pin2->write(false);
			}
			else{

				h_pin1->write(false);
				h_pin2->write(true);
			}
		}
		else{
			h_pin1->write(false);
			h_pin2->write(false);
		}

		 sprintf(uart_buff,"N: %d	S: %d	W: %d	E:%d\n\r", north_ldr, south_ldr, west_ldr, east_ldr);
		 Board_UARTPutSTR(uart_buff);
		 Board_UARTPutSTR("\n\r");

		vTaskDelay(10);
	}
}

int main(){

	prvSetupHardware();

	sbRIT = xSemaphoreCreateBinary();

	STEP = new DigitalIoPin(0, 24, false, true, false);
	DIR = new DigitalIoPin(1, 0, false, true, false);

	h_pin1 = new DigitalIoPin(0, 22, false, true, false);
	h_pin2 = new DigitalIoPin(0, 23, false, true, false);

	/*ADC Task*/
	xTaskCreate(vTask_ADC, "vTask_ADC",
			configMINIMAL_STACK_SIZE + 256, NULL, (tskIDLE_PRIORITY + 1UL),
			(TaskHandle_t *) NULL);

	vTaskStartScheduler();

	return 0;
}
