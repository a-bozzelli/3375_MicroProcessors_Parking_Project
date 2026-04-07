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
int is_full                 = 0;    /* True when available_count == 0      */
volatile int key0_pressed   = 0;    /* Edge-detected flag for park button  */
volatile int key1_pressed   = 0;    /* Edge-detected flag for leave button */

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

// We load a value for ~50 ms: 200,000,000 / 20 = 10,000,000 ticks.
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


        /* Step 2: Clamp occupied_count if capacity was reduced */
        if (occupied_count > total_capacity) {
            occupied_count = total_capacity;
        }

        /* Step 3: Poll buttons for park/leave events */
        poll_buttons();

        /* Step 4: Process park event (KEY0) */
        if (key0_pressed) {
            park_car();
        }

        /* Step 5: Process leave event (KEY1) */
        if (key1_pressed) {
            free_spot();
        }

        /* Step 6: Compute available count and full status */
        available_count = total_capacity - occupied_count;
        is_full = (available_count == 0) ? 1 : 0;

        /* Step 7: Update all outputs */
        update_leds(occupied_count);
        update_seven_seg(available_count, is_full);
    }

    return 0;  /* Never reached */
}