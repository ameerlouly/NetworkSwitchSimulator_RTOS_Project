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
typedef uint32_t NumOfError_t;
typedef uint16_t NumOfPackets_t;
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
} NodeType_t;

/** End of Type Declarations ***************************************************/

/** Project Parameters *********************************************************/

#define T1 					( pdMS_TO_TICKS(100) )
#define T2 					( pdMS_TO_TICKS(200) )
#define Tout 				( pdMS_TO_TICKS(200) )
#define Pdrop 				( (double)0.01 )
#define P_ack 				( (double)0.01 )
#define P_WRONG_PACKET		( (double)0.0 )
#define Tdelay				( pdMS_TO_TICKS(50) )
#define D					( pdMS_TO_TICKS(5) )
#define C					( (uint8_t)100 )
#define K					( (uint16_t)40 )
static const uint32_t	L1 = 500;
static const uint32_t	L2 = 1500;

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

SemaphoreHandle_t HandleToNum;

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
	Node1Queue = xQueueCreate(10, sizeof(packet*));
	Node2Queue = xQueueCreate(10, sizeof(packet*));
	Node3Queue = xQueueCreate(10, sizeof(packet*));
	Node4Queue = xQueueCreate(10, sizeof(packet*));
	RouterQueue = xQueueCreate(30, sizeof(packet*));

	/** Creating Semaphores **/
	Node1SendData = xSemaphoreCreateBinary();
	xSemaphoreTake(Node1SendData, 0);

	Node2SendData = xSemaphoreCreateBinary();
	xSemaphoreTake(Node2SendData, 0);

	HandleToNum = xSemaphoreCreateMutex();
	xSemaphoreGive(HandleToNum);

	RouterTransmit = xSemaphoreCreateBinary();
	xSemaphoreTake(RouterTransmit, 0);

	GeneratePacket = xSemaphoreCreateMutex();
	xSemaphoreGive(GeneratePacket);

	StopTransmission = xSemaphoreCreateMutex();
	xSemaphoreTake(StopTransmission, 0);

	/** Node Types Definitions ****************************************************/

//					   {Task Handle, Queue Handle, SenderTimer, ACKTout Timer, SendData Semaphore}
	NodeType_t Node1 = {Node1Task, Node1Queue, tNode1_Sender, NULL,Node1SendData};
	NodeType_t Node2 = {Node2Task, Node2Queue, tNode2_Sender, NULL, Node2SendData};
	NodeType_t Node3 = {Node3Task, Node3Queue, NULL, NULL, NULL};
	NodeType_t Node4 = {Node4Task, Node4Queue, NULL, NULL, NULL};
	NodeType_t Router = {RouterTask, RouterQueue, tRouterDelay, NULL, RouterTransmit};

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
		status &= xTaskCreate(vRecieverTask, "Node 3", 512, (void*)&Node3, 2, &Node3Task);
		status &= xTaskCreate(vRecieverTask, "Node 4", 512, (void*)&Node4, 2, &Node4Task);
		status &= xTaskCreate(vRouterTask, "Router", 512, (void*)&Router, 3, &RouterTask);
		

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

	packet* PacketToSend = NULL;	// Packet to store packet to send
	packet* PacketBackup = NULL;
	uint16_t SequenceToNode3 = 0;
	uint16_t SequenceToNode4 = 0;
	uint16_t CurrentSequence = 0;

