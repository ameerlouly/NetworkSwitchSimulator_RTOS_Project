/*
 * This file is part of the µOS++ distribution.
 *   (https://github.com/micro-os-plus)
 * Copyright (c) 2014 Liviu Ionescu.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

// ----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include "diag/trace.h"
#include <stdint.h>
#include <time.h> // used by rand()
#include <assert.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include "semphr.h"	// To use semaphores

#define CCM_RAM __attribute__((section(".ccmram")))

// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
//
// Semihosting STM32F4 empty sample (trace via DEBUG).
//
// Trace support is enabled by adding the TRACE macro definition.
// By default the trace messages are forwarded to the DEBUG output,
// but can be rerouted to any device or completely suppressed, by
// changing the definitions required in system/src/diag/trace-impl.c
// (currently OS_USE_TRACE_ITM, OS_USE_TRACE_SEMIHOSTING_DEBUG/_STDOUT).
//

// ----- main() ---------------------------------------------------------------

// Sample pragmas to cope with warnings. Please note the related line at
// the end of this function, used to pop the compiler diagnostics status.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#pragma GCC diagnostic ignored "-Wreturn-type"

/** Macros ********************************************************************/

#define ACK_PACKET 				(uint32_t)2001
#define NUM_OF_TRIES 			(uint8_t)4
#define PACKET_SENT				(uint8_t)1
#define MAX_NUM_OF_PACKETS		(uint16_t)2000

/** End of Macros *************************************************************/

/** Type Declarations	*******************************************************/

typedef uint32_t SequenceNumber_t;
typedef uint32_t NumOfError_t;
typedef uint16_t NumOfPackets_t;
typedef uint8_t  Payload_t;

typedef struct {
	QueueHandle_t sender;
	QueueHandle_t reciever;

	SequenceNumber_t sequenceNumber;

	uint16_t length;
} header_t;

typedef struct {
	header_t header;
	Payload_t* data;
} packet;

typedef struct {
	TaskHandle_t* CurrentTask;
	QueueHandle_t CurrentQueue;
	TimerHandle_t CurrentTimer;
	TimerHandle_t ACKToutTimer;
	SemaphoreHandle_t SendDataSema;
} NodeType_t;

/** End of Type Declarations ***************************************************/

/** Project Parameters *********************************************************/

#define T1 					( pdMS_TO_TICKS(200) )
#define T2 					( pdMS_TO_TICKS(500) )
#define Tout 				( pdMS_TO_TICKS(200) )
#define Pdrop 				( (double)0.01 ) 
#define Tdelay				( pdMS_TO_TICKS(200) )
static const uint32_t	L1 = 1000;
static const uint32_t	L2 = 2000;

/** End of Project Parameters *************************************************/

/** Timer CallBacks Prototypes ************************************************/

static void vSenderTimerCallBack(TimerHandle_t xTimer);
static void vRouterDelayCallBack(TimerHandle_t xTimer);
//void vACKToutCallBack(TimerHandle_t xTimer);

/** End of Timer CallBacks Prototypes *****************************************/

/** Timer Handles *************************************************************/

TimerHandle_t tNode1_Sender = NULL;
TimerHandle_t tNode2_Sender = NULL;

TimerHandle_t tRouterDelay = NULL;

//TimerHandle_t	tNode1_ACKTout = NULL;

/** End of Timer Handles ******************************************************/

/** Semaphore Handles *********************************************************/

SemaphoreHandle_t Node1SendData;
SemaphoreHandle_t Node2SendData;

SemaphoreHandle_t RouterTransmit;

SemaphoreHandle_t PrepareACK;

SemaphoreHandle_t GeneratePacket;

SemaphoreHandle_t StopTransmission;

/** End of Semaphore Handles **************************************************/

/** Task Handles **************************************************************/

TaskHandle_t Node1Task = NULL;
TaskHandle_t Node2Task = NULL;
TaskHandle_t Node3Task = NULL;
TaskHandle_t Node4Task = NULL;
TaskHandle_t RouterTask = NULL;

