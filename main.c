#include <stdint.h>
#include <math.h>

#define SYSCLK_HZ 16000000 

// System control registers
#define SYSCTL_RCGCGPIO_R (*((volatile uint32_t *)0x400FE608)) // GPIO clock
#define SYSCTL_RCGCPWM_R (*((volatile uint32_t *)0x400FE640)) // PWM clock

// GPIO Port B registers (base: 0x40005000)
#define GPIO_PORTB_AFSEL_R (*((volatile uint32_t *)0x40005420)) // Alt function
#define GPIO_PORTB_DEN_R (*((volatile uint32_t *)0x4000551C)) // Digital enable
#define GPIO_PORTB_AMSEL_R (*((volatile uint32_t *)0x40005528)) // Analog mode
#define GPIO_PORTB_PCTL_R (*((volatile uint32_t *)0x4000552C)) // Port control

// GPIO Port F registers (base: 0x40025000)
#define GPIO_PORTF_DATA_R (*((volatile uint32_t *)0x400253FC)) // Data
#define GPIO_PORTF_DIR_R (*((volatile uint32_t *)0x40025400)) // Direction
#define GPIO_PORTF_AFSEL_R (*((volatile uint32_t *)0x40025420)) // Alt function
#define GPIO_PORTF_DEN_R (*((volatile uint32_t *)0x4002551C)) // Digital enable

// PWM Module 0, Generator 0 registers (base: 0x40028000)
#define PWM0_ENABLE_R (*((volatile uint32_t *)0x40028008)) // PWM output enable
#define PWM0_0_CTL_R (*((volatile uint32_t *)0x40028040)) // Generator control
#define PWM0_0_LOAD_R (*((volatile uint32_t *)0x40028050)) // Load (period)
#define PWM0_0_CMPA_R (*((volatile uint32_t *)0x40028058)) // Compare A (duty)
#define PWM0_0_GENA_R (*((volatile uint32_t *)0x40028060)) // Generator A action
#define SYSCLK 16000000 // 16 MHz default clock (no PLL)
#define TONE_HZ 440 // Frequency in Hz

// SysTick registers (Cortex-M core)
#define NVIC_ST_CTRL_R (*((volatile uint32_t *)0xE000E010)) // Control/Status
#define NVIC_ST_RELOAD_R (*((volatile uint32_t *)0xE000E014)) // Reload value
#define NVIC_ST_CURRENT_R (*((volatile uint32_t *)0xE000E018)) // Current value
#define SYSCTL_RCC_R (*((volatile uint32_t *)0x400FE060))

#define COUNTFLAG (1U << 16)

#define TABLE_SIZE 32
#define MAX 15

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

void SysTick_Init(void) {
    NVIC_ST_CTRL_R = 0;           // Disable during setup
    NVIC_ST_RELOAD_R = (SYSCLK / 8000) - 1;       
    NVIC_ST_CURRENT_R = 0;        
    // Enable with Core Clock (4) and Interrupt (2) and Enable (1) -> 7
    NVIC_ST_CTRL_R = 0x07;        
}

void SysTick_Handler(void) {
    uint32_t index = (fixedTableIndex >> 16);
    PWM0_0_CMPA_R = PWM0_0_LOAD_R - (PWM0_0_LOAD_R * sineTable[index] / MAX);
    
    fixedTableIndex += fixedSTEP;

    // Wrap around logic for a 32-entry table (32 << 16 = 0x200000)
    if (fixedTableIndex >= (TABLE_SIZE << 16)) {
        fixedTableIndex -= (TABLE_SIZE << 16);
    }

    // Keep your millisecond tick logic
    static uint8_t prescaler = 0;
    if (++prescaler >= 8) {
        ms_ticks++;
        prescaler = 0;
    }
}

void Wait_ms(uint32_t ms) {
    uint32_t start = ms_ticks;
    while ((ms_ticks - start) < ms);
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
    
    uint32_t tempStep = fixedSTEP;
    fixedSTEP = 0;
    Wait_ms(50); 
    fixedSTEP = tempStep;
}

void scale(void) {
    int melody[] = {40, 42, 44, 45, 47, 49, 51, 52};

    int i;
    for (i = 0; i < 8; i++) {
        note(melody[i], 400);
    }
}

void PWM_Init(void) {
    SYSCTL_RCGCGPIO_R |= 0x02; // Enable clock for Port B
    SYSCTL_RCGCPWM_R |= 0x01; // Enable clock for PWM Module 0
    SYSCTL_RCC_R |= 0x00100000; // Use PWM divider
    SYSCTL_RCC_R &= ~0x000E0000;
    
    // Configure PB6 as PWM output (alternate function 4 = M0PWM0)
    GPIO_PORTB_AFSEL_R |= 0x40; // Enable alt function on PB6
    GPIO_PORTB_PCTL_R = (GPIO_PORTB_PCTL_R & 0xF0FFFFFF) | 0x04000000; // AF4
    GPIO_PORTB_DEN_R |= 0x40; // Enable digital I/O on PB6
    
    // PWM generator 0: count down, 440 Hz, 50% duty
    PWM0_0_CTL_R = 0; // Disable during setup
    PWM0_0_GENA_R = 0x8C; // High at LOAD, low at CMPA
    PWM0_0_LOAD_R = (SYSCLK / (2 * 20000)) - 1;
    PWM0_0_CMPA_R = PWM0_0_LOAD_R; //
    PWM0_0_CTL_R = 1; // Enable generator
    PWM0_ENABLE_R |= 0x01; // Enable PWM output on PB6
}

#define C 40
#define D 42
#define E 44
#define F 45
#define G 47
#define A 49
#define b 51

#define q 400
#define h 800

int melody[] =       {C, C, G, G, A, A, G, F, F, E, E, D, D, C,
                      G, G, F, F, E, E, D, G, G, F, F, E, E, D,
                      C, C, G, G, A, A, G, F, F, E, E, D, D, C};
int durationlist[] = {q, q, q, q, q, q, h, q, q, q, q, q, q, h};

int main(void) {
    *((volatile uint32_t *)0xE000ED88) |= ((3UL << 20) | (3UL << 22)); // enables the FPU for floating points

    SysTick_Init();
    PWM_Init();
    int i;
    int d = 1000;
    int n = sizeof(melody)/sizeof(melody[0]);

    // // Task 1: Play a Note
    note(40, 400);
    fixedSTEP = 0;
    Wait_ms(3000);

    // Task 2: Play Scale
    scale();
    fixedSTEP = 0;
    Wait_ms(3000);
    
    // Task 3: Play a Melody
    for(i = 0; i < n; i++) {
        note(melody[i], durationlist[i%14]);
    }
    fixedSTEP = 0;
    
}