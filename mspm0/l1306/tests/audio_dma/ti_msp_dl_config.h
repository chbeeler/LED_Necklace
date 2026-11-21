// Host-only peripheral stubs for this test.
#include <stdint.h>
#define AUDIO_DMA_CHAN_ID 0
#define DMA 0
#define AUDIO_ADC_INST 0
#define AUDIO_TIMER_INST 0
#define CPUCLK_FREQ 32000000
#define DL_DMA_INTERRUPT_CHANNEL0 1
#define DMA_INT_IRQn 0
#define AUDIO_ADC_INST_INT_IRQN 1
#define DL_TIMER_CORE_HALT_IMMEDIATE 0
#define DL_DMA_EVENT_IIDX_DMACH0 1
#define DL_ADC12_IIDX_OVERFLOW 1
#define DL_ADC12_IIDX_TRIG_OVERFLOW 2
#define AUDIO_ADC_INST_IRQHandler TestADC_IRQHandler
static inline uint32_t __get_PRIMASK(void){return 0;}
static inline void __set_PRIMASK(uint32_t x){(void)x;}
static inline void __disable_irq(void){}
static inline void __SEV(void){}
#define DL_ADC12_getFIFOAddress(...) ((unsigned)0)
#define DL_ADC12_getPendingInterrupt(...) ((unsigned)0)
#define DL_DMA_clearInterruptStatus(...) ((unsigned)0)
#define DL_DMA_enableChannel(...) ((unsigned)0)
#define DL_DMA_enableInterrupt(...) ((unsigned)0)
#define DL_DMA_getPendingInterrupt(...) ((unsigned)0)
#define DL_DMA_setDestAddr(...) ((unsigned)0)
#define DL_DMA_setSrcAddr(...) ((unsigned)0)
#define DL_DMA_setTransferSize(...) ((unsigned)0)
#define DL_TimerG_setCoreHaltBehavior(...) ((unsigned)0)
#define DL_TimerG_startCounter(...) ((unsigned)0)
#define NVIC_EnableIRQ(...) ((unsigned)0)
#define NVIC_SetPriority(...) ((unsigned)0)
#define delay_cycles(...) ((unsigned)0)