/** End of Task Handles *******************************************************/

/** Queue Handles *************************************************************/

QueueHandle_t Node1Queue;
QueueHandle_t Node2Queue;
QueueHandle_t Node3Queue;
QueueHandle_t Node4Queue;
QueueHandle_t RouterQueue;

/** End of Queue Handles ******************************************************/

/** Task Functions Prototypes *************************************************/

void vRecieverTask(void *pvParameters);	//	Receiver Task code
void vSenderTask(void *pvParameters);	//	Sender Task code
void vRouterTask(void* pvParameters);	//	Router Task code

/** End of Function Prototypes ************************************************/

/** Helpful Functions Definitions *********************************************/

	/*	Random Number Generator	*/
uint32_t RandomNum(uint32_t min, uint32_t max)
{
	return ((rand() % (max - min + 1)) + min);
}

	//! Something Wrong with this function causing random Data and queues on occasions (Commented out for now)
	//! Packet Generation Happens inside task at the time being
//packet* PacketGenerator(QueueHandle_t CurrentQueue)
//{
//	packet* packet = malloc(sizeof(packet));	// pointer to packet Storing the Packet to be Sent
//
//	packet->sender = CurrentQueue;	// Define sender as the current task
//
//	switch(RandomNum(3, 4))	// Define receiver as a random node between 3 or 4
//	{
//	case 3:
//		packet->reciever = Node3Queue;
//		break;
//
//	case 4:
//		packet->reciever = Node4Queue;
//		break;
//
//	default:
//		break;
//	}
//
//	packet->data = RandomNum(L1, L2);
//
//	return packet;
//}

uint8_t QueueHandleToNum(QueueHandle_t Queue)
{
	if(Queue == Node1Queue)
		return 1;
	else if(Queue == Node2Queue)
		return 2;
	else if(Queue == Node3Queue)
		return 3;
	else if(Queue == Node4Queue)
		return 4;
	else if(Queue == RouterQueue)
		return 0;
	else
		return 255;
}

BaseType_t checkProb(double prob)
{
	if(prob <= 0.0 || prob >= 1.0)
	{
		return pdFALSE;
	}

	double random_value = (double)rand() / RAND_MAX;

	if(random_value < prob)
	{
		return pdTRUE;
	}
	else
	{
		return pdFALSE;
	}
}

//** End of Helpful Functions Definitions **************************************/

