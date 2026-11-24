#include <assert.h>
#include <math.h>
#include <stdio.h>
static unsigned ledLatches;
void boardLatchLed(void){++ledLatches;}
#include "audio.c"
static bool pollNextBlock(AudioWindow *w){
 blockReady=true;++micDmaBlocks;
 return audioPollWindow(w);
}
int main(void){
 AudioWindow w={0};
 for(unsigned block=0;block<8;block++){
  for(unsigned i=0;i<32;i++){
   int sign=(i&1)?1:-1;
   buffers[0][2*i]=(100+sign)|((uint32_t)(200+sign*2)<<16);
   buffers[0][2*i+1]=(300+sign*3)|((uint32_t)4000<<16);
  }
  readyBuffer=0;blockReady=true;++micDmaBlocks;
  assert(audioPollWindow(&w)==(block==7));
 }
 assert(w.opa.mean==100 && w.raw.mean==200 && w.gpamp.mean==300);
 assert(w.opa.rms==1 && w.raw.rms==2 && w.gpamp.rms==3);
 assert(w.gpamp.minimum==297 && w.gpamp.maximum==303);
 assert(w.timeMs==16 && micWindows==1);
 for (unsigned mode = AUDIO_CAPTURE_OPA; mode <= AUDIO_CAPTURE_ALL; ++mode) {
  const unsigned blocks = mode == AUDIO_CAPTURE_ALL ? 4 : 16;
  audioCaptureStart((AudioCaptureMode)mode);
  blockReady = true;
  ++micDmaBlocks;
  audioPollWindow(&w);
  ++micDmaBlocks; // Drop one block after starting the capture.
  for (unsigned block = 0; block < blocks; ++block) {
   assert(audioCaptureResult() == NULL);
   blockReady = true;
   ++micDmaBlocks;
   audioPollWindow(&w);
  }
  const uint16_t *result = audioCaptureResult();
  assert(result != NULL);
  for (unsigned i = 0; i < AUDIO_CAPTURE_SAMPLES; ++i) {
   const unsigned channel = mode == AUDIO_CAPTURE_ALL ? i % 4 : mode;
   const unsigned frame = mode == AUDIO_CAPTURE_ALL ? i / 4 : i;
   const int sign = (frame & 1) ? 1 : -1;
   const int expected[] = {100+sign, 200+sign*2, 300+sign*3, 4000};
   assert(result[i] == expected[channel]);
  }
 }
 puts("PASS: all raw capture modes and restart after a DMA gap");
 puts("PASS: DMA decoding, channel order, padding discard, RMS and timestamp");

 // Windows end on every eighth DMA block. One that lost a block is dropped.
 while (micDmaBlocks % 8) pollNextBlock(&w);
 uint32_t windows = micWindows;
 ++micDmaBlocks; // Lose the first block of the next window.
 for (unsigned block = 0; block < 7; ++block) assert(!pollNextBlock(&w));
 assert(micDmaBlocks % 8 == 0 && micWindows == windows);
 for (unsigned block = 0; block < 8; ++block)
  assert(pollNextBlock(&w) == (block == 7));
 assert(micWindows == windows + 1 && w.opa.rms == 1);
 assert(w.timeMs == micDmaBlocks * 2);
 puts("PASS: block-aligned windows and discard of a short window");

 // The interrupt applies the pending LED duty only between two windows.
 testDmaPending = DL_DMA_EVENT_IIDX_DMACH0;
 ledLatches = 0;
 for (unsigned block = 1; block <= 24; ++block) {
  DMA_IRQHandler();
  assert(ledLatches == block / 8);
  assert(micDmaBlocks % 8 == block % 8);
 }
 puts("PASS: LED duty latched at window boundaries only");

 // The amplifier level is slot 0 alone, or the average of slots 0, 2 and 3.
 readyBuffer = 0; // The interrupt test left the empty buffer selected.
 audioSetAveraging(false);
 for (unsigned block = 0; block < 8; ++block)
  assert(pollNextBlock(&w) == (block == 7));
 assert(w.amp.mean == 100 && w.amp.rms == 1);
 assert(w.amp.minimum == 99 && w.amp.maximum == 101);
 pollNextBlock(&w);
 audioSetAveraging(true); // Mid-window: that window must be dropped.
 for (unsigned block = 0; block < 7; ++block) assert(!pollNextBlock(&w));
 for (unsigned block = 0; block < 8; ++block)
  assert(pollNextBlock(&w) == (block == 7));
 // Per frame: (100+s) + (300+3s) + 4000 with s = +/-1, divided by three.
 assert(fabsf(w.amp.mean - 4400.0f / 3) < 1e-3f);
 assert(fabsf(w.amp.rms - 4.0f / 3) < 1e-4f);
 assert(w.amp.minimum == 4396 / 3 && w.amp.maximum == 4404 / 3);
 assert(w.opa.mean == 100 && w.gpamp.mean == 300);
 puts("PASS: single and three-conversion amplifier level");

 // Self-test levels: slot 0, slot 1, their average, slot 2, slot 3, average.
 audioSetTestMode(true);
 for (unsigned block = 0; block < 8; ++block)
  assert(pollNextBlock(&w) == (block == 7));
 // Per frame: 100+s, 200+2s, 300+3s and a constant 4000, with s = +/-1.
 assert(w.testRms[0] == 1 && w.testRms[1] == 2 && w.testRms[2] == 1.5f);
 assert(w.testRms[3] == 3 && w.testRms[4] == 0 && w.testRms[5] == 1.5f);
 puts("PASS: self-test levels per slot and per pair");
}
