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
#include <string.h>

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
#define MAX_NUM_OF_PACKETS		(uint16_t)4000
#define DivertPacket(PacketToDivert) 		( (PacketToDivert == Node3Queue)? Node4Queue : Node3Queue )

/** End of Macros *************************************************************/

/** Type Declarations	*******************************************************/

typedef uint32_t SequenceNumber_t;
typedef uint16_t NumOfError_t;
typedef uint16_t NumOfPackets_t;
typedef uint32_t NumOfBytes_t;

typedef uint8_t  Payload_t;

typedef struct {
	QueueHandle_t sender;
	QueueHandle_t reciever;
} Ack_t;

typedef struct {
	QueueHandle_t sender;
	QueueHandle_t reciever;

	SequenceNumber_t sequenceNumber;

	uint16_t length;

	uint16_t padding;	// To make sure the header 16 bytes
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
	SemaphoreHandle_t ACK_Sema;
} NodeType_t;

/** End of Type Declarations ***************************************************/

/** Project Parameters *********************************************************/

#define T1 					( pdMS_TO_TICKS(100) )
#define T2 					( pdMS_TO_TICKS(200) )
#define Tout 				( pdMS_TO_TICKS(200) )	// 150, 175, 200, 225
#define Pdrop 				( (double)0.02 )		// 0.01, 0.02, 0.04, 0.08
#define P_ack 				( (double)0.01 )
#define P_WRONG_PACKET		( (double)0.0 )
#define Tdelay				( pdMS_TO_TICKS(50) )
#define D					( pdMS_TO_TICKS(5) )
#define C					( (uint32_t)100000 )
#define K					( (uint16_t)40 )
#define N					( (uint8_t)4 )
static const uint32_t	L1 = 500;
static const uint32_t	L2 = 1500;

/** End of Project Parameters *************************************************/

/** Timer CallBacks Prototypes ************************************************/

static void vSenderTimerCallBack(TimerHandle_t xTimer);
static void vRouterDelayCallBack(TimerHandle_t xTimer);
void vACKToutCallBack(TimerHandle_t xTimer);

/** End of Timer CallBacks Prototypes *****************************************/

/** Timer Handles *************************************************************/

TimerHandle_t tNode1_Sender = NULL;
TimerHandle_t tNode2_Sender = NULL;

TimerHandle_t tRouterDelay = NULL;
TimerHandle_t tRouterACKDelay = NULL;

TimerHandle_t	tNode1_ACKTout = NULL;
TimerHandle_t	tNode2_ACKTout = NULL;

/** End of Timer Handles ******************************************************/

/** Semaphore Handles *********************************************************/

SemaphoreHandle_t Node1SendData;
SemaphoreHandle_t Node2SendData;

SemaphoreHandle_t RouterTransmit;
SemaphoreHandle_t RouterACKTransmit;

SemaphoreHandle_t HandleToNum;

SemaphoreHandle_t GeneratePacket;

SemaphoreHandle_t StopTransmission;

SemaphoreHandle_t Node1ACKReceive;
SemaphoreHandle_t Node2ACKReceive;

/** End of Semaphore Handles **************************************************/

/** Task Handles **************************************************************/

TaskHandle_t Node1Task = NULL;
TaskHandle_t Node2Task = NULL;
TaskHandle_t Node3Task = NULL;
TaskHandle_t Node4Task = NULL;
TaskHandle_t RouterTask = NULL;
TaskHandle_t RouterACKTask = NULL;

/** End of Task Handles *******************************************************/

/** Queue Handles *************************************************************/

QueueHandle_t Node1Queue;
QueueHandle_t Node2Queue;
QueueHandle_t Node3Queue;
QueueHandle_t Node4Queue;
QueueHandle_t RouterQueue;
QueueHandle_t RouterACKQueue;

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

/** Global Variables for Statistics ***************/

TickType_t startTime = 0;
TickType_t endTime = 0;