int main(int argc, char* argv[])
{
	// Add your code here.
	srand(time(NULL));	// Initialize RandomNum
	BaseType_t status; // Storing the status of the program

	/** Creating Timers **/
	tNode1_Sender = xTimerCreate("Node 1 Sender",
								T1,
								pdFALSE,
								(void*)1,
								vSenderTimerCallBack);
	tNode2_Sender = xTimerCreate("Node 2 Sender",
								T1,
								pdFALSE,
								(void*)2,
								vSenderTimerCallBack);
	tRouterDelay = xTimerCreate("Router Delay Timer",
								Tdelay,
								pdFALSE,
								(void*)1,
								vRouterDelayCallBack);
//	tNode1_ACKTout = xTimerCreate("Node 1 Tou",
//								 Tout,
//								 pdFALSE,
//								 (void*)1,
//								 vACKToutCallBack);
//	xTimerStart(tNode1_Sender, 0);
//	if(tNode1_Sender == NULL)
//	{
//		trace_puts("Error Creating Timer");
//		return 0;
//	}

	/** Creating Queues **/
	Node1Queue = xQueueCreate(5, sizeof(packet));
	Node2Queue = xQueueCreate(5, sizeof(packet));
	Node3Queue = xQueueCreate(5, sizeof(packet));
	Node4Queue = xQueueCreate(5, sizeof(packet));
	RouterQueue = xQueueCreate(10, sizeof(packet));

	/** Creating Semaphores **/
	Node1SendData = xSemaphoreCreateBinary();
	xSemaphoreTake(Node1SendData, 0);

	Node2SendData = xSemaphoreCreateBinary();
	xSemaphoreTake(Node2SendData, 0);

	PrepareACK = xSemaphoreCreateMutex();
	xSemaphoreGive(PrepareACK);

	RouterTransmit = xSemaphoreCreateBinary();
	xSemaphoreTake(RouterTransmit, 0);

	GeneratePacket = xSemaphoreCreateMutex();
	xSemaphoreGive(GeneratePacket);

	StopTransmission = xSemaphoreCreateMutex();
	xSemaphoreTake(StopTransmission, 0);

	/** Node Types Definitions ****************************************************/

//					   {Task Handle, Queue Handle, SenderTimer, ACKTout Timer, SendData Semaphore}
	NodeType_t Node1 = {&Node1Task, Node1Queue, tNode1_Sender, NULL,Node1SendData};
	NodeType_t Node2 = {&Node2Task, Node2Queue, tNode2_Sender, NULL, Node2SendData};
	NodeType_t Node3 = {&Node3Task, Node3Queue, NULL, NULL, NULL};
	NodeType_t Node4 = {&Node4Task, Node4Queue, NULL, NULL, NULL};
	NodeType_t Router = {&RouterTask, RouterQueue, tRouterDelay, NULL, RouterTransmit};

	/** End of Node Types Definitions *********************************************/
	if(		Node1Queue != NULL &&
			Node2Queue != NULL &&
			Node3Queue != NULL &&
			Node4Queue != NULL &&
			RouterQueue != NULL)	// Check if Queue Creation was successful
	{
		// Creating Tasks
		status = xTaskCreate(vSenderTask, "Node 1", 1024, (void*)&Node1, 1, &Node1Task);
		status &= xTaskCreate(vSenderTask, "Node 2", 1024, (void*)&Node2, 1, &Node2Task);
		status &= xTaskCreate(vRecieverTask, "Node 3", 1024, (void*)&Node3, 2, &Node3Task);
		status &= xTaskCreate(vRecieverTask, "Node 4", 1024, (void*)&Node4, 2, &Node4Task);
		status &= xTaskCreate(vRouterTask, "Router", 2048, (void*)&Router, 3, &RouterTask);
		

		if(status == pdPASS)
		{
			trace_puts("Starting Scheduler\n");
			vTaskStartScheduler();
		}
		else
		{
			trace_puts("Error Creating Tasks");
			return 0;
		}
	}
	else
	{
		trace_puts("Error Creating Queues");
		return 0;
	}

	// Should never reach here
	return 0;
}

#pragma GCC diagnostic pop

// ----------------------------------------------------------------------------

//** Node Task Definitions *****************************************************/

