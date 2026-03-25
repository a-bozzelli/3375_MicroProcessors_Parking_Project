/*
 * Smart Parking Garage System
 * DE10-Standard (ARM Cortex-A9 HPS)
 *
 * I/O Mapping:
 *   KEY0        → Park a car (increment occupied)
 *   KEY1        → Free a spot (decrement occupied)
 *   SW[7:0]     → Total capacity (0–255)
 *   SW[9]       → System on/off
 *   LEDR[9:0]   → First 10 spot indicators (on = occupied)
 *   HEX0–HEX2   → Available spots (3-digit decimal)
 *   HEX3         → "F" when garage is full
 *   HEX4–HEX5   → Unused (blanked)
 */


/* Push buttons */
volatile int *KEY_BASE = (volatile int *)0xFF200050;  /* Data register    */
volatile int *KEY_EDGE = (volatile int *)0xFF20005C;  /* Edge capture reg */

/* Slider switches */
volatile int *SW_BASE = (volatile int *)0xFF200040;

/* Red LEDs */
volatile int *LEDR_BASE = (volatile int *)0xFF200000;

/* 7-Segment displays */
volatile int *HEX_BASE_03 = (volatile int *)0xFF200020;  /* HEX3–HEX0 */
volatile int *HEX_BASE_45 = (volatile int *)0xFF200030;  /* HEX5–HEX4 */

/* ARM A9 Private Timer (for debounce timing) */
volatile int *TIMER_LOAD    = (volatile int *)0xFFFEC600;
volatile int *TIMER_VALUE   = (volatile int *)0xFFFEC604;
volatile int *TIMER_CONTROL = (volatile int *)0xFFFEC608;
volatile int *TIMER_STATUS  = (volatile int *)0xFFFEC60C;


volatile int occupied_count = 0;    /* Current number of parked cars       */
int total_capacity          = 0;    /* Read from SW[7:0], range 0–255      */
int available_count         = 0;    /* Computed: total_capacity - occupied  */
int system_enabled          = 0;    /* Read from SW[9]                     */
int is_full                 = 0;    /* True when available_count == 0      */
volatile int key0_pressed   = 0;    /* Edge-detected flag for park button  */
volatile int key1_pressed   = 0;    /* Edge-detected flag for leave button */

/*
 * Segment encoding (active-low on DE10-Standard):
 *
 *     --0--
 *    |     |
 *    5     1
 *    |     |
 *     --6--
 *    |     |
 *    4     2
 *    |     |
 *     --3--
 *
 * Each entry: bits [6:0] map to segments 6..0
 * 0 = segment ON, 1 = segment OFF (active low)
 */
const unsigned char seven_seg_table[10] = {
    0x3F, // 0 
    0x06, // 1 
    0x5B, // 2 
    0x4F, // 3 
    0x66, // 4 
    0x6D, // 5 
    0x7D, // 6 
    0x07, // 7 
    0x7F, // 8 
    0x6F  // 9 
};

/* "F" character for FULL indicator: segments 0,4,5,6 */
const unsigned char CHAR_F = 0x71;

/* Blank display (all segments off) */
const unsigned char BLANK  = 0x00;


/*
 * Clears the edge capture register so no stale button presses are detected.
 */
void init_buttons(void) {
    *(KEY_EDGE) = 0xF;  /* Write 1s to clear all edge capture bits */
}

/*
 * Turns off all 10 red LEDs.
 */
void init_leds(void) {
    *(LEDR_BASE) = 0x000;
}

/*
 * Blanks all six 7-segment displays.
 */
void init_seven_seg(void) {
    *(HEX_BASE_03) = 0x00000000;
    *(HEX_BASE_45) = 0x00000000;
}

/*
 * ARM A9 private timer for debounce delays.
 * Timer clock = 200 MHz
 * We load a value for ~50 ms: 200,000,000 / 20 = 10,000,000 ticks.
 * Timer is configured but NOT started — used on-demand for delays.
 */
void init_timer(void) {
    *(TIMER_CONTROL) = 0x0;            /* Stop timer            */
    *(TIMER_LOAD)    = 10000000;       /* ~50 ms at 200 MHz     */
    *(TIMER_STATUS)  = 0x1;            /* Clear interrupt flag   */
}

/*
 * Master initialization — calls all init functions, resets state.
 */
void system_init(void) {
    init_buttons();
    init_leds();
    init_seven_seg();
    init_timer();

    occupied_count = 0;
    total_capacity = 0;
    available_count = 0;
    system_enabled = 0;
    is_full = 0;
    key0_pressed = 0;
    key1_pressed = 0;
}

/*
 * read_capacity()
 * Reads SW[7:0] for total capacity (0–255).
 * Reads SW[9] for system enable.
 */
void read_switches(void) {
    int sw_val = *(SW_BASE);
    total_capacity = sw_val & 0xFF;          /* SW[7:0] = capacity     */
    system_enabled = (sw_val >> 9) & 0x1;    /* SW[9]   = on/off       */
}

/*
 * sets private timer to wait ~50 ms for debouncing.
 * Blocking delay — starts timer, waits for it to expire.
 */