//?		/** Used for ACK Part **/
	packet* PacketRecieved = NULL; // Buffer to store recieved ACK Packets
	BaseType_t status;

	if(CurrentNode->CurrentTask == Node2Task)
	{
		vTaskDelay(pdMS_TO_TICKS(20)); // Small delay so that both tasks dont start at the same time
	}

	while(1)
	{
		xTimerStart(CurrentNode->CurrentTimer, 0);
		xSemaphoreTake(CurrentNode->SendDataSema, portMAX_DELAY);	// Dont Start Sending Data until allowed; Sema = 0

			/* Generate and Send Packet when Semaphore is Taken */
//		PacketToSend = PacketGenerator(CurrentNode->CurrentQueue);	//! PacketGenerator Function causes a bug, commented out for now

		xSemaphoreTake(GeneratePacket, portMAX_DELAY);

		PacketToSend = malloc(sizeof(packet));
		if(PacketToSend == NULL)
		{
			// Failed to Generate Packet, Trying Again
			trace_puts("Failed to Allocate Packet");
			continue;
		}

		PacketToSend->header.sender = CurrentNode->CurrentQueue;

		switch(RandomNum(3, 4))
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

		CurrentSequence = PacketToSend->header.sequenceNumber;

		PacketToSend->header.length = RandomNum(L1, L2);
		PacketToSend->data = calloc(PacketToSend->header.length - sizeof(header_t), sizeof(Payload_t));
		if(PacketToSend->data == NULL)
		{
			trace_puts("Failed to allocate data");
			continue;
		}


			/* Store A Backup of the Data transmitted in case of retransmission */
		PacketBackup = malloc(sizeof(packet));
		PacketBackup->data = calloc(PacketToSend->header.length - sizeof(header_t), sizeof(Payload_t));
		strcpy(PacketBackup->data, PacketToSend->data);
		PacketBackup->header = PacketToSend->header;

//		PacketBackup->header.length = PacketToSend->header.length;
//		PacketBackup->header.sequenceNumber = PacketToSend->header.sequenceNumber;
//		PacketBackup->header.sender = PacketToSend->header.sender;
//		PacketBackup->header.reciever = PacketToSend->header.reciever;

		trace_printf("Node %d: Sending %d to %d No #%d\n", QueueHandleToNum(CurrentNode->CurrentQueue),
														 PacketToSend->header.length,
														 QueueHandleToNum(PacketToSend->header.reciever),
														 CurrentSequence);
		xQueueSend(RouterQueue, &PacketToSend, portMAX_DELAY);

		xSemaphoreGive(GeneratePacket);

//?				/** ACK Part **/
		uint8_t ACK_recieved = 0;
		for(int i = 0; i < NUM_OF_TRIES; i++)
		{
			 status = xQueueReceive(CurrentNode->CurrentQueue, &PacketRecieved, Tout);
			 if(status == pdPASS)
			 {
			 	switch(QueueHandleToNum(PacketRecieved->header.sender))
				{
					case 3:
					if(PacketRecieved->header.sequenceNumber == SequenceToNode3)
					{
						ACK_recieved = 1;
					}
					break;
					
					case 4:
					if(PacketRecieved->header.sequenceNumber == SequenceToNode4)
					{
						ACK_recieved = 1;
					}
					break;
				}

				if(ACK_recieved)
				{
					break;
				}
			 }
			 else
			 {
			 	trace_printf("Node %d: Awaiting ACK from %d No #%i, Attempt #%d\n", QueueHandleToNum(CurrentNode->CurrentQueue),
																	   QueueHandleToNum(PacketBackup->header.reciever),
																	   CurrentSequence,
																	   i + 1);
			 	xQueueSend(RouterQueue, &PacketBackup, portMAX_DELAY);
			 }
		}

		 if(ACK_recieved == 1)
		 {
			// Display Received Packets
				trace_printf("\n\n***Node %d: Received ACK from %d to %d No #%d\n", QueueHandleToNum(CurrentNode->CurrentQueue),
																				    QueueHandleToNum(PacketRecieved->header.sender),
																				    QueueHandleToNum(PacketRecieved->header.reciever),
																				    PacketRecieved->header.sequenceNumber);

				xSemaphoreTake(GeneratePacket, portMAX_DELAY);
				free(PacketBackup->data);
				free(PacketBackup);
				free(PacketRecieved->data);
				free(PacketRecieved);
				xSemaphoreGive(GeneratePacket);

		 	trace_puts("Packet Sent Successfully, Sending Next Packet!");
			continue;

		 }
		 else
		 {
		 	trace_printf("Node %d Did not receive ACK, Skipping Packet...\n", QueueHandleToNum(CurrentNode->CurrentQueue));

			xSemaphoreTake(GeneratePacket, portMAX_DELAY);
			free(PacketBackup->data);
		 	free(PacketBackup);
			free(PacketRecieved->data);
			free(PacketRecieved);
			xSemaphoreGive(GeneratePacket);

			xQueueReset(CurrentNode->CurrentQueue);
			continue;
		 }
	}
}

void vRecieverTask(void *pvParameters)
{
	trace_puts("Beginning of Receiver Task");
	NodeType_t *CurrentNode = (NodeType_t*)pvParameters;
	packet* PacketRecieved = NULL;		// Buffer to Store Received Packets
//	packet* PacketToSend = NULL;	// Buffer to Store ACKs

	uint8_t sendACK = 0;

	packet* PacketToSend = NULL;

	BaseType_t status = pdFAIL;

	// QueueHandle_t SenderQueue = NULL;
	// QueueHandle_t RecieverQueue = NULL;
	// SequenceNumber_t CurrentSequence = 0;
	// uint16_t ReceivedLength = 0;
	header_t ReceivedData;

	static NumOfError_t WrongPackets = 0;

	SequenceNumber_t previousSequence1 = 0;
	SequenceNumber_t previousSequence2 = 0;

	static NumOfPackets_t totalReceived = 0;

	static NumOfPackets_t totalLost = 0;

	while(1)
	{
		status = xQueueReceive(CurrentNode->CurrentQueue, &PacketRecieved, pdMS_TO_TICKS(1));
		if(status != pdPASS)
		{
			continue;
		}

		ReceivedData = PacketRecieved->header;

		xSemaphoreTake(GeneratePacket, portMAX_DELAY);
		free(PacketRecieved->data);
		free(PacketRecieved);
		xSemaphoreGive(GeneratePacket);

		// Display Received Packets
		trace_printf("\n\nNode %d: Received %d from %d to %d No #%d\n", QueueHandleToNum(CurrentNode->CurrentQueue),
																  	  	ReceivedData.length,
																  	  	QueueHandleToNum(ReceivedData.sender),
																	  	QueueHandleToNum(ReceivedData.reciever),
																  	  	ReceivedData.sequenceNumber);


		sendACK = 1;
		// Checks if the Received Packets are meant for the Current Node
		if(QueueHandleToNum(ReceivedData.reciever) == QueueHandleToNum(CurrentNode->CurrentQueue))
		{
					/** Handle Sequence Numbers and Count Lost Packets**/
			switch(QueueHandleToNum(ReceivedData.sender))
			{
			case 1:
				if(ReceivedData.sequenceNumber <= previousSequence1)
				{
					trace_printf("Node %d: Already Received Packet\n\n", QueueHandleToNum(CurrentNode->CurrentQueue));
					sendACK = 1;
					break;
				}
				totalLost += ReceivedData.sequenceNumber - previousSequence1 - 1;
				previousSequence1 = ReceivedData.sequenceNumber;
				break;

			case 2:
				if(ReceivedData.sequenceNumber <= previousSequence2)
				{
					trace_printf("Node %d: Already Received Packet\n\n", QueueHandleToNum(CurrentNode->CurrentQueue));
					sendACK = 1;
					break;
				}
				totalLost += ReceivedData.sequenceNumber - previousSequence2 - 1;
				previousSequence2 = ReceivedData.sequenceNumber;
				break;
			}

			// Statistics Purposes
			// totalReceived++;
			// trace_printf("Received: %d, Lost: %d, Diverted: %d\n\n", totalReceived, totalLost, WrongPackets);

			xSemaphoreTake(GeneratePacket, portMAX_DELAY);
			free(PacketRecieved->data);
			free(PacketRecieved);
			xSemaphoreGive(GeneratePacket);

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

		

			if(sendACK == 1)
			{
				xSemaphoreTake(GeneratePacket, portMAX_DELAY);

				PacketToSend = malloc(sizeof(packet));
				if(PacketToSend == NULL)
				{
					// Failed to Generate Packet, Trying Again
					trace_puts("Failed to Allocate ACK");
					free(PacketToSend->data);
					free(PacketToSend);
					continue;
				}

				PacketToSend->header.sender = CurrentNode->CurrentQueue;
				PacketToSend->header.reciever = ReceivedData.sender;
				PacketToSend->header.sequenceNumber = ReceivedData.sequenceNumber;
				PacketToSend->header.length = K;
				PacketToSend->data = calloc(PacketToSend->header.length - sizeof(header_t), sizeof(Payload_t));
				if(PacketToSend->data == NULL)
				{
					// Failed to Generate Packet, Trying Again
					trace_puts("Failed to Allocate ACK Data");
					free(PacketToSend->data);
					free(PacketToSend);
					continue;
				}

				trace_printf("\n\nNode %d: Sending ACK from %d to %d No #%d\n", QueueHandleToNum(CurrentNode->CurrentQueue),
																  	  			QueueHandleToNum(PacketToSend->header.sender),
																	  			QueueHandleToNum(PacketToSend->header.reciever),
																  	  			PacketToSend->header.sequenceNumber);

				xQueueSend(RouterQueue, &PacketToSend, portMAX_DELAY); // Send ACK

				xSemaphoreGive(GeneratePacket);
			}
		
		}
		else
		{
			trace_printf("\n\nNode %d: Received Wrong Packet from %d\n", QueueHandleToNum(CurrentNode->CurrentQueue),
														   				 QueueHandleToNum(ReceivedData.sender));
			
			xSemaphoreTake(GeneratePacket, portMAX_DELAY);
			free(PacketRecieved->data);
			free(PacketRecieved);
			xSemaphoreGive(GeneratePacket);
			WrongPackets++;
		}
	}

}