void vSenderTask(void *pvParameters)
{
	trace_puts("Beginning of Sender Task");
	NodeType_t *CurrentNode = (NodeType_t*)pvParameters;	// Set Parameters

	packet* PacketToSend = NULL;
	uint16_t SequenceToNode3 = 0;
	uint16_t SequenceToNode4 = 0;

	uint8_t RecieverNum = 3;
//?		/** Used for ACK Part (Commented Out for Phase 1)	**/
//	packet* PacketRecieved = NULL; // Buffer to store recieved ACK Packets
//	BaseType_t status;

	if(CurrentNode->CurrentTask == Node1Task)
	{
		vTaskDelay(pdMS_TO_TICKS(20)); // Small delay so that both tasks dont start at the same time
	}

	while(1)
	{
		xTimerStart(CurrentNode->CurrentTimer, 0);
		xSemaphoreTake(CurrentNode->SendDataSema, portMAX_DELAY);	// Dont Start Sending Data until allowed; Sema = 0

			/* Generate and Send Packet when Semaphore is Taken */
//		PacketToSend = PacketGenerator(CurrentNode->CurrentQueue);	//! PacketGenerator Function causes a bug, commented out for now

		PacketToSend = malloc(sizeof(packet));
		if(PacketToSend == NULL)
		{
			// Failed to Generate Packet, Trying Again
			continue;
		}

		PacketToSend->header.sender = CurrentNode->CurrentQueue;


		RecieverNum = RandomNum(3, 4);

		switch(RecieverNum)
		{
		case 3:
			PacketToSend->header.reciever = Node3Queue;
			PacketToSend->header.sequenceNumber = ++SequenceToNode3;
			break;
		case 4:
			PacketToSend->header.reciever = Node4Queue;
			PacketToSend->header.sequenceNumber = ++SequenceToNode4;
			break;
		}

		PacketToSend->header.length = RandomNum(L1, L2);
		PacketToSend->data = calloc(PacketToSend->header.length - sizeof(header_t), sizeof(Payload_t));


		xQueueSend(RouterQueue, &PacketToSend, portMAX_DELAY);


//?				/** ACK Part (Commented Out for Phase 1)	**/
//		for(int i = 0; i < NUM_OF_TRIES; i++)
//		{
//			 status = xQueueReceive(CurrentNode->CurrentQueue, &PacketRecieved, Tout);
//			 if(status == pdPASS)
//			 {
//			 	trace_printf("***Node %d Received ACK***\n", QueueHandleToNum(CurrentNode->CurrentQueue));
//			 	break;
//			 }
//			 else
//			 {
//			 	trace_printf("Node %d Awaiting ACK\n", QueueHandleToNum(CurrentNode->CurrentQueue));
//			 	xQueueSend(RouterQueue, &PacketToSend, 0);
//			 }
//		}
//
//		 if(status == pdPASS)
//		 {
//		 	trace_puts("Packet Sent Successfully, Sending Next Packet!");
//		 }
//		 else
//		 {
//		 	trace_printf("Node %d Did not receive ACK\n", QueueHandleToNum(CurrentNode->CurrentQueue));
//		 	free(PacketToSend);
//		 }
//
//		 free(PacketRecieved);
	}
}

void vRecieverTask(void *pvParameters)
{
	trace_puts("Beginning of Receiver Task");
	NodeType_t *CurrentNode = (NodeType_t*)pvParameters;
	packet* PacketRecieved = NULL;		// Buffer to Store Received Packets
//	packet* PacketToSend = NULL;	// Buffer to Store ACKs

	static uint8_t Finished = 0;

	static NumOfError_t WrongPackets = 0;

	SequenceNumber_t previousSequence1 = 0;
	SequenceNumber_t previousSequence2 = 0;

	NumOfPackets_t totalReceived1 = 0;
	NumOfPackets_t totalReceived2 = 0;
	NumOfPackets_t totalLost1 = 0;
	NumOfPackets_t totalLost2 = 0;


	while(1)
	{
		xQueueReceive(CurrentNode->CurrentQueue, &PacketRecieved, portMAX_DELAY);
//		trace_printf("Received: %d from Node %d", PacketRecieved->data,
//												  QueueHandleToNum(PacketRecieved->sender));

		// Checks if the Received Packets are meant for the Current Node
		if(PacketRecieved->header.reciever != CurrentNode->CurrentQueue)
		{
			WrongPackets++;
			free(PacketRecieved);
		}
		else
		{
			trace_printf("\n\nNode %d: Received %d from %d No #%d\n", QueueHandleToNum(CurrentNode->CurrentQueue),
																  PacketRecieved->header.length,
																  QueueHandleToNum(PacketRecieved->header.sender),
																  PacketRecieved->header.sequenceNumber);

					/** Handle Sequence Numbers and Count Lost Packets**/
			switch(QueueHandleToNum(PacketRecieved->header.sender))
			{
			case 1:
				totalReceived1++;
				totalLost1 += PacketRecieved->header.sequenceNumber - previousSequence1 - 1;
				previousSequence1 = PacketRecieved->header.sequenceNumber;
				trace_printf("Received: %d, Lost: %d", totalReceived1, totalLost1);
				break;

			case 2:
				totalReceived2++;
				totalLost2 += PacketRecieved->header.sequenceNumber - previousSequence2 - 1;
				previousSequence2 = PacketRecieved->header.sequenceNumber;
				trace_printf("Received: %d, Lost: %d\n\n", totalReceived2, totalLost2);
				break;
			}


			free(PacketRecieved);

			// Suspend Current Task if it has received 2000 or more Packets
			if((totalReceived1 + totalLost1 + totalReceived2 + totalLost2) >= MAX_NUM_OF_PACKETS)
			{
				trace_printf("\n**Suspended Node %d...Printing Node Statistics....\n",
										QueueHandleToNum(CurrentNode->CurrentQueue));
				trace_printf("\nTotal Packets from 1: %d\n", totalReceived1 + totalLost1);
				trace_printf("Total Received from 1: %d\n", totalReceived1);
				trace_printf("Total Lost from 1: %d\n", totalLost1);
//				trace_printf("Lost \% %d", ((float)totalLost1/(totalReceived1 + totalLost1)) * 100);

				trace_printf("\nTotal Packets from 2: %d\n", totalReceived2 + totalLost2);
				trace_printf("Total Received from 2: %d\n", totalReceived2);
				trace_printf("Total Lost from 2: %d\n", totalLost2);
//				trace_printf("Lost \% %d", (uint32_t)((float)totalLost2 / (totalReceived2 + totalLost2)) * 100);

				if(Finished)
				{
					vTaskSuspendAll();
				}

//				vTaskSuspend(CurrentNode->CurrentTask);
				Finished = 1;
				xSemaphoreTake(StopTransmission, portMAX_DELAY); // Blocks Task
			}

//?			/**  ACK Part (Commented Out for Phase 1)	**/
//			PacketToSend = malloc(sizeof(packet));
//
//			trace_printf("Received %d from Node %d", PacketRecieved->data,
//													  QueueHandleToNum(PacketRecieved->sender));
//			PacketToSend->sender = PacketRecieved->reciever;
//			PacketToSend->reciever = PacketRecieved->sender;
//			PacketToSend->data = ACK_PACKET;
//			free(PacketRecieved);
//
//			xQueueSend(RouterQueue, &PacketToSend, 0); // Send ACK
		}
	}

}

