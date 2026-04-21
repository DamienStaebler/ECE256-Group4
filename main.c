#include <stdint.h>
#include <math.h>

#define SYSCLK 16000000 // 16 MHz default clock

// System control registers
#define SYSCTL_RCGCGPIO_R (*((volatile uint32_t *)0x400FE608)) // GPIO clock
#define SYSCTL_RCGCPWM_R (*((volatile uint32_t *)0x400FE640)) // PWM clock

// GPIO Port B registers (base: 0x40005000)
#define GPIO_PORTB_AFSEL_R (*((volatile uint32_t *)0x40005420)) // Alt function
#define GPIO_PORTB_DEN_R (*((volatile uint32_t *)0x4000551C)) // Digital enable
#define GPIO_PORTB_PCTL_R (*((volatile uint32_t *)0x4000552C)) // Port control

// Port F pins (SW1: PF4, LED: PF1=Red, PF2=Blue, PF3=Green)
#define GPIO_PORTF_DATA_R (*((volatile uint32_t *)0x400253FC))
#define GPIO_PORTF_DIR_R   (*((volatile uint32_t *)0x40025400))
#define GPIO_PORTF_PUR_R   (*((volatile uint32_t *)0x40025510))
#define GPIO_PORTF_DEN_R   (*((volatile uint32_t *)0x4002551C))

// PWM Module 0, Generator 0 registers (base: 0x40028000)
#define PWM0_ENABLE_R (*((volatile uint32_t *)0x40028008)) // PWM output enable
#define PWM0_0_CTL_R (*((volatile uint32_t *)0x40028040)) // Generator control
#define PWM0_0_LOAD_R (*((volatile uint32_t *)0x40028050)) // Load (period)
#define PWM0_0_CMPA_R (*((volatile uint32_t *)0x40028058)) // Compare A (duty)
#define PWM0_0_GENA_R (*((volatile uint32_t *)0x40028060)) // Generator A action

// SysTick registers (Cortex-M core)
#define NVIC_ST_CTRL_R (*((volatile uint32_t *)0xE000E010)) // Control/Status
#define NVIC_ST_RELOAD_R (*((volatile uint32_t *)0xE000E014)) // Reload value
#define NVIC_ST_CURRENT_R (*((volatile uint32_t *)0xE000E018)) // Current value
#define SYSCTL_RCC_R (*((volatile uint32_t *)0x400FE060))

#define SYSCTL_PRGPIO_R (*((volatile uint32_t *)0x400FEA08))
#define SYSCTL_PRPWM_R  (*((volatile uint32_t *)0x400FEA40))

#define TABLE_SIZE 32
#define MAX 15

#define q 400 // Quarter
#define h 800 // Half

// Timers
void SysTick_Init(void);
void SysTick_Handler(void);
void Wait_ms(uint32_t ms);

// Init
void PWM_Init(void);
void PortF_Init(void);

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
    // Phrase 1: Twin-kle, twin-kle, lit-tle star
    {C, q}, {C, q}, {G, q}, {G, q}, {A, q}, {A, q}, {G, h}, 
    
    // Phrase 2: How I won-der what you are
    {F, q}, {F, q}, {E, q}, {E, q}, {D, q}, {D, q}, {C, h}, 
    
    // Phrase 3: Up a-bove the world so high (The Bridge Part 1)
    {G, q}, {G, q}, {F, q}, {F, q}, {E, q}, {E, q}, {D, h}, 
    
    // Phrase 4: Like a dia-mond in the sky (The Bridge Part 2)
    {G, q}, {G, q}, {F, q}, {F, q}, {E, q}, {E, q}, {D, h}, 
    
    // Phrase 5: Twin-kle, twin-kle, lit-tle star (Reprise)
    {C, q}, {C, q}, {G, q}, {G, q}, {A, q}, {A, q}, {G, h}, 
    
    // Phrase 6: How I won-der what you are (Reprise)
    {F, q}, {F, q}, {E, q}, {E, q}, {D, q}, {D, q}, {C, h}  
};

typedef enum { IDLE, PLAYING, PAUSED } State_t;
volatile State_t currentState = IDLE;

