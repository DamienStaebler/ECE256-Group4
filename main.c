#include <stdint.h>
#include <math.h>

/* Added for UART + shift register */
#include <stdbool.h>
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_ints.h"
#include "driverlib/gpio.h"
#include "driverlib/uart.h"
#include "driverlib/pin_map.h"
#include "driverlib/interrupt.h"
#include "driverlib/sysctl.h"
#include "driverlib/udma.h"

#define SYSCLK 16000000 // 16 MHz default clock

// System control registers
#define SYSCTL_RCGCGPIO_R (*((volatile uint32_t *)0x400FE608))
#define SYSCTL_RCGCPWM_R  (*((volatile uint32_t *)0x400FE640))

// DMA & UART DMA Registers
#define SYSCTL_RCGCDMA_R  (*((volatile uint32_t *)0x400FE60C))
#define UDMA_CFG_R        (*((volatile uint32_t *)0x400FF004))
#define UDMA_CTLBASE_R    (*((volatile uint32_t *)0x400FF008))
#define UDMA_CHMAP1_R     (*((volatile uint32_t *)0x400FF514))
#define UDMA_ENASET_R     (*((volatile uint32_t *)0x400FF028))
#define UART0_DMACTL_R    (*((volatile uint32_t *)0x4000C048))
#define UART0_DR_R        (*((volatile uint32_t *)0x4000C000))
#define UART0_IM_R        (*((volatile uint32_t *)0x4000C038))

// GPIO Port B registers (base: 0x40005000)
#define GPIO_PORTB_AFSEL_R (*((volatile uint32_t *)0x40005420))
#define GPIO_PORTB_DEN_R   (*((volatile uint32_t *)0x4000551C))
#define GPIO_PORTB_PCTL_R  (*((volatile uint32_t *)0x4000552C))

// Port F pins (LED: PF1=Red, PF2=Blue, PF3=Green)
#define GPIO_PORTF_DATA_R (*((volatile uint32_t *)0x400253FC))
#define GPIO_PORTF_DIR_R  (*((volatile uint32_t *)0x40025400))
#define GPIO_PORTF_DEN_R  (*((volatile uint32_t *)0x4002551C))

// PWM Module 0, Generator 0 registers
#define PWM0_ENABLE_R  (*((volatile uint32_t *)0x40028008))
#define PWM0_0_CTL_R   (*((volatile uint32_t *)0x40028040))
#define PWM0_0_LOAD_R  (*((volatile uint32_t *)0x40028050))
#define PWM0_0_CMPA_R  (*((volatile uint32_t *)0x40028058))
#define PWM0_0_GENA_R  (*((volatile uint32_t *)0x40028060))

// SysTick registers
#define NVIC_ST_CTRL_R    (*((volatile uint32_t *)0xE000E010))
#define NVIC_ST_RELOAD_R  (*((volatile uint32_t *)0xE000E014))
#define NVIC_ST_CURRENT_R (*((volatile uint32_t *)0xE000E018))
#define SYSCTL_RCC_R      (*((volatile uint32_t *)0x400FE060))

#define SYSCTL_PRGPIO_R (*((volatile uint32_t *)0x400FEA08))
#define SYSCTL_PRPWM_R  (*((volatile uint32_t *)0x400FEA40))

#define TABLE_SIZE 32
#define MAX 15

#define q    400   // Quarter note
#define dq   600   // Dotted quarter
#define h    800   // Half note
#define h_d  1200  // Dotted half (full measure)

// --- Shift register pin definitions (PB0/1/2) ---
#define SHIFT_PERIPH  SYSCTL_PERIPH_GPIOB
#define SHIFT_PORT    GPIO_PORTB_BASE
#define DATA_PIN      GPIO_PIN_0   // DS
#define CLOCK_PIN     GPIO_PIN_1   // SH_CP
#define LATCH_PIN     GPIO_PIN_2   // ST_CP

// μDMA channel control table (must be 1024-byte aligned)
uint8_t dmaCtrlTable[1024] __attribute__((aligned(1024)));
uint8_t rxBuf[256];
volatile bool dmaComplete = false;