void vRouterTask(void *pvParameters)
{
	NodeType_t *CurrentNode = (NodeType_t*)pvParameters;

	packet* PacketRecieved = NULL;	// Buffer to Process Received Packets

	while(1)
	{
		xQueueReceive(RouterQueue, &PacketRecieved, portMAX_DELAY);

		//! Router Received Data Printer
//		trace_printf("\nReceived Packet from %d to %d\n", QueueHandleToNum(PacketRecieved->sender),
//													   QueueHandleToNum(PacketRecieved->reciever));
//		if(PacketRecieved->data == ACK_PACKET)
//		{
//			trace_printf("Content: ACK\n");
//		}
//		else
//		{
//			trace_printf("Content: %d\n", PacketRecieved->data);
//		}

		if(checkProb(Pdrop) == pdTRUE)
		{
			free(PacketRecieved);
			trace_printf("\n\nRouter Dropped Packet...\n");
		}
		else
		{
//			vTaskDelay(Tdelay); // Delay Packet after making sure its forwarded //! Redacted, Using a Timer instead
			xTimerStart(CurrentNode->CurrentTimer, 0);
			xSemaphoreTake(CurrentNode->SendDataSema, portMAX_DELAY);

			if(xQueueSend(PacketRecieved->header.reciever, &PacketRecieved, 0) != pdPASS)
			{
				trace_printf("\n\nReceiver Queue Full, Router Dropped Packet...\n");
				free(PacketRecieved);
			}
		}
	}
}

/** End of Node Task Definitions **********************************************/

/** Timer CallBacks Definitions ***********************************************/

void vSenderTimerCallBack(TimerHandle_t xTimer)
{
//	trace_puts("Inside Timer Callback");
	int timerID = (int)pvTimerGetTimerID(xTimer);
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;


	switch(timerID)
	{
	case 1:
		xSemaphoreGiveFromISR(Node1SendData, &xHigherPriorityTaskWoken); // sema = 1
		break;
	case 2:
		xSemaphoreGiveFromISR(Node2SendData, &xHigherPriorityTaskWoken);
		break;
	}

//	xTimerChangePeriodFromISR(xTimer, RandomNum(T1,T2), &xHigherPriorityTaskWoken);

	if(xHigherPriorityTaskWoken)
	{
		portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	}

}

