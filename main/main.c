/*
 * author: Adam Seals
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "command.h"
#include "config.h"

#include "esp_wifi.h"
#include "inttypes.h"
#include "network.h"
#include "stddef.h"
#include "esp_log.h"
#include "esp_err.h"

#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "mdns.h"

static const char *TAG = "MAIN";

volatile bool dataButtonHit = false;

#define DEVICE_TYPE "dooropen"

void app_main(void){

	esp_err_t ret;

	//======= INITIALIZE STORAGE =======
	//This is used by the wifi driver to store cridentials

	printf("Initializing flash storage...\n");
	fflush(stdout);

	ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		printf("Erasing flash\n");
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}else{
		printf("Some other error with nvs: %i\n", ret);
	}

	nvs_handle_t nvsHandle;
	nvs_open("storage", NVS_READWRITE, &nvsHandle);

	ESP_ERROR_CHECK(ret);

	fflush(stdin);
	printf("Enter something in the next 5 seconds to enter command mode.\n");
	fflush(stdout);

	vTaskDelay(5000/portTICK_PERIOD_MS);

	char tmp[512]="";
	fgets(tmp, 512, stdin);

	struct wifiCridentials wifiCrids = {
		.ssid = "",
		.passwd = ""
	};

	if(strcmp(tmp, "") != 0){
		commandMode(&nvsHandle, &wifiCrids);
	}

	//======= CONNECT TO/INITIALIZE WIFI & CONNECT TO TCP SERVER =======

	printf("Connecting to WIFI...\n");
	fflush(stdout);
	esp_err_t status = WIFI_FAILURE;

	//initialize default esp event loop
	ESP_ERROR_CHECK(esp_event_loop_create_default()); // this is required for connectWifi()
	// connect to wireless AP
	status = connectWifi(&wifiCrids);

	if(status == ESP_ERR_WIFI_SSID){

		printf("Wifi SSID field empty. Please reboot and enter them in command mode.\n");
		abort();
	}
	if(status == ESP_ERR_WIFI_PASSWORD){

		printf("Wifi password field empty. Please reboot and enter them in command mode.\n");
		abort();
	}
	if(WIFI_SUCCESS != status) {
		ESP_LOGI(TAG, "Failed to associate to AP, dying...\n");
		abort();
	}
	
	printf("Connecting to server...\n");
	fflush(stdout);

	// == RESOLVE IP OVER mDNS ==

	ret = mdns_init();

	mdns_result_t * result = NULL;

	do{
		ret = mdns_query_ptr(MDNS_SERVICE_TYPENAME, "_tcp", 3000, 1,  &result);
		if(ret){
			printf("mDNS query Failed, Trying again.\n");
		}
		if(!result){
			printf("No results found through mDNS! Trying again.\n");
		}
	}while(ret || !result);

	printf("Connecting to the following server:\n");
	mdns_print_result(result);

	int32_t socketfd = 0;

	uint32_t address = result->addr->next->addr.u_addr.ip4.addr;

	printf("ip4 address: %"PRIu32" \n", address);
	status = connect_tcp_server(&socketfd, &address, htons(result->port));
	mdns_query_results_free(result);

	if(TCP_SUCCESS != status){
		ESP_LOGI(TAG, "Failed to connect to remote server, dying...\n");
		abort();
	}

	// Restore Device ID (assigned from server), from nvs
	uint32_t deviceID;
	uint32_t recvBuff[64];
	char sendBuff[64];

	ret = nvs_get_u32(nvsHandle, "devID", &deviceID);
	printf("ID from nvs: %"PRIu32"\n", deviceID);

	switch(ret){

		case ESP_OK: break;
		case ESP_ERR_NVS_NOT_FOUND: {

			deviceID = 0;
			break;
		}
		default: {
			printf("WTF, default case on nvs?\n");
			abort();
			break;
		}
	}

	sprintf(sendBuff, "%"PRIu32":%s\n", deviceID, DEVICE_TYPE);

	// send existing id to server
	uint32_t bytes_sent = 0;
	while(bytes_sent < strlen(sendBuff)){
		int32_t socketStatus = send(socketfd, sendBuff + bytes_sent, strlen(sendBuff), 0);
		if(socketStatus == -1){
			break;
		}else{
			bytes_sent += socketStatus;
		}
	}

	printf("ID to server: %s\n", sendBuff);

	// receive a new ID or an echo of the current one
	read(socketfd, recvBuff, 64);

	printf("ID from server: %"PRIu32"\n", *recvBuff);

	ret = nvs_set_u32(nvsHandle, "devID", *recvBuff);
	ESP_ERROR_CHECK(ret);

	int32_t socketStatus = 0;
	while(socketStatus != -1){

		fflush(stdout);

		char sendbuff[256] = {0};
		bool isOpenEvent = false;
		bool isCloseEvent = false;

		// TODO: add sensor stuff here

		if(isOpenEvent){
			sprintf(sendBuff, "TRUE\n");
		}else if (isCloseEvent) {
			sprintf(sendBuff, "FALSE\n");
		}else{
			sprintf(sendBuff, "NONE\n");
		}

		printf("==================================================================================\n");
		fflush(stdout);

		uint32_t bytes_sent = 0;
		while(bytes_sent < strlen(sendBuff)){
			socketStatus = send(socketfd, sendbuff + bytes_sent, strlen(sendbuff), 0);
			if(socketStatus == -1){
				break;
			}else{
				bytes_sent += socketStatus;
			}
		}

		vTaskDelay(2000/portTICK_PERIOD_MS);
	}
	close(socketfd);
	abort();
}