// Flag so ISR avoids blocking TX; main() drains it
volatile const char *txPending = 0;

// Timers
void SysTick_Init(void);
void SysTick_Handler(void);
void Wait_ms(uint32_t ms);

// Init
void PWM_Init(void);
void PortF_Init(void);
void ShiftReg_Init(void);
void UART0_Init(void);
void DMA_Init(void);

// UART
void UART0_ISR(void);
void UART0_SendString(const char *str);

// Shift register
void shiftOut(uint8_t data);
void latch(void);

void Set_LED(uint8_t color);

// PAUSED/IDLE cases can cleanly override it without fighting note().
void note(int note_val, int duration);

volatile float tableIndex = 0;
volatile float STEP = 0;
volatile uint32_t ms_ticks = 0;
volatile uint32_t fixedTableIndex = 0;
volatile uint32_t fixedSTEP = 0;

uint8_t sineTable[TABLE_SIZE] = {
    7, 8, 10, 11, 12, 13, 14, 14, 15,
    14, 14, 13, 12, 11, 10, 8, 7, 6,
    4, 3, 2, 1, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6
};

typedef enum {
    REST = 0,
    C = 40, Cs, D = 42, Ds, E = 44, F = 45, Fs, G = 47, Gs, A = 49, As, B = 51
} Pitch_t;

typedef struct {
    Pitch_t  pitch;
    uint16_t duration;
    uint8_t  bitstring;   // shift register LED pattern for this note
} Note_t;

// Bit-string key:
//   A - 0b00011100
//   C - 0b00101010
//   D - 0b00110010
//   E - 0b00100110
//   F - 0b00100010
//   G - 0b00010100

Note_t twinklestar[] = {
    // Phrase 1: Twin-kle, twin-kle, lit-tle star
    {C, q,  0b00101010}, {C, q,  0b00101010},
    {G, q,  0b00010100}, {G, q,  0b00010100},
    {A, q,  0b00011100}, {A, q,  0b00011100},
    {G, h,  0b00010100},

    // Phrase 2: How I won-der what you are
    {F, q,  0b00100010}, {F, q,  0b00100010},
    {E, q,  0b00100110}, {E, q,  0b00100110},
    {D, q,  0b00110010}, {D, q,  0b00110010},
    {C, h,  0b00101010},

    // Phrase 3: Up a-bove the world so high
    {G, q,  0b00010100}, {G, q,  0b00010100},
    {F, q,  0b00100010}, {F, q,  0b00100010},
    {E, q,  0b00100110}, {E, q,  0b00100110},
    {D, h,  0b00110010},

    // Phrase 4: Like a dia-mond in the sky
    {G, q,  0b00010100}, {G, q,  0b00010100},
    {F, q,  0b00100010}, {F, q,  0b00100010},
    {E, q,  0b00100110}, {E, q,  0b00100110},
    {D, h,  0b00110010},

    // Phrase 5: Twin-kle, twin-kle, lit-tle star (Reprise)
    {C, q,  0b00101010}, {C, q,  0b00101010},
    {G, q,  0b00010100}, {G, q,  0b00010100},
    {A, q,  0b00011100}, {A, q,  0b00011100},
    {G, h,  0b00010100},

    // Phrase 6: How I won-der what you are (Reprise)
    {F, q,  0b00100010}, {F, q,  0b00100010},
    {E, q,  0b00100110}, {E, q,  0b00100110},
    {D, q,  0b00110010}, {D, q,  0b00110010},
    {C, h,  0b00101010}
};

Note_t jinglebells[] = {
    {E, q,  0b00100110}, {E, q,  0b00100110}, {E, h,  0b00100110},
    {E, q,  0b00100110}, {E, q,  0b00100110}, {E, h,  0b00100110},
    {E, q,  0b00100110}, {G, q,  0b00010100},
    {C, q,  0b00101010}, {D, q,  0b00110010}, {E, h,  0b00100110},
    {F, q,  0b00100010}, {F, q,  0b00100010},
    {F, q,  0b00100010}, {F, q,  0b00100010},
    {F, q,  0b00100010}, {E, q,  0b00100110},
    {E, q,  0b00100110}, {E, q,  0b00100110},
    {G, q,  0b00010100}, {G, q,  0b00010100},
    {F, q,  0b00100010}, {D, q,  0b00110010}, {C, h,  0b00101010}
};