static void vRouterDelayCallBack(TimerHandle_t xTimer)
{
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	xSemaphoreGiveFromISR(RouterTransmit, xHigherPriorityTaskWoken);

	if(xHigherPriorityTaskWoken)
	{
		portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	}
}

//!		/**		ACK Part (Commented Out for Phase 1)	**/

//? This is Alternative Solution for the ACK Receive and Delay, not completed
//void vACKToutCallBack(TimerHandle_t xTimer)
//{
//	trace_puts("Inside Timer Callback");
//	int timerID = (int)pvTimerGetTimerID(xTimer);
//	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
//
//	BaseType_t status = pdFAIL;
//
//	packet PacketRecieved;
//
//	switch(timerID)
//	{
//	case 1:
//		status = xQueueReceiveFromISR(Node1Queue, &PacketRecieved, xHigherPriorityTaskWoken);
//		if(status == pdPASS)
//		{
//			xSemaphoreGiveFromISR(Node1DataSent, xHigherPriorityTaskWoken);
//		}
//		break;
//	}
//
//	if(xHigherPriorityTaskWoken)
//	{
//		portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
//	}
//}


/** End of Timer CallBacks Definitions ****************************************/


void vApplicationMallocFailedHook( void )
{
	/* Called if a call to pvPortMalloc() fails because there is insufficient
	free memory available in the FreeRTOS heap.  pvPortMalloc() is called
	internally by FreeRTOS API functions that create tasks, queues, software
	timers, and semaphores.  The size of the FreeRTOS heap is set by the
	configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */
	for( ;; );
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook( TaskHandle_t pxTask, char *pcTaskName )
{
	( void ) pcTaskName;
	( void ) pxTask;

	/* Run time stack overflow checking is performed if
	configconfigCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
	function is called if a stack overflow is detected. */
	for( ;; );
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook( void )
{
volatile size_t xFreeStackSpace;

	/* This function is called on each cycle of the idle task.  In this case it
	does nothing useful, other than report the amout of FreeRTOS heap that
	remains unallocated. */
	xFreeStackSpace = xPortGetFreeHeapSize();

	if( xFreeStackSpace > 100 )
	{
		/* By now, the kernel has allocated everything it is going to, so
		if there is a lot of heap remaining unallocated then
		the value of configTOTAL_HEAP_SIZE in FreeRTOSConfig.h can be
		reduced accordingly. */

	}
}

void vApplicationTickHook(void) {
}

StaticTask_t xIdleTaskTCB CCM_RAM;
StackType_t uxIdleTaskStack[configMINIMAL_STACK_SIZE] CCM_RAM;

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize) {
  /* Pass out a pointer to the StaticTask_t structure in which the Idle task's
  state will be stored. */
  *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;

  /* Pass out the array that will be used as the Idle task's stack. */
  *ppxIdleTaskStackBuffer = uxIdleTaskStack;

  /* Pass out the size of the array pointed to by *ppxIdleTaskStackBuffer.
  Note that, as the array is necessarily of type StackType_t,
  configMINIMAL_STACK_SIZE is specified in words, not bytes. */
  *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

static StaticTask_t xTimerTaskTCB CCM_RAM;
static StackType_t uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH] CCM_RAM;

/* configUSE_STATIC_ALLOCATION and configUSE_TIMERS are both set to 1, so the
application must provide an implementation of vApplicationGetTimerTaskMemory()
to provide the memory that is used by the Timer service task. */
void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer, StackType_t **ppxTimerTaskStackBuffer, uint32_t *pulTimerTaskStackSize) {
  *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
  *ppxTimerTaskStackBuffer = uxTimerTaskStack;
  *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}