/** End Global Variables for Statistics ***********/

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
	tRouterACKDelay = xTimerCreate("Router Delay Timer",
								Tdelay,
								pdFALSE,
								(void*)2,
								vRouterDelayCallBack);
	tNode1_ACKTout = xTimerCreate("Node 1 Tout",
								 Tout,
								 pdFALSE,
								 (void*)1,
								 vACKToutCallBack);
	tNode2_ACKTout = xTimerCreate("Node 2 Tout",
								 Tout,
								 pdFALSE,
								 (void*)2,
								 vACKToutCallBack);
//	xTimerStart(tNode1_Sender, 0);
//	if(tNode1_Sender == NULL)
//	{
//		trace_puts("Error Creating Timer");
//		return 0;
//	}

	/** Creating Queues **/
	Node1Queue = xQueueCreate(20, sizeof(packet*));
	Node2Queue = xQueueCreate(20, sizeof(packet*));
	Node3Queue = xQueueCreate(40, sizeof(packet*));
	Node4Queue = xQueueCreate(40, sizeof(packet*));
	RouterQueue = xQueueCreate(40, sizeof(packet*));
	RouterACKQueue = xQueueCreate(40, sizeof(packet*));

	/** Creating Semaphores **/
	Node1SendData = xSemaphoreCreateBinary();
	xSemaphoreTake(Node1SendData, 0);

	Node2SendData = xSemaphoreCreateBinary();
	xSemaphoreTake(Node2SendData, 0);

	HandleToNum = xSemaphoreCreateMutex();
	xSemaphoreGive(HandleToNum);

	RouterTransmit = xSemaphoreCreateBinary();
	xSemaphoreTake(RouterTransmit, 0);

	RouterACKTransmit = xSemaphoreCreateBinary();
	xSemaphoreTake(RouterACKTransmit, 0);

	GeneratePacket = xSemaphoreCreateMutex();
	xSemaphoreGive(GeneratePacket);

	StopTransmission = xSemaphoreCreateMutex();
	xSemaphoreTake(StopTransmission, 0);

	Node1ACKReceive = xSemaphoreCreateBinary();
	xSemaphoreTake(Node1ACKReceive, 0);

	Node2ACKReceive = xSemaphoreCreateBinary();
	xSemaphoreTake(Node2ACKReceive, 0);

	/** Node Types Definitions ****************************************************/

