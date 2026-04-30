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

#define SYSCLK 16000000 // 16 MHz default clock

// System control registers
#define SYSCTL_RCGCGPIO_R (*((volatile uint32_t *)0x400FE608))
#define SYSCTL_RCGCPWM_R  (*((volatile uint32_t *)0x400FE640))

// GPIO Port B registers (base: 0x40005000)
#define GPIO_PORTB_AFSEL_R (*((volatile uint32_t *)0x40005420))
#define GPIO_PORTB_DEN_R   (*((volatile uint32_t *)0x4000551C))
#define GPIO_PORTB_PCTL_R  (*((volatile uint32_t *)0x4000552C))

// Port F pins (LED: PF1=Red, PF2=Blue, PF3=Green)
// SW1 (PF4) removed — state now controlled via UART
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

#define q 400
#define h 800

// --- Shift register pin definitions (PB0/1/2) ---
#define SHIFT_PERIPH  SYSCTL_PERIPH_GPIOB
#define SHIFT_PORT    GPIO_PORTB_BASE
#define DATA_PIN      GPIO_PIN_0   // DS
#define CLOCK_PIN     GPIO_PIN_1   // SH_CP
#define LATCH_PIN     GPIO_PIN_2   // ST_CP

// Timers
void SysTick_Init(void);
void SysTick_Handler(void);
void Wait_ms(uint32_t ms);

// Init
void PWM_Init(void);
void PortF_Init(void);
void ShiftReg_Init(void);   // Added
void UART0_Init(void);      // Added

// UART
void UART0_ISR(void);                   // Added
void UART0_SendString(const char *str); // Added

// Shift register
void shiftOut(uint8_t data); // Added
void latch(void);            // Added

void Set_LED(uint8_t color);
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
    Pitch_t pitch;
    uint16_t duration;
} Note_t;

Note_t twinklestar[] = {
    {C,q},{C,q},{G,q},{G,q},{A,q},{A,q},{G,h},
    {F,q},{F,q},{E,q},{E,q},{D,q},{D,q},{C,h},
    {G,q},{G,q},{F,q},{F,q},{E,q},{E,q},{D,h},
    {G,q},{G,q},{F,q},{F,q},{E,q},{E,q},{D,h},
    {C,q},{C,q},{G,q},{G,q},{A,q},{A,q},{G,h},
    {F,q},{F,q},{E,q},{E,q},{D,q},{D,q},{C,h}
};

typedef enum { IDLE, PLAYING, PAUSED } State_t;
volatile State_t currentState = IDLE;

int main(void) {
    *((volatile uint32_t *)0xE000ED88) |= ((3UL << 20) | (3UL << 22));

    SysTick_Init();
    PWM_Init();
    PortF_Init();
    ShiftReg_Init();  // Added — PB0/1/2 outputs (PB6 already set by PWM_Init)
    UART0_Init();     // Added

    // Clear shift register on startup
    shiftOut(0x00);
    latch();

    UART0_SendString("\r\n=== Twinkle Star Player ===\r\n");
    UART0_SendString("Press any key to start/pause/resume.\r\n\r\n");

    int melody_idx = 0;
    int total_notes = sizeof(twinklestar) / sizeof(Note_t);
    int phrase;

    while (1) {
        // Button removed — state is now set by UART ISR

        switch (currentState) {
            case IDLE:
                Set_LED(0x02); // Red
                shiftOut(0x00); latch(); // All shift register LEDs off
                break;

            case PLAYING:
                phrase = melody_idx / 7;
                uint8_t colors[] = {0x08, 0x04, 0x02, 0x0C, 0x0A, 0x0E};
                uint8_t c = colors[phrase % 6];
                Set_LED(c);
                shiftOut(c); latch(); // Mirror phrase color to shift register

                if (melody_idx < total_notes) {
                    note(twinklestar[melody_idx].pitch, twinklestar[melody_idx].duration);
                    if (currentState == PLAYING) {
                        melody_idx++;
                    }
                } else {
                    melody_idx = 0;
                }
                break;

            case PAUSED:
                Set_LED(0x0E); // White
                shiftOut(0x3F); latch(); // All shift register LEDs on when paused
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
    uint32_t index = (fixedTableIndex >> 16);
    PWM0_0_CMPA_R = PWM0_0_LOAD_R - (PWM0_0_LOAD_R * sineTable[index] / MAX);

    fixedTableIndex += fixedSTEP;
    if (fixedTableIndex >= (TABLE_SIZE << 16))
        fixedTableIndex -= (TABLE_SIZE << 16);

    static uint8_t prescaler = 0;
    if (++prescaler >= 8) {
        ms_ticks++;
        prescaler = 0;
    }
}

// Wait_ms — button polling removed; exits early if state leaves PLAYING
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

    GPIO_PORTF_DIR_R |= 0x0E;   // PF1/2/3 outputs
    // PF4 (SW1) direction + pull-up removed
    GPIO_PORTF_DEN_R |= 0x0E;   // Digital enable PF1/2/3 only
}

// ShiftReg_Init — Port B clock already on from PWM_Init; just set PB0/1/2
void ShiftReg_Init(void) {
    while (!SysCtlPeripheralReady(SHIFT_PERIPH)) {}
    GPIOPinTypeGPIOOutput(SHIFT_PORT, DATA_PIN | CLOCK_PIN | LATCH_PIN);
    GPIOPinWrite(SHIFT_PORT, DATA_PIN | CLOCK_PIN | LATCH_PIN, 0);
}

void UART0_Init(void) {
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_UART0)) {}

    GPIOPinConfigure(GPIO_PA0_U0RX);
    GPIOPinConfigure(GPIO_PA1_U0TX);
    GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    UARTConfigSetExpClk(UART0_BASE, SysCtlClockGet(), 115200,
                        (UART_CONFIG_WLEN_8 |
                         UART_CONFIG_STOP_ONE |
                         UART_CONFIG_PAR_NONE));

    UARTIntEnable(UART0_BASE, UART_INT_RX | UART_INT_RT);
    UARTIntRegister(UART0_BASE, UART0_ISR);
    IntEnable(INT_UART0);
    IntMasterEnable();
    UARTEnable(UART0_BASE);
}

void UART0_ISR(void) {
    uint32_t status = UARTIntStatus(UART0_BASE, true);
    UARTIntClear(UART0_BASE, status);

    while (UARTCharsAvail(UART0_BASE)) {
        UARTCharGetNonBlocking(UART0_BASE); // consume byte; any key acts as button

        // Mirrors original button behaviour:
        // IDLE -> PLAYING, PLAYING -> PAUSED, PAUSED -> PLAYING
        switch (currentState) {
            case IDLE:
                currentState = PLAYING;
                UART0_SendString("\r\nPLAYING\r\n");
                break;
            case PLAYING:
                currentState = PAUSED;
                fixedSTEP = 0;
                UART0_SendString("\r\nPAUSED\r\n");
                break;
            case PAUSED:
                currentState = PLAYING;
                UART0_SendString("\r\nPLAYING\r\n");
                break;
        }
    }
}

void UART0_SendString(const char *str) {
    while (*str) UARTCharPut(UART0_BASE, *str++);
}

void Set_LED(uint8_t color) {
    GPIO_PORTF_DATA_R = (GPIO_PORTF_DATA_R & ~0x0E) | (color & 0x0E);
}

void note(int note_val, int duration) {
    if (note_val == 0) {
        fixedSTEP = 0;
    } else {
        float freq = 440.0 * pow(2.0, (note_val - 49.0) / 12.0);
        float floatStep = (freq * (float)TABLE_SIZE) / 8000.0;
        fixedSTEP = (uint32_t)(floatStep * 65536.0);
    }
    Wait_ms(duration);
    fixedSTEP = 0;
    Wait_ms(50);
}

void shiftOut(uint8_t data) {
    GPIOPinWrite(SHIFT_PORT, CLOCK_PIN, 0);
    int i = 7;
    for (; i >= 0; i--) {
        GPIOPinWrite(SHIFT_PORT, CLOCK_PIN, 0);
        if (data & (1 << i))
            GPIOPinWrite(SHIFT_PORT, DATA_PIN, DATA_PIN);
        else
            GPIOPinWrite(SHIFT_PORT, DATA_PIN, 0);
        GPIOPinWrite(SHIFT_PORT, CLOCK_PIN, CLOCK_PIN);
    }
    GPIOPinWrite(SHIFT_PORT, CLOCK_PIN | DATA_PIN, 0);
}

void latch(void) {
    GPIOPinWrite(SHIFT_PORT, LATCH_PIN, 0);
    GPIOPinWrite(SHIFT_PORT, LATCH_PIN, LATCH_PIN);
    GPIOPinWrite(SHIFT_PORT, LATCH_PIN, 0);
}