void delay_debounce(void) {
    *(TIMER_CONTROL) = 0x0;        /* Stop timer                     */
    *(TIMER_STATUS)  = 0x1;        /* Clear any previous timeout     */
    *(TIMER_LOAD)    = 10000000;   /* Reload: ~50 ms at 200 MHz      */
    *(TIMER_CONTROL) = 0x1;        /* Start timer, no auto-reload    */

    while ((*(TIMER_STATUS) & 0x1) == 0) {
        /* Wait for timer to reach 0 */
    }

    *(TIMER_STATUS)  = 0x1;        /* Clear the timeout flag         */
    *(TIMER_CONTROL) = 0x0;        /* Stop timer                     */
}

/*
 * Reads the edge capture register for KEY0 and KEY1.
 * Sets key0_pressed / key1_pressed flags.
 * Clears the edge capture bits after reading.
 * Applies debounce delay if a press is detected.
 */
void poll_buttons(void) {
    int edge = *(KEY_EDGE);

    key0_pressed = 0;
    key1_pressed = 0;

    if (edge & 0x1) {
        /* KEY0 was pressed (park) */
        key0_pressed = 1;
        *(KEY_EDGE) = 0x1;    /* Clear KEY0 edge capture bit */
        delay_debounce();
        *(KEY_EDGE) = 0xF;    /* Clear any edges during debounce */
    }

    if (edge & 0x2) {
        /* KEY1 was pressed (leave) */
        key1_pressed = 1;
        *(KEY_EDGE) = 0x2;    /* Clear KEY1 edge capture bit */
        delay_debounce();
        *(KEY_EDGE) = 0xF;    /* Clear any edges during debounce */
    }
}

/*
 * Maps the occupied count to the 10 red LEDs.
 * - If occupied >= 10: all 10 LEDs on (0x3FF)
 * - If occupied < 10:  lowest N LEDs on
 *
 */
void update_leds(int occupied) {
    int led_val;

    if (occupied >= 10) {
        led_val = 0x3FF;   /* All 10 LEDs on */
    } else if (occupied <= 0) {
        led_val = 0x000;   /* All LEDs off */
    } else {
        led_val = (1 << occupied) - 1;  /* Set lowest N bits */
    }

    *(LEDR_BASE) = led_val;
}

/*
 * Displays the available count on HEX0 (ones), HEX1 (tens), HEX2 (hundreds).
 * Displays "F" on HEX3 if garage is full, otherwise blank.
 * Blanks HEX4 and HEX5 (unused).
 */
void update_seven_seg(int available, int full) {
    int ones, tens, hundreds;
    int hex03_val;

    /* Clamp available to displayable range */
    if (available < 0)   available = 0;
    if (available > 255) available = 255;

    /* Extract decimal digits */
    ones     = available % 10;
    tens     = (available / 10) % 10;
    hundreds = (available / 100) % 10;

    /*
     * HEX_BASE_03 register layout (32 bits):
     *   [31:24] = HEX3
     *   [23:16] = HEX2
     *   [15:8]  = HEX1
     *   [7:0]   = HEX0
     */
    hex03_val = (seven_seg_table[ones])            /* HEX0: ones     */
              | (seven_seg_table[tens]     << 8)   /* HEX1: tens     */
              | (seven_seg_table[hundreds] << 16); /* HEX2: hundreds */

    if (full) {
        hex03_val |= (CHAR_F << 24);              /* HEX3: "F"      */
    } else {
        hex03_val |= (BLANK << 24);               /* HEX3: blank    */
    }

    *(HEX_BASE_03) = hex03_val;

    /* Blank HEX4 and HEX5 */
    *(HEX_BASE_45) = 0x00000000;
}

/*
 * Attempts to park one car.
 * Increments occupied_count if garage is not full.
 */
void park_car(void) {
    if (occupied_count < total_capacity) {
        occupied_count++;
    }
    /* If full, do nothing — press is ignored */
}

/*
 * Attempts to free one spot.
 * Decrements occupied_count if garage is not empty.
 */
void free_spot(void) {
    if (occupied_count > 0) {
        occupied_count--;
    }
    /* If empty, do nothing — press is ignored */
}

/*
 * Turns off all LEDs and blanks all 7-segment displays.
 * Called when system is disabled.
 */
void clear_all_outputs(void) {
    *(LEDR_BASE)    = 0x000;
    *(HEX_BASE_03)  = 0x00000000;
    *(HEX_BASE_45)  = 0x00000000;
}

int main(void) {

    /* Initialize everything */
    system_init();

    while (1) {

        /* Step 1: Read switches (capacity + on/off) */
        read_switches();

        /* Step 2: Check if system is enabled */
        if (!system_enabled) {
            // 
            // SYSTEM OFF state:
            // - Clear all outputs
            // - Reset occupied count
            // - Clear any pending button edges
            // - Loop back and wait for SW[9] to turn on
            // 
            occupied_count = 0;
            clear_all_outputs();
            *(KEY_EDGE) = 0xF;   /* Clear stale edges */
            continue;
        }

        /* Step 3: Clamp occupied_count if capacity was reduced */
        if (occupied_count > total_capacity) {
            occupied_count = total_capacity;
        }

        /* Step 4: Poll buttons for park/leave events */
        poll_buttons();

        /* Step 5: Process park event (KEY0) */
        if (key0_pressed) {
            park_car();
        }

        /* Step 6: Process leave event (KEY1) */
        if (key1_pressed) {
            free_spot();
        }

        /* Step 7: Compute available count and full status */
        available_count = total_capacity - occupied_count;
        is_full = (available_count == 0) ? 1 : 0;

        /* Step 8: Update all outputs */
        update_leds(occupied_count);
        update_seven_seg(available_count, is_full);
    }

    return 0;  /* Never reached */
}