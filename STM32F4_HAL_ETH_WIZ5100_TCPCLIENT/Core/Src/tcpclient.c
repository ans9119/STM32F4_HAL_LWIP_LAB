/*
 * tcpclient.c
 *
 *  Created on: 2020. 5. 31.
 *      Author: eziya76@gmail.com
 */

#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

#include "tcpclient.h"
#include "wizInterface.h"
#include "wizchip_conf.h"
#include "socket.h"
#include "dhcp.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define DHCP_SOCKET		0	//dhcp socket 0
#define CLIENT_SOCKET	1	//tcp client socket 1

#define SERVER_IP1	192	//server ip
#define SERVER_IP2	168
#define SERVER_IP3	1
#define SERVER_IP4	227
#define SERVER_PORT	1234 //server port

static bool ip_assigned = 0;
static uint8_t serverIP[] = { SERVER_IP1, SERVER_IP2, SERVER_IP3, SERVER_IP4 };
static uint8_t dhcp_buffer[1024];
static uint16_t dhcp_retry = 0;
static uint8_t buff_size[] = { 2, 2, 2, 2 };

//network information
wiz_NetInfo netInfo = {
		.mac 	= {0x00, 0x08, 0xdc, 0xab, 0xcd, 0xef},
		.ip 	= {192, 168, 1, 180},
		.sn 	= {255, 255, 255, 0},
		.gw 	= {192, 168, 1, 1}};

//timeout information
wiz_NetTimeout timeoutInfo  = {
		.retry_cnt = 3,
		.time_100us = 10000};

//dhcp callbacks
void cbIPAddrAssigned(void) {
	printf("IP Address is assigned.\n");
	ip_assigned = true;
}

void cbIPAddrConfict(void) {
	printf("IP Address is conflicted.\n");
	ip_assigned = false;
}

//tcp client task
void StartWizTcpClientTask(void const *argument) {

	int32_t ret;
	osDelay(1000); //wait w5100 initialization when using arduino ethernet shield

	reg_wizchip_cs_cbfunc(WIZ_SPI_Select, WIZ_SPI_Deselect); //register chip select functions
	reg_wizchip_spi_cbfunc(WIZ_SPI_RxByte, WIZ_SPI_TxByte); //register spi functions

	ret = wizchip_init(buff_size, buff_size); //rx,tx buffer sizes
	if(ret < 0)
	{
		HAL_GPIO_TogglePin(LED_RED_GPIO_Port, LED_RED_Pin);
		printf("wozchip_init failed.\n");
		while(1);
	}

	ctlnetwork(CN_SET_TIMEOUT, (void*)&timeoutInfo); //set timeout

	setSHAR(netInfo.mac); //set MAC address
	DHCP_init(DHCP_SOCKET, dhcp_buffer); //init DHCP
	reg_dhcp_cbfunc(cbIPAddrAssigned, cbIPAddrAssigned, cbIPAddrConfict); //register DHCP callbacks

	dhcp_retry = 0;
	while(!ip_assigned && dhcp_retry < 5000)
	{
		dhcp_retry++;
		DHCP_run();		//get ip from dhcp server
	}

	if(ip_assigned)
	{
		getIPfromDHCP(netInfo.ip);
		getGWfromDHCP(netInfo.gw);
		getSNfromDHCP(netInfo.sn);
	}

	wizchip_setnetinfo(&netInfo);	//set network information
	wizchip_getnetinfo(&netInfo);	//get network information

	printf("IP: %03d.%03d.%03d.%03d\nGW: %03d.%03d.%03d.%03d\nNet: %03d.%03d.%03d.%03d\n", netInfo.ip[0], netInfo.ip[1],
				netInfo.ip[2], netInfo.ip[3], netInfo.gw[0], netInfo.gw[1], netInfo.gw[2], netInfo.gw[3], netInfo.sn[0],
				netInfo.sn[1], netInfo.sn[2], netInfo.sn[3]);

	while (1) {
		ret = socket(CLIENT_SOCKET, Sn_MR_TCP, 3000, 0);	//create socket
		if (ret < 0) {
			HAL_GPIO_TogglePin(LED_RED_GPIO_Port, LED_RED_Pin);
			printf("socket failed{%ld}.\n", ret);
			close(CLIENT_SOCKET);
			continue;
		}

		ret = connect(CLIENT_SOCKET, serverIP, SERVER_PORT);	//connect to the server
		if (ret < 0) {
			HAL_GPIO_TogglePin(LED_RED_GPIO_Port, LED_RED_Pin);
			printf("connect failed{%ld}.\n", ret);
			close(CLIENT_SOCKET);
			continue;
		}

		//prepare data to send & receive
		struct time_packet packet;
		memset(&packet, 0, sizeof(struct time_packet));
		packet.head = 0xAE;	//head
		packet.type = REQ; 	//request type
		packet.tail = 0xEA; //tail

		uint8_t failed = 0;
		int16_t written = 0;
		int16_t read = 0;

		//send request
		do {
			ret = send(CLIENT_SOCKET, (uint8_t*) (&packet + written), sizeof(struct time_packet) - written);
			if (ret < 0) {
				HAL_GPIO_TogglePin(LED_RED_GPIO_Port, LED_RED_Pin);
				printf("send failed{%ld}.\n", ret);
				close(CLIENT_SOCKET); //unexpected close
				failed = 1;
				break;
			}
			written += ret;
		} while (written < sizeof(struct time_packet));

		if (failed)
			continue; //start again

		//receive response
		while (1) {
			ret = recv(CLIENT_SOCKET, (uint8_t*) (&packet + read), sizeof(struct time_packet) - read);
			if (ret < 0) {
				HAL_GPIO_TogglePin(LED_RED_GPIO_Port, LED_RED_Pin);
				printf("recv failed.{%ld}\n", ret);
				close(CLIENT_SOCKET); //unexpected close
				failed = 1;
				break;
			}

			read += ret;
			if (read >= sizeof(struct time_packet))	//overflow
				break;
		}

		if (failed)
			continue; //start again

		if (read == sizeof(struct time_packet) && packet.type == RESP) //if received length is valid
				{
			printf("%04d-%02d-%02d %02d:%02d:%02d\n", packet.year + 2000, packet.month, packet.day, packet.hour,
					packet.minute, packet.second); //print time information
			HAL_GPIO_TogglePin(LED_BLUE_GPIO_Port, LED_BLUE_Pin); //toggle data led
		}

		disconnect(CLIENT_SOCKET);	//send FIN
		close(CLIENT_SOCKET);		//close socket

		osDelay(10);
	}

	vTaskDelete(NULL);	//clear task
}