int main(void) {
    *((volatile uint32_t *)0xE000ED88) |= ((3UL << 20) | (3UL << 22)); 
    
    SysTick_Init();
    PWM_Init();
    PortF_Init();
    
    int melody_idx = 0;
    int total_notes = sizeof(twinklestar) / sizeof(Note_t);
    int phrase;

    // FSM
    while (1) {
        int button_pressed = ((GPIO_PORTF_DATA_R & 0x10) == 0);

        switch (currentState) {
            case IDLE:
                Set_LED(0x02); // Red
                if (button_pressed) {
                    Wait_ms(200); 
                    currentState = PLAYING;
                }
                break;

            case PLAYING:
                phrase = melody_idx / 7; // every 7 phrase
                // Phrase colors: Green -> Blue -> Cyan
                uint8_t colors[] = {0x08, 0x04, 0x02, 0x0C, 0x0A, 0x0E}; 
                Set_LED(colors[phrase % 6]);

                if (melody_idx < total_notes) {
                    note(twinklestar[melody_idx].pitch, twinklestar[melody_idx].duration);
                    
                    // Only move to the next note if we didn't just pause mid-note
                    if (currentState == PLAYING) {
                        melody_idx++;
                    }
                } else {
                    melody_idx = 0;
                }
                break;

            case PAUSED:
                Set_LED(0x0E); // White when paused
                if (button_pressed) {
                    Wait_ms(200);
                    currentState = PLAYING;
                }
                break;
        }
    }
}


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
    while ((ms_ticks - start) < ms) {
        if ((GPIO_PORTF_DATA_R & 0x10) == 0) {
            uint32_t debounce_start = ms_ticks;
            while((ms_ticks - debounce_start) < 200); 
            
            if (currentState == PLAYING) {
                currentState = PAUSED;
                fixedSTEP = 0; // stop sound
            } else {
                currentState = PLAYING;
            }
            
            // Wait for release
            while((GPIO_PORTF_DATA_R & 0x10) == 0);
        }
        
        // If the button changed the state to PAUSED, we exit the delay early
        if (currentState != PLAYING) return;
    }
}

void PWM_Init(void) {
    SYSCTL_RCGCPWM_R |= 0x01;   // Enable PWM Module 0 clock
    SYSCTL_RCGCGPIO_R |= 0x02;  // Enable Port B clock
    
    // CRITICAL: Wait for peripherals to be ready
    while((SYSCTL_PRPWM_R & 0x01) == 0);
    while((SYSCTL_PRGPIO_R & 0x02) == 0);

    SYSCTL_RCC_R |= 0x00100000;   // Use PWM divider
    SYSCTL_RCC_R &= ~0x000E0000;  // PWM clock = SYSCLK / 2 (8 MHz)
    
    GPIO_PORTB_AFSEL_R |= 0x40;   // PB6 alt function
    GPIO_PORTB_PCTL_R = (GPIO_PORTB_PCTL_R & 0xF0FFFFFF) | 0x04000000; 
    GPIO_PORTB_DEN_R |= 0x40; 
    
    PWM0_0_CTL_R = 0;             // Setup mode
    PWM0_0_GENA_R = 0x8C;         
    PWM0_0_LOAD_R = 400 - 1;      // ~20kHz carrier frequency
    PWM0_0_CMPA_R = 200;          // Start at 50% duty
    PWM0_0_CTL_R = 1;             // Enable generator
    PWM0_ENABLE_R |= 0x01;        // Enable PWM0
}

void PortF_Init(void) {
    SYSCTL_RCGCGPIO_R |= 0x20; 
    while((SYSCTL_PRGPIO_R & 0x20) == 0); // Wait for Port F
    
    GPIO_PORTF_DIR_R |= 0x0E;  
    GPIO_PORTF_DIR_R &= ~0x10; 
    GPIO_PORTF_PUR_R |= 0x10;  
    GPIO_PORTF_DEN_R |= 0x1E;  
}

void Set_LED(uint8_t color) {
    GPIO_PORTF_DATA_R = (GPIO_PORTF_DATA_R & ~0x0E) | (color & 0x0E);
}

void note(int note_val, int duration) {
    if (note_val == 0) {
        fixedSTEP = 0;
    } else {
        // Calculate new frequency
        float freq = 440.0 * pow(2.0, (note_val - 49.0) / 12.0);
        float floatStep = (freq * (float)TABLE_SIZE) / 8000.0;
        fixedSTEP = (uint32_t)(floatStep * 65536.0);
    }

    Wait_ms(duration);
    
    fixedSTEP = 0;
    Wait_ms(50); 
}