Note_t silentnight[] = {
    // Si-lent night, ho-ly night
    {G,  dq, 0b00010100}, {A,  q,  0b00011100}, {G,  h,   0b00010100},
    {G,  dq, 0b00010100}, {A,  q,  0b00011100}, {G,  h,   0b00010100},

    // All is calm, all is bright
    {D,  h,  0b00110010}, {D,  h,  0b00110010},
    {B,  h,  0b00101010}, {B,  h,  0b00101010},

    // Round yon vir-gin moth-er and child
    {A,  h,  0b00011100}, {A,  h,  0b00011100},
    {C,  h,  0b00101010}, {C,  h,  0b00101010},

    // Ho-ly in-fant so ten-der and mild
    {C,  q,  0b00101010}, {G,  q,  0b00010100}, {E,  q,   0b00100110},
    {G,  dq, 0b00010100}, {F,  q,  0b00100010}, {D,  h,   0b00110010},

    // Sleep in heav-en-ly peace
    {C,  h_d, 0b00101010},
    {G,  h_d, 0b00010100},
    {E,  h_d, 0b00100110}
};

typedef enum { IDLE, PLAYING, PAUSED } State_t;
volatile State_t currentState = IDLE;
const uint8_t colors[] = {0x08, 0x04, 0x02, 0x0C, 0x0A, 0x0E};

volatile Note_t *currentMelody    = twinklestar;
volatile int     currentMelodySize = sizeof(twinklestar) / sizeof(Note_t);
volatile bool    songChanged       = false;

int main(void) {
    *((volatile uint32_t *)0xE000ED88) |= ((3UL << 20) | (3UL << 22));

    SysCtlClockSet(SYSCTL_SYSDIV_1 | SYSCTL_USE_OSC | SYSCTL_OSC_MAIN | SYSCTL_XTAL_16MHZ);

    SysTick_Init();
    PWM_Init();
    PortF_Init();
    ShiftReg_Init();
    UART0_Init();
    DMA_Init();

    // Clear shift register on startup
    shiftOut(0x00);
    latch();

    UART0_SendString("\r\nPAUSED\r\n");

    int melody_idx = 0;

    while (1) {
        // Drain pending TX message (never block inside ISR)
        if (txPending) { UART0_SendString((const char *)txPending); txPending = 0; }

        // Reset index whenever the song changes
        if (songChanged) {
            melody_idx = 0;
            songChanged = false;
        }

        switch (currentState) {
            case IDLE:
                Set_LED(0x02); // Red
                shiftOut(0b00111110); latch();
                break;

            case PLAYING:
                if (melody_idx < currentMelodySize) {
                    uint8_t c = colors[(melody_idx / 7) % 6];
                    Set_LED(c);

                    //Fixed the shiftout of the bitstring moving it from the note call
                    shiftOut(currentMelody[melody_idx].bitstring);
                    latch();

                    note(currentMelody[melody_idx].pitch,
                         currentMelody[melody_idx].duration);

                    // Only advance if we weren't interrupted
                    if (currentState == PLAYING && !songChanged) {
                        melody_idx++;
                    }
                } else {
                    melody_idx = 0;
                }
                break;

            case PAUSED:
                Set_LED(0x0E); // White
                shiftOut(0x00); latch(); // All shift register LEDs off when paused
                break;
        }
    }
}

void SysTick_Init(void) {
    NVIC_ST_CTRL_R = 0;
    NVIC_ST_RELOAD_R = (SYSCLK / 8000) - 1;
    NVIC_ST_CURRENT_R = 0;
    NVIC_ST_CTRL_R = 0x07;
}

void SysTick_Handler(void) {
    if (fixedTableIndex >= (TABLE_SIZE << 16)) {
        fixedTableIndex -= (TABLE_SIZE << 16);
    }

    uint32_t index = (fixedTableIndex >> 16);
    PWM0_0_CMPA_R = PWM0_0_LOAD_R - (PWM0_0_LOAD_R * sineTable[index & 0x1F] / MAX);

    fixedTableIndex += fixedSTEP;

    static uint8_t prescaler = 0;
    if (++prescaler >= 8) {
        ms_ticks++;
        prescaler = 0;
    }
}

// Exits early if state leaves PLAYING so the UI stays responsive
void Wait_ms(uint32_t ms) {
    uint32_t start = ms_ticks;
    while ((ms_ticks - start) < ms) {
        if (currentState != PLAYING) return;
    }
}

void PWM_Init(void) {
    SYSCTL_RCGCPWM_R |= 0x01;
    SYSCTL_RCGCGPIO_R |= 0x02;
    while ((SYSCTL_PRPWM_R  & 0x01) == 0);
    while ((SYSCTL_PRGPIO_R & 0x02) == 0);

    SYSCTL_RCC_R |=  0x00100000;
    SYSCTL_RCC_R &= ~0x000E0000;

    GPIO_PORTB_AFSEL_R |= 0x40;
    GPIO_PORTB_PCTL_R = (GPIO_PORTB_PCTL_R & 0xF0FFFFFF) | 0x04000000;
    GPIO_PORTB_DEN_R  |= 0x40;

    PWM0_0_CTL_R  = 0;
    PWM0_0_GENA_R = 0x8C;
    PWM0_0_LOAD_R = 400 - 1;
    PWM0_0_CMPA_R = 200;
    PWM0_0_CTL_R  = 1;
    PWM0_ENABLE_R |= 0x01;
}

void PortF_Init(void) {
    SYSCTL_RCGCGPIO_R |= 0x20;
    while ((SYSCTL_PRGPIO_R & 0x20) == 0);

    GPIO_PORTF_DIR_R |= 0x0E;
    GPIO_PORTF_DEN_R |= 0x0E;
}

void ShiftReg_Init(void) {
    while (!SysCtlPeripheralReady(SHIFT_PERIPH)) {}
    GPIOPinTypeGPIOOutput(SHIFT_PORT, DATA_PIN | CLOCK_PIN | LATCH_PIN);
    GPIOPinWrite(SHIFT_PORT, DATA_PIN | CLOCK_PIN | LATCH_PIN, 0);
}

void UART0_Init(void) {
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);

    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOA)) {}
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_UART0)) {}

    GPIOPinConfigure(GPIO_PA0_U0RX);
    GPIOPinConfigure(GPIO_PA1_U0TX);
    GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    UARTConfigSetExpClk(UART0_BASE, SysCtlClockGet(), 115200,
                        (UART_CONFIG_WLEN_8 |
                         UART_CONFIG_STOP_ONE |
                         UART_CONFIG_PAR_NONE));

    UARTIntEnable(UART0_BASE, UART_INT_RT);
    IntEnable(INT_UART0);
    IntMasterEnable();
    UARTEnable(UART0_BASE);
}

void HandleCommand(char cmd) {
    switch (cmd) {
        case 'p': // Play/Pause toggle
            if (currentState == PLAYING) {
                currentState = PAUSED;
                fixedSTEP = 0;
                txPending = "\r\nPAUSED\r\n";
            } else {
                currentState = PLAYING;
                txPending = "\r\nPLAYING\r\n";
            }
            break;

        case 't': // Twinkle Twinkle
            currentMelody     = twinklestar;
            currentMelodySize = sizeof(twinklestar) / sizeof(Note_t);
            songChanged       = true;
            currentState      = PLAYING;
            txPending         = "\r\nSONG: TWINKLE\r\n";
            break;

        case 's': // Silent Night
            currentMelody     = silentnight;
            currentMelodySize = sizeof(silentnight) / sizeof(Note_t);
            songChanged       = true;
            currentState      = PLAYING;
            txPending         = "\r\nSONG: SILENT NIGHT\r\n";
            break;

        case 'j': // Jingle Bells
            currentMelody     = jinglebells;
            currentMelodySize = sizeof(jinglebells) / sizeof(Note_t);
            songChanged       = true;
            currentState      = PLAYING;
            txPending         = "\r\nSONG: JINGLE BELLS\r\n";
            break;
    }
}

