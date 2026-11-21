#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "audio.c"
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
}
