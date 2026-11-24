#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "audio.h"
volatile uint32_t micMissedBlocks, micAdcOverruns, micTriggerOverruns;
static int rx=-1; static char output[8192]; static unsigned used;
bool DL_UART_Main_isRXFIFOEmpty(int u){(void)u;return rx<0;}
uint8_t DL_UART_Main_receiveData(int u){(void)u;int c=rx;rx=-1;return c;}
bool DL_UART_Main_isTXFIFOFull(int u){(void)u;return false;}
void DL_UART_Main_transmitData(int u,char c){(void)u;output[used++]=c;output[used]=0;}
bool DL_UART_Main_isBusy(int u){(void)u;return false;}
void audioCaptureStart(AudioCaptureMode mode){(void)mode;}
void audioCaptureCancel(void){}
const uint16_t *audioCaptureResult(void){return NULL;}
void boardLedPwmEnable(bool enable){(void)enable;}
static bool enabled;
void boardGpampEnable(bool e){enabled=e;}
static int adcGpamp=-1, averaging=-1, sensitivity=50;
void boardSelectAmplifier(bool gpamp){adcGpamp=gpamp;}
void audioSetAveraging(bool on){averaging=on;}
uint8_t aeqAdjustSensitivity(int steps){sensitivity+=5*steps;return sensitivity;}
volatile float micNoiseFloor=0.25f, micInputGain=12.0f;
volatile uint16_t ledDutyPeak;
#include "selftest.h"
static bool testing; static SelftestRecord fake; static const SelftestRecord *results;
void selftestStart(void){testing=true;}
void selftestStop(void){testing=false;}
bool selftestActive(void){return testing;}
const SelftestRecord *selftestResults(void){return results;}
static void send(char c){void menuDebugPoll(void);rx=c;menuDebugPoll();}
static bool replied(const char *expected){
 const unsigned n=strlen(expected);
 return used>=n && !strcmp(output+used-n,expected);
}
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

 // Path select: amplifier channel, averaging, and GPAMP power follow the path.
 send('4');assert(replied("PATH: 4\r\n")&&adcGpamp==1&&averaging==1&&enabled);
 send('X');assert(enabled); // The selected path still needs the GPAMP.
 send('3');assert(replied("PATH: 3\r\n")&&adcGpamp==1&&averaging==0&&enabled);
 send('1');assert(replied("PATH: 1\r\n")&&adcGpamp==0&&averaging==0&&!enabled);
 send('M');assert(enabled);
 send('2');assert(replied("PATH: 2\r\n")&&adcGpamp==0&&averaging==1&&enabled);
 send('X');assert(!enabled);
 // Sensitivity controls.
 send('+');assert(replied("SENS: 55\r\n"));
 send('-');assert(replied("SENS: 50\r\n"));
 send('f');assert(replied("FLOOR: 240\r\n"));
 send('F');send('F');assert(replied("FLOOR: 260\r\n"));
 for(int i=0;i<40;i++)send('f');
 assert(replied("FLOOR: 0\r\n")&&micNoiseFloor==0);
 send('K');assert(replied("GAIN: 13\r\n"));
 send('k');send('k');assert(replied("GAIN: 11\r\n"));
 ledDutyPeak=1234;send('?');
 assert(replied("PATH: 2\r\nSENS: 50\r\nFLOOR: 0\r\nGAIN: 11\r\nLEDPK: 1234\r\n"));
 assert(ledDutyPeak==0);
 puts("PASS: path select and sensitivity commands");

 // Self-test commands. Printing sends one line per poll once the queue is empty.
 send('P');for(int i=0;i<4;i++)menuDebugPoll();
 assert(replied("T0,0,0\r\nT9\r\n")); // Nothing stored.
 send('T');assert(testing&&replied("TEST: 1\r\n"));
 send('X');assert(enabled); // The test needs the GPAMP.
 unsigned before=used;send('3');send('+');assert(used==before&&adcGpamp==0);
 fake.intervals=300;
 for(int p=0;p<SELFTEST_PATHS;p++){
  fake.meanRmsMilli[p]=1000+p;
  for(int b=0;b<SELFTEST_BINS;b++)fake.flashIntervals[p][b]=p*100+b;
 }
 results=&fake;before=used;
 send('P');for(int i=0;i<10;i++)menuDebugPoll();
 assert(!strncmp(output+before,"T0,300,1\r\nT1,0,1000,0,1,2,3,",28));
 assert(strstr(output+before,"\r\nT1,5,1005,500,501,"));
 assert(strstr(output+before,",522,523\r\nT9\r\n")&&replied("T9\r\n"));
 send('t');assert(!testing&&replied("PATH: 2\r\nTEST: 0\r\n")&&!enabled);
 puts("PASS: self-test start, stop and result printing");
}