void vRouterTask(void *pvParameters)
{
	trace_puts("Beginning of Router Task");
	NodeType_t *CurrentNode = (NodeType_t*)pvParameters;

	packet* PacketRecieved = NULL;	// Buffer to Process Received Packets

	while(1)
	{
		xQueueReceive(RouterQueue, &PacketRecieved, portMAX_DELAY);

		// ! Router Received Data Printer
		trace_printf("\n\n\nROUTER: Received Packet from %d to %d\n", QueueHandleToNum(PacketRecieved->header.sender),
													   QueueHandleToNum(PacketRecieved->header.reciever));
		if(PacketRecieved->header.length == K)
		{
			trace_printf("---Content: ACK\n\n\n");
		}
		else
		{
			trace_printf("Content: %d\n\n\n", PacketRecieved->header.length);
		}

		if(PacketRecieved->header.length == K)
		{
			if(checkProb(P_ack) == pdTRUE)
			{
				xSemaphoreTake(GeneratePacket, portMAX_DELAY);
				free(PacketRecieved->data);
				free(PacketRecieved);
				xSemaphoreGive(GeneratePacket);
				trace_printf("\n\nRouter Dropped ACK...\n");
			}
			else
			{
	//			vTaskDelay(Tdelay); // Delay Packet after making sure its forwarded //! Redacted, Using a Timer instead
				xTimerChangePeriod(CurrentNode->CurrentTimer, D + ((PacketRecieved->header.length * 8.0) / C), 0);
				xTimerStart(CurrentNode->CurrentTimer, 0);
				xSemaphoreTake(CurrentNode->SendDataSema, portMAX_DELAY);
				if(xQueueSend(PacketRecieved->header.reciever, &PacketRecieved, 0) != pdPASS)
				{
					trace_printf("\n\nReceiver Queue Full, Router Dropped Packet...\n");
					xSemaphoreTake(GeneratePacket, portMAX_DELAY);
					free(PacketRecieved->data);
					free(PacketRecieved);
					xSemaphoreGive(GeneratePacket);
					PacketRecieved = NULL;
				}
			}
		}
		else
		{
			if(checkProb(Pdrop) == pdTRUE)
			{
				xSemaphoreTake(GeneratePacket, portMAX_DELAY);
				free(PacketRecieved->data);
				free(PacketRecieved);
				xSemaphoreGive(GeneratePacket);
				trace_printf("\n\nRouter Dropped Packet...\n");
			}
			else if(checkProb(P_WRONG_PACKET) == pdTRUE)
			{
				trace_printf("\n\nRouter Diverting Packet...\n");
				xTimerChangePeriod(CurrentNode->CurrentTimer, D + ((PacketRecieved->header.length * 8.0) / C), 0);
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
				xTimerChangePeriod(CurrentNode->CurrentTimer, D + ((PacketRecieved->header.length * 8.0) / C), 0);
				xTimerStart(CurrentNode->CurrentTimer, 0);
				xSemaphoreTake(CurrentNode->SendDataSema, portMAX_DELAY);
				if(xQueueSend(PacketRecieved->header.reciever, &PacketRecieved, 0) != pdPASS)
				{
					trace_printf("\n\nReceiver Queue Full, Router Dropped Packet...\n");
					PacketRecieved = NULL;
				}
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

	// xTimerChangePeriodFromISR(xTimer, RandomNum(T1,T2), &xHigherPriorityTaskWoken);

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
	/* Called if a call to malloc() fails because there is insufficient
	free memory available in the FreeRTOS heap.  malloc() is called
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