//					   {Task Handle, Queue Handle, SenderTimer, ACKTout Timer, SendData Semaphore, ACK Sema}
	NodeType_t Node1 = {Node1Task, Node1Queue, tNode1_Sender, tNode1_ACKTout,Node1SendData, Node1ACKReceive};
	NodeType_t Node2 = {Node2Task, Node2Queue, tNode2_Sender, tNode2_ACKTout, Node2SendData, Node2ACKReceive};
	NodeType_t Node3 = {Node3Task, Node3Queue, NULL, NULL, NULL, NULL};
	NodeType_t Node4 = {Node4Task, Node4Queue, NULL, NULL, NULL, NULL};
	NodeType_t Router = {RouterTask, RouterQueue, tRouterDelay, NULL, RouterTransmit, NULL};
	NodeType_t RouterACK = {RouterACKTask, RouterACKQueue, tRouterACKDelay, NULL, RouterACKTransmit, NULL};

	/** End of Node Types Definitions *********************************************/
	if(		Node1Queue != NULL &&
			Node2Queue != NULL &&
			Node3Queue != NULL &&
			Node4Queue != NULL &&
			RouterQueue != NULL &&
			RouterACKQueue != NULL)	// Check if Queue Creation was successful
	{
		// Creating Tasks
		status = xTaskCreate(vSenderTask, "Node 1", 512, (void*)&Node1, 1, &Node1Task);
		status &= xTaskCreate(vSenderTask, "Node 2", 512, (void*)&Node2, 1, &Node2Task);
		status &= xTaskCreate(vRecieverTask, "Node 3", 512, (void*)&Node3, 2, &Node3Task);
		status &= xTaskCreate(vRecieverTask, "Node 4", 512, (void*)&Node4, 2, &Node4Task);
		status &= xTaskCreate(vRouterTask, "Router", 512, (void*)&Router, 3, &RouterTask);
		status &= xTaskCreate(vRouterTask, "Router ACK", 512, (void*)&RouterACK, 3, &RouterACKTask);
		

		if(status == pdPASS)
		{
			puts("Starting Scheduler\n");
			startTime = xTaskGetTickCount();
			vTaskStartScheduler();
		}
		else
		{
			puts("Error Creating Tasks");
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

	packet* PacketToSend = NULL;	// Packet to store packet to send
	packet* PacketBackup[N] = { 0 };
	SequenceNumber_t SequenceToNode3 = 0;
	SequenceNumber_t SequenceToNode4 = 0;
	uint16_t CurrentSequence = 0;

	NumOfPackets_t totalSent = 0;
	NumOfPackets_t totalACKsReceived = 0;
	NumOfPackets_t totalDropped = 0;
	NumOfPackets_t NumOfTries = 0;

	NumOfBytes_t BytesSent = 0;
	NumOfBytes_t BytesSuccess = 0;
	NumOfBytes_t BytesFailed = 0;

//?		/** Used for ACK Part **/
	packet* PacketRecieved = NULL; // Buffer to store recieved ACK Packets
	BaseType_t status;

	if(CurrentNode->CurrentTask == Node2Task)
	{
		vTaskDelay(pdMS_TO_TICKS(50)); // Small delay so that both tasks dont start at the same time
	}

	xQueueReset(CurrentNode->CurrentQueue);

	while(1)
	{
		
		xTimerStart(CurrentNode->CurrentTimer, 0);
		xSemaphoreTake(CurrentNode->SendDataSema, portMAX_DELAY);	// Dont Start Sending Data until allowed; Sema = 0
		uint8_t CurrentReceiver = RandomNum(3, 4);

		for(int i = 0; i < N; i++)
		{
			xSemaphoreTake(GeneratePacket, portMAX_DELAY);
				/* Generate and Send Packet when Semaphore is Taken */
			PacketToSend = pvPortMalloc(sizeof(packet));
			if(PacketToSend == NULL)
			{
				// Failed to Generate Packet, Trying Again
				trace_puts("Failed to Allocate Packet");
				xSemaphoreGive(GeneratePacket);
				continue;
			}

			PacketToSend->header.sender = CurrentNode->CurrentQueue;
			switch(CurrentReceiver)
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
			PacketToSend->data = pvPortMalloc((PacketToSend->header.length - sizeof(header_t)) * sizeof(Payload_t));
			if(PacketToSend->data == NULL)
			{
				trace_puts("Failed to allocate data");
				vPortFree(PacketToSend);
				xSemaphoreGive(GeneratePacket);
				continue;
			}

				/* Store A Data in buffer till ACK is Recieved */
			// CurrentSequence = PacketToSend->header.sequenceNumber;
			PacketBackup[i] = pvPortMalloc(sizeof(packet));
			PacketBackup[i]->data = pvPortMalloc((PacketToSend->header.length - sizeof(header_t)) * sizeof(Payload_t));
			memcpy(PacketBackup[i]->data, PacketToSend->data, sizeof(Payload_t) * (PacketToSend->header.length - sizeof(header_t)));
			memcpy(&PacketBackup[i]->header, &PacketToSend->header, sizeof(header_t));


			trace_printf("Node %d: Sending %d to %d No #%d\n", QueueHandleToNum(CurrentNode->CurrentQueue),
															PacketToSend->header.length,
															QueueHandleToNum(PacketToSend->header.reciever),
															PacketToSend->header.sequenceNumber);
												
			if(xQueueSend(RouterQueue, &PacketToSend, portMAX_DELAY) == pdPASS)
			{
				// trace_printf("Node %d: Sent Successfully to %d\n", QueueHandleToNum(PacketToSend->header.sender),
				// 												QueueHandleToNum(PacketToSend->header.reciever));
				totalSent++;
				BytesSent += PacketBackup[i]->header.length;
			}
			else
			{
				trace_printf("Node %d: Failed to Send\n", QueueHandleToNum(CurrentNode->CurrentQueue));
			}

			xSemaphoreGive(GeneratePacket);
		}	// End of Go-Back-N for loop

//? ****************** ACK Part (Sender Section) ********************************************************************************************************************
		uint8_t ACK_recieved = 0;	// Used only for Status Message after Checking transmission Buffer ACKs
		uint8_t BufferStatus[N] = { 0 };	// Used to track the status of each ACK in transmission Buffer
		SequenceNumber_t PreviousSequence = PacketBackup[0]->header.sequenceNumber;
		int j = 0;
		while(j < N)
		{
			int i = 0;
			while(i < NUM_OF_TRIES)
			{
				ACK_recieved = 0;
				xTimerStart(CurrentNode->ACKToutTimer, 0);
				xSemaphoreTake(CurrentNode->ACK_Sema, portMAX_DELAY);
				status = xQueueReceive(CurrentNode->CurrentQueue, &PacketRecieved, 0);
				if(status == pdPASS)
				{
					trace_printf("------Node %d: Recieved ACK from %d No #%i at Attempt #%d\n", QueueHandleToNum(CurrentNode->CurrentQueue),
																						QueueHandleToNum(PacketBackup[j]->header.reciever),
																						PacketBackup[j]->header.sequenceNumber,
																						i + 1);
					if(PacketRecieved->header.sequenceNumber >= PreviousSequence &&
					   PacketRecieved->header.sender == PacketBackup[j]->header.reciever)
					{
						// SequenceToNode3 = PacketRecieved->header.sequenceNumber;
						ACK_recieved = 1;
						BufferStatus[j] = 1;
						PreviousSequence = PacketRecieved->header.sequenceNumber;
						xSemaphoreTake(GeneratePacket, portMAX_DELAY);
						vPortFree(PacketRecieved->data);
						vPortFree(PacketRecieved);
						xSemaphoreGive(GeneratePacket);
						i++;
						break; // Exit the Retry loop
					}
					else
					{
						// In case of recieving an old ACK from a previous packet
						trace_puts("____OLD ACK____: Resending Packets");
						xSemaphoreTake(GeneratePacket, portMAX_DELAY);
						vPortFree(PacketRecieved->data);
						vPortFree(PacketRecieved);
						xSemaphoreGive(GeneratePacket);

						while(xQueueReceive(CurrentNode->CurrentQueue, &PacketRecieved, 0) == pdPASS)
						{
							vPortFree(PacketRecieved->data);
							vPortFree(PacketRecieved);
						}
						continue;
					}
				}
				else
				{
					trace_printf("Node %d: Awaiting ACK from %d No #%i, Attempt #%d\n", QueueHandleToNum(CurrentNode->CurrentQueue),
																						QueueHandleToNum(PacketBackup[j]->header.reciever),
																						PacketBackup[j]->header.sequenceNumber,
																						i + 1);
					trace_puts("Resending Packets...");

					while(xQueueReceive(CurrentNode->CurrentQueue, &PacketRecieved, 0) == pdPASS)
					{
						vPortFree(PacketRecieved->data);
						vPortFree(PacketRecieved);
					}

					xSemaphoreTake(GeneratePacket, portMAX_DELAY);
					for(int k = j; k < N; k++)
					{																
						NumOfTries++;
						PacketToSend = pvPortMalloc(sizeof(packet));
						PacketToSend->data = pvPortMalloc((PacketBackup[k]->header.length - sizeof(header_t)) * sizeof(Payload_t));
						memcpy(PacketToSend->data, PacketBackup[k]->data, sizeof(Payload_t) * (PacketBackup[k]->header.length - sizeof(header_t)));
						memcpy(&PacketToSend->header, &PacketBackup[k]->header, sizeof(header_t));
						xQueueSend(RouterQueue, &PacketToSend, portMAX_DELAY);
					}
					xSemaphoreGive(GeneratePacket);
					i++;
				}
			} // End of Retry Loop
			j++;
		} // End of Go-Back-N for loop

		// Clear Queue from all old ACKs
		while(xQueueReceive(CurrentNode->CurrentQueue, &PacketRecieved, 0) == pdPASS)
		{
			vPortFree(PacketRecieved->data);
			vPortFree(PacketRecieved);
		}

		if(ACK_recieved == 1)
		{
			// Display Received Packets
			trace_printf("\n\n***Node %d: Received ACKs from %d to %d\n", QueueHandleToNum(CurrentNode->CurrentQueue),
																				QueueHandleToNum(PacketRecieved->header.sender),
																				QueueHandleToNum(PacketRecieved->header.reciever));

			xSemaphoreTake(GeneratePacket, portMAX_DELAY);
			// trace_puts("Before Free 3");
			for(int i = 0; i < N; i++)
			{
				if(BufferStatus[i] == 1)
				{
					BytesSuccess += PacketBackup[i]->header.length;
					totalACKsReceived++;
				}
				else
				{
					BytesFailed += PacketBackup[i]->header.length;
					totalDropped++;
				}
				vPortFree(PacketBackup[i]->data);
				vPortFree(PacketBackup[i]);
			}
			xSemaphoreGive(GeneratePacket);

		 	trace_puts("Packet Sent Successfully, Sending Next Patch!");
		}
		else
		{
		 	trace_printf("Node %d Did not receive ACK, Skipping Packets...\n", QueueHandleToNum(CurrentNode->CurrentQueue));

			// BytesFailed += PacketBackup->header.length;

			xSemaphoreTake(GeneratePacket, portMAX_DELAY);
			// trace_puts("Before Free 4");
			for(int i = 0; i < N; i++)
			{
				if(BufferStatus[i] == 1)
				{
					BytesSuccess += PacketBackup[i]->header.length;
					totalACKsReceived++;
				}
				else
				{
					BytesFailed += PacketBackup[i]->header.length;
					totalDropped++;
				}
				vPortFree(PacketBackup[i]->data);
				vPortFree(PacketBackup[i]);
			}

			xSemaphoreGive(GeneratePacket);
		}
		 
		// BytesFailed = BytesSent - BytesSuccess;
		// totalDropped = totalSent - totalACKsReceived;

		 				/** System Statistics **/
		printf("\n\n\n\n-------NODE %d STATISTICS--------\n", QueueHandleToNum(CurrentNode->CurrentQueue));
		printf("Total Packets Sent: %d\n", totalSent);
		printf("Total ACKs Received: %d\n", totalACKsReceived);
		printf("Total Re-Transmitted: %d\n", NumOfTries);
		printf("Total Packets Dropped: %d\n\n", totalDropped);
		printf("Bytes Sent: %ld\n", BytesSent);
		printf("Bytes Successful: %ld\n", BytesSuccess);
		printf("Bytes Failed: %ld\n", BytesFailed);
		puts("------------------------------------------\n\n\n\n\n");

		if(totalSent == 500)
		{
			endTime = xTaskGetTickCount();
			trace_puts("\n\n\n...SUSPENDING ALL TASKS...\n");
			trace_printf("Total Elapsed Time = %ld seconds", ((endTime - startTime) * portTICK_PERIOD_MS) / 1000);
			vTaskSuspendAll();
		}
	}
}

void vRecieverTask(void *pvParameters)
{
	trace_puts("Beginning of Receiver Task");
	NodeType_t *CurrentNode = (NodeType_t*)pvParameters;
	packet* PacketRecieved = NULL;		// Buffer to Store Received Packets
//	packet* PacketToSend = NULL;	// Buffer to Store ACKs

	packet* PacketToSend = NULL;

	BaseType_t status = pdFAIL;

	header_t ReceivedData;	// Buffer to Hold received Packets

	NumOfError_t WrongPackets = 0;

	SequenceNumber_t previousSequence1 = 0;
	SequenceNumber_t previousSequence2 = 0;

//	NumOfPackets_t totalReceived = 0;

	NumOfPackets_t totalLost = 0;

	xQueueReset(CurrentNode->CurrentQueue);

	while(1)
	{
		status = xQueueReceive(CurrentNode->CurrentQueue, &PacketRecieved, portMAX_DELAY);
		if(status != pdPASS)
		{
			continue;
		}
		else if(PacketRecieved == NULL)
		{
			continue;
		}
		else if(QueueHandleToNum(PacketRecieved->header.sender) == 255 || QueueHandleToNum(PacketRecieved->header.reciever) == 255)
		{
			puts("Receiver: Corrupted Packet, Dropping");
			// trace_puts("Before Free 5");
			vPortFree(PacketRecieved->data);
			vPortFree(PacketRecieved);
			continue;
		}

		// Display Received Packets
		// trace_printf("\n\nNode %d: Received %d from %d to %d No #%d\n", QueueHandleToNum(CurrentNode->CurrentQueue),
		// 														  	  	PacketRecieved->header.length,
		// 														  	  	QueueHandleToNum(PacketRecieved->header.sender),
		// 															  	QueueHandleToNum(PacketRecieved->header.reciever),
		// 														  	  	PacketRecieved->header.sequenceNumber);

		// ReceivedData = PacketRecieved->header;
		memcpy(&ReceivedData, &PacketRecieved->header, sizeof(header_t));

		xSemaphoreTake(GeneratePacket, portMAX_DELAY);
		// trace_puts("Before Free 6");
		vPortFree(PacketRecieved->data);
		vPortFree(PacketRecieved);
		xSemaphoreGive(GeneratePacket);



		// Checks if the Received Packets are meant for the Current Node
		if(ReceivedData.reciever == CurrentNode->CurrentQueue)
		{
					/** Handle Sequence Numbers and Count Lost Packets**/
			switch(QueueHandleToNum(ReceivedData.sender))
			{
			case 1:
				if(ReceivedData.sequenceNumber <= previousSequence1)
				{
					// trace_printf("Node %d: Already Received Packet\n\n", QueueHandleToNum(CurrentNode->CurrentQueue));
					break;
				}
				totalLost += ReceivedData.sequenceNumber - previousSequence1 - 1;
				if(totalLost != 0)
				{
					totalLost = 0;
					continue;
				}
				previousSequence1 = ReceivedData.sequenceNumber;
				break;

			case 2:
				if(ReceivedData.sequenceNumber <= previousSequence2)
				{
					// trace_printf("Node %d: Already Received Packet\n\n", QueueHandleToNum(CurrentNode->CurrentQueue));
					break;
				}
				totalLost += ReceivedData.sequenceNumber - previousSequence2 - 1;
				if(totalLost != 0)
				{
					totalLost = 0;
					continue;
				}
				previousSequence2 = ReceivedData.sequenceNumber;
				break;
			}

			// Statistics Purposes
			// totalReceived++;
			// trace_printf("--Receiver Node %d Statistics--\n", QueueHandleToNum(CurrentNode->CurrentQueue));
			// trace_printf("Received: %d, Lost: %d, Diverted: %d\n\n", totalReceived, totalLost, WrongPackets);

// 			// Suspend Current Task if it has received 2000 or more Packets
// 			if((totalReceived + totalLost) >= MAX_NUM_OF_PACKETS)
// 			{
// 				trace_printf("\n**SYSTEM SUSPENDED...PRINTING STATISTICS....\n");
// 				trace_printf("\nTotal Packets: %d\n", totalReceived + totalLost);
// 				trace_printf("Total Received: %d\n", totalReceived);
// 				trace_printf("Total Lost: %d\n", totalLost);
// 				trace_printf("Total Dropped: %d\n", totalLost - WrongPackets);
// //				trace_printf("Lost \% %d", ((float)totalLost1/(totalReceived1 + totalLost1)) * 100);
// 				trace_printf("Diverted Packets: %d\n", WrongPackets);
// 				vTaskSuspendAll(); // Suspends Task
// 			}

//?			/**  ACK Part  **/

		

				xSemaphoreTake(GeneratePacket, portMAX_DELAY);

				PacketToSend = pvPortMalloc(sizeof(packet));
				if(PacketToSend == NULL)
				{
					// Failed to Generate Packet, Trying Again
					trace_puts("Failed to Allocate ACK");
					continue;
				}

				PacketToSend->header.sender = CurrentNode->CurrentQueue;
				PacketToSend->header.reciever = ReceivedData.sender;
				PacketToSend->header.sequenceNumber = ReceivedData.sequenceNumber;
				PacketToSend->header.length = K;
				PacketToSend->data = pvPortMalloc((K - sizeof(header_t)) * sizeof(Payload_t));
				if(PacketToSend->data == NULL)
				{
					// Failed to Generate Packet, Trying Again
					trace_puts("Failed to Allocate ACK Data");
					vPortFree(PacketToSend);
					continue;
				}

				// trace_printf("\n\nNode %d: Sending ACK from %d to %d No #%d\n", QueueHandleToNum(CurrentNode->CurrentQueue),
				// 												  	  			QueueHandleToNum(PacketToSend->header.sender),
				// 													  			QueueHandleToNum(PacketToSend->header.reciever),
				// 												  	  			PacketToSend->header.sequenceNumber);

				if((QueueHandleToNum(PacketToSend->header.sender) != 255) && (QueueHandleToNum(PacketToSend->header.reciever) != 255))
					xQueueSend(RouterACKQueue, &PacketToSend, portMAX_DELAY); // Send ACK
				else
				{
					printf("Receiver %d: Data Corrupted", QueueHandleToNum(CurrentNode->CurrentQueue));
					// trace_puts("Before Free 7");
					xSemaphoreGive(GeneratePacket);
					continue;
				}

				xSemaphoreGive(GeneratePacket);
		}
		else
		{
			// trace_printf("\n\nNode %d: Received Wrong Packet from %d\n", QueueHandleToNum(CurrentNode->CurrentQueue),
			// 											   				 QueueHandleToNum(ReceivedData.sender));
			WrongPackets++;
		}
	}

}

void vRouterTask(void *pvParameters)
{
	trace_puts("Beginning of Router Task");
	NodeType_t *CurrentNode = (NodeType_t*)pvParameters;

	packet* PacketRecieved = NULL;	// Buffer to Process Received Packets

	BaseType_t status;

	xQueueReset(CurrentNode->CurrentQueue);

	while(1)
	{
		status = xQueueReceive(CurrentNode->CurrentQueue, &PacketRecieved, portMAX_DELAY);
		// status = xQueueReceive(RouterACKQueue, &ACKReceived, pdMS_TO_TICKS(1));

		if(QueueHandleToNum(PacketRecieved->header.sender) == 255 || QueueHandleToNum(PacketRecieved->header.reciever) == 255)
		{
			puts("ROUTER: Corrupted Packet, Dropping");
			// trace_puts("Before Free 8");
			vPortFree(PacketRecieved->data);
			vPortFree(PacketRecieved);
			continue;
		}

		//? Router Received Data Printer
		// printf("\n\n\nROUTER: Received Packet from %d to %d\n", QueueHandleToNum(PacketRecieved->header.sender),
		// 											   QueueHandleToNum(PacketRecieved->header.reciever));
		// if(status == pdPASS && PacketRecieved->header.length == K)
		// {
		// 	printf("---Content: ACK\n\n\n");
		// }
		// else if(status == pdPASS)
		// {
		// 	printf("Content: %d\n\n\n", PacketRecieved->header.length);
		// }
		// else if(PacketRecieved == NULL)
		// {
		// 	continue;
		// }
		// else
		// {
		// 	continue;
		// }

		if(status == pdPASS && PacketRecieved->header.length == K)
		{
			if(checkProb(P_ack) == pdTRUE)
			{
				xSemaphoreTake(GeneratePacket, portMAX_DELAY);
				// trace_puts("Before Free 9");
				vPortFree(PacketRecieved->data);
				vPortFree(PacketRecieved);
				xSemaphoreGive(GeneratePacket);
				printf("\n\nRouter Dropped ACK...\n");
			}
			else
			{
				xTimerChangePeriod(CurrentNode->CurrentTimer, D + pdMS_TO_TICKS( ((PacketRecieved->header.length * 8.0) / C) * 1000), 0);
				xTimerStart(CurrentNode->CurrentTimer, 0);
				xSemaphoreTake(CurrentNode->SendDataSema, portMAX_DELAY);
				if(xQueueSend(PacketRecieved->header.reciever, &PacketRecieved, 0) != pdPASS)
				{
					printf("\n\nReceiver Queue Full, Router Dropped Packet...\n");
					xSemaphoreTake(GeneratePacket, portMAX_DELAY);
					// trace_puts("Before Free 10");
					vPortFree(PacketRecieved->data);
					vPortFree(PacketRecieved);
					xSemaphoreGive(GeneratePacket);
					PacketRecieved = NULL;
				}
			}
		}
		else if(status == pdPASS)
		{
			if(checkProb(Pdrop) == pdTRUE)
			{
				xSemaphoreTake(GeneratePacket, portMAX_DELAY);
				// trace_puts("Before Free 11");
				vPortFree(PacketRecieved->data);
				vPortFree(PacketRecieved);
				xSemaphoreGive(GeneratePacket);
				trace_printf("\n\nRouter Dropped Packet...\n");
			}
			else if(checkProb(P_WRONG_PACKET) == pdTRUE)
			{
				trace_printf("\n\nRouter Diverting Packet...\n");
				xTimerChangePeriod(CurrentNode->CurrentTimer, D + pdMS_TO_TICKS( ((PacketRecieved->header.length * 8.0) / C) * 1000), 0);
				xTimerStart(CurrentNode->CurrentTimer, 0);
				xSemaphoreTake(CurrentNode->SendDataSema, portMAX_DELAY);

				if(xQueueSend(DivertPacket(PacketRecieved->header.reciever), &PacketRecieved, 0) != pdPASS)
				{
					trace_printf("\n\nReceiver Queue Full, Router Dropped Packet...\n");
					PacketRecieved = NULL;
				}
			}
			else
			{
	//			vTaskDelay(Tdelay); // Delay Packet after making sure its forwarded //! Redacted, Using a Timer instead
				xTimerChangePeriod(CurrentNode->CurrentTimer, D + pdMS_TO_TICKS( ((PacketRecieved->header.length * 8.0) / C) * 1000), 0);
				xTimerStart(CurrentNode->CurrentTimer, 0);
				xSemaphoreTake(CurrentNode->SendDataSema, portMAX_DELAY);
				if(xQueueSend(PacketRecieved->header.reciever, &PacketRecieved, 0) != pdPASS)
				{
					trace_printf("\n\nReceiver Queue Full, Router Dropped Packet...\n");
					PacketRecieved = NULL;
				}
			}
		}
		else
		{
			puts("ROUTER: Failed to receive packet");
			// trace_puts("Before Free 12");
			vPortFree(PacketRecieved->data);
			vPortFree(PacketRecieved);
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

	xTimerChangePeriodFromISR(xTimer, RandomNum(T1,T2), &xHigherPriorityTaskWoken);

	if(xHigherPriorityTaskWoken)
	{
		portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	}

}

static void vRouterDelayCallBack(TimerHandle_t xTimer)
{
	int timerID = (int)pvTimerGetTimerID(xTimer);
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	
	switch(timerID)
	{
	case 1:
		xSemaphoreGiveFromISR(RouterTransmit, xHigherPriorityTaskWoken);
		break;

	case 2:
		xSemaphoreGiveFromISR(RouterACKTransmit, xHigherPriorityTaskWoken);
		break;
	}

	if(xHigherPriorityTaskWoken)
	{
		portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	}
}

//!		/**		ACK Part (Commented Out for Phase 1)	**/

//? This is Alternative Solution for the ACK Receive and Delay, not completed
void vACKToutCallBack(TimerHandle_t xTimer)
{
	int timerID = (int)pvTimerGetTimerID(xTimer);
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;

	switch(timerID)
	{
	case 1:
		xSemaphoreGiveFromISR(Node1ACKReceive ,&xHigherPriorityTaskWoken);
		break;
	case 2:
		xSemaphoreGiveFromISR(Node2ACKReceive ,&xHigherPriorityTaskWoken);
		break;
	}

	if(xHigherPriorityTaskWoken)
	{
		portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	}
}


/** End of Timer CallBacks Definitions ****************************************/


void vApplicationMallocFailedHook( void )
{
	/* Called if a call to pvPortMalloc() fails because there is insufficient
	free memory available in the FreeRTOS heap.  pvPortMalloc() is called
	internally by FreeRTOS API functions that create tasks, queues, software
	timers, and semaphores.  The size of the FreeRTOS heap is set by the
	configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */
	endTime = xTaskGetTickCount();
	trace_puts("\n\n\n...MALLOC FAILED...\n");
	trace_printf("Total Elapsed Time = %ld seconds", ((endTime - startTime) * portTICK_PERIOD_MS) / 1000);
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