void UART0_ISR(void) {
    uint32_t status = UARTIntStatus(UART0_BASE, true);
    UARTIntClear(UART0_BASE, status);

    if (status & UART_INT_DMARX) {
        if (!uDMAChannelIsEnabled(UDMA_CH8_UART0RX)) {
            HandleCommand(rxBuf[0]);
            rxBuf[0] = 0;

            uDMAChannelTransferSet(UDMA_CH8_UART0RX | UDMA_PRI_SELECT,
                                   UDMA_MODE_BASIC,
                                   (void *)&UART0_DR_R,
                                   rxBuf,
                                   1);
            uDMAChannelEnable(UDMA_CH8_UART0RX);
        }
    }

    // Drain any bytes the DMA missed on receive timeout
    if (status & UART_INT_RT) {
        while (UARTCharsAvail(UART0_BASE)) {
            HandleCommand((char)UARTCharGetNonBlocking(UART0_BASE));
        }
    }
}

void UART0_SendString(const char *str) {
    while (*str) UARTCharPut(UART0_BASE, *str++);
}

void Set_LED(uint8_t color) {
    GPIO_PORTF_DATA_R = (GPIO_PORTF_DATA_R & ~0x0E) | (color & 0x0E);
}

// note() only handles audio — shift register is the caller's responsibility
void note(int note_val, int duration) {
    if (note_val == 0) {
        fixedSTEP = 0;
    } else {
        float freq     = 440.0f * powf(2.0f, (note_val - 49.0f) / 12.0f);
        float floatStep = (freq * (float)TABLE_SIZE) / 8000.0f;
        fixedSTEP      = (uint32_t)(floatStep * 65536.0f);
    }
    Wait_ms(duration);
    // always zero the step so audio stops even if Wait_ms exited early
    fixedSTEP = 0;
    Wait_ms(50);
}

void shiftOut(uint8_t data) {
    GPIOPinWrite(SHIFT_PORT, CLOCK_PIN, 0);
    int i;
    for (i = 7; i >= 0; i--) {
        GPIOPinWrite(SHIFT_PORT, CLOCK_PIN, 0);
        GPIOPinWrite(SHIFT_PORT, DATA_PIN,
                     (data & (1 << i)) ? DATA_PIN : 0);
        GPIOPinWrite(SHIFT_PORT, CLOCK_PIN, CLOCK_PIN);
    }
    GPIOPinWrite(SHIFT_PORT, CLOCK_PIN | DATA_PIN, 0);
}

void latch(void) {
    GPIOPinWrite(SHIFT_PORT, LATCH_PIN, 0);
    GPIOPinWrite(SHIFT_PORT, LATCH_PIN, LATCH_PIN);
    GPIOPinWrite(SHIFT_PORT, LATCH_PIN, 0);
}

void DMA_Init(void) {
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UDMA);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_UDMA));

    uDMAEnable();
    uDMAControlBaseSet(dmaCtrlTable);

    uDMAChannelAttributeDisable(UDMA_CH8_UART0RX, UDMA_ATTR_ALL);

    uDMAChannelControlSet(UDMA_CH8_UART0RX | UDMA_PRI_SELECT,
                          UDMA_SIZE_8 | UDMA_SRC_INC_NONE | UDMA_DST_INC_8 | UDMA_ARB_1);

    uDMAChannelTransferSet(UDMA_CH8_UART0RX | UDMA_PRI_SELECT,
                           UDMA_MODE_BASIC,
                           (void *)&UART0_DR_R,
                           rxBuf,
                           1);

    uDMAChannelEnable(UDMA_CH8_UART0RX);
    UARTDMAEnable(UART0_BASE, UART_DMA_RX);

    // Arm DMARX interrupt only after DMA is fully configured
    UARTIntEnable(UART0_BASE, UART_INT_DMARX);
}
