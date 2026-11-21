#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "audio.h"
volatile uint32_t micMissedBlocks, micAdcOverruns, micTriggerOverruns;
static int rx=-1; static char output[1024]; static unsigned used;
bool DL_UART_Main_isRXFIFOEmpty(int u){(void)u;return rx<0;}
uint8_t DL_UART_Main_receiveData(int u){(void)u;int c=rx;rx=-1;return c;}
bool DL_UART_Main_isTXFIFOFull(int u){(void)u;return false;}
void DL_UART_Main_transmitData(int u,char c){(void)u;output[used++]=c;output[used]=0;}
static bool enabled;
void boardGpampEnable(bool e){enabled=e;}
void menuDebugPoll(void); void menuDebugMeasure(const AudioWindow*);
int main(void){
 AudioWindow w={.opa={1400,3,1390,1410},.raw={1401,4,1380,1420},.gpamp={1402,5,1370,1430},.timeMs=256};
 for(int i=0;i<16;i++)menuDebugMeasure(&w);
 menuDebugPoll();assert(used==0);
 rx='M';menuDebugPoll();assert(enabled);
 for(int i=0;i<64;i++)menuDebugMeasure(&w);
 menuDebugPoll();assert(used==0);
 for(int i=0;i<15;i++)menuDebugMeasure(&w);
 menuDebugPoll();assert(used==0);
 menuDebugMeasure(&w);menuDebugPoll();
 assert(!strcmp(output,"N2,1,256,1400000,3000,3000,1390,1410,1401000,4000,4000,1380,1420,1402000,5000,5000,1370,1430,0,0,0,0\r\n"));
 unsigned previous=used;rx='X';menuDebugPoll();assert(!enabled);
 for(int i=0;i<16;i++)menuDebugMeasure(&w);
 menuDebugPoll();assert(used==previous);
 puts("PASS: GPAMP enable/disable, startup settling and three-channel CSV");
}
