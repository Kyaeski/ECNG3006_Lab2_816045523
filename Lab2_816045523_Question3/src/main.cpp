/*
 * ECNG 3006 - Lab 2, Question 3
 * Three FreeRTOS tasks and one mutex on the Arduino MKR ZERO (ATSAMD21G18A)
 *
 *   Task 1: take mutex, LED on, actively wait 0.5 s, give mutex, yield
 *   Task 2: take mutex, LED off, give mutex, task-delay 1 s
 *   Task 3: print a status message on the UART, task-delay 1 s
 *
 * Two test cases (pick one with TEST_CASE below):
 *   1 = all three tasks the SAME priority, run to completion
 *       -> needs configUSE_PREEMPTION 0 in FreeRTOSConfig.h
 *   2 = case (c): Task 1 highest, then Task 2, then Task 3,
 *       priority inheritance enabled (a FreeRTOS mutex always uses it)
 *       -> needs configUSE_PREEMPTION 1 in FreeRTOSConfig.h
 */

#include <sam.h>
#include <FreeRTOS_SAMD21.h>

/* ---------------- Test case selection ---------------- */

#define TEST_CASE   1       /* 1 = same priority (run to completion), 2 = case (c) */

#if TEST_CASE == 1
static const int TASK1_PRIORITY = 1;
static const int TASK2_PRIORITY = 1;
static const int TASK3_PRIORITY = 1;
#elif TEST_CASE == 2
static const int TASK1_PRIORITY = 3;    /* highest */
static const int TASK2_PRIORITY = 2;
static const int TASK3_PRIORITY = 1;    /* lowest */
#else
#error "TEST_CASE must be 1 or 2"
#endif

/* Stop the build if FreeRTOSConfig.h doesn't match the chosen test case */
#if (TEST_CASE == 1) && (configUSE_PREEMPTION != 0)
#error "Test case 1 (run to completion) needs configUSE_PREEMPTION 0 in FreeRTOSConfig.h"
#endif
#if (TEST_CASE == 2) && (configUSE_PREEMPTION != 1)
#error "Test case 2 (case c) needs configUSE_PREEMPTION 1 in FreeRTOSConfig.h"
#endif

/* ---------------- Hardware settings ---------------- */

/* LED on PB08 (on-board LED). Change if your LED is on another pin. */
#define LED_PORT    1u                  /* 0 = PORTA, 1 = PORTB */
#define LED_PIN     8u
#define LED_MASK    (1ul << LED_PIN)

/* UART on SERCOM5: TX = PB22 (pin 14), RX = PB23 (pin 13), 115200 baud */
#define UART_BAUD_REG   63019u          /* 65536 * (1 - 16 * 115200 / 48 MHz) */

/* ---------------- Shared variables ---------------- */

static SemaphoreHandle_t xLedMutex;     /* guards the LED pin */

/* How many times Task 1 and Task 2 have used the LED (evidence for Q3) */
static volatile uint32_t ulTask1Runs = 0;
static volatile uint32_t ulTask2Runs = 0;

/* ---------------- LED ---------------- */

static void led_init(void)
{
    PORT->Group[LED_PORT].DIRSET.reg = LED_MASK;    /* make pin an output */
    PORT->Group[LED_PORT].OUTCLR.reg = LED_MASK;    /* start with LED off */
}

static void led_on(void)  { PORT->Group[LED_PORT].OUTSET.reg = LED_MASK; }
static void led_off(void) { PORT->Group[LED_PORT].OUTCLR.reg = LED_MASK; }

static int led_is_on(void)
{
    return (PORT->Group[LED_PORT].OUT.reg & LED_MASK) != 0;
}

/* ---------------- UART ---------------- */

static void uart_init(void)
{
    /* Power SERCOM5 and give it the 48 MHz clock (GCLK0) */
    PM->APBCMASK.reg |= PM_APBCMASK_SERCOM5;
    GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID(SERCOM5_GCLK_ID_CORE) |
                        GCLK_CLKCTRL_GEN_GCLK0 | GCLK_CLKCTRL_CLKEN;
    while (GCLK->STATUS.bit.SYNCBUSY) { }

    /* Hand PB22 and PB23 to SERCOM5 (peripheral function D) */
    PORT->Group[1].PINCFG[22].reg = PORT_PINCFG_PMUXEN;
    PORT->Group[1].PINCFG[23].reg = PORT_PINCFG_PMUXEN | PORT_PINCFG_INEN;
    PORT->Group[1].PMUX[11].reg   = PORT_PMUX_PMUXE_D | PORT_PMUX_PMUXO_D;

    /* Reset, then configure: UART, LSB first, RX on pad 3, TX on pad 2 */
    SERCOM5->USART.CTRLA.reg = SERCOM_USART_CTRLA_SWRST;
    while (SERCOM5->USART.SYNCBUSY.bit.SWRST) { }

    SERCOM5->USART.CTRLA.reg = SERCOM_USART_CTRLA_MODE(1) |
                               SERCOM_USART_CTRLA_DORD |
                               SERCOM_USART_CTRLA_RXPO(3) |
                               SERCOM_USART_CTRLA_TXPO(1);
    SERCOM5->USART.BAUD.reg  = UART_BAUD_REG;

    /* 8 data bits, enable transmitter and receiver */
    SERCOM5->USART.CTRLB.reg = SERCOM_USART_CTRLB_CHSIZE(0) |
                               SERCOM_USART_CTRLB_TXEN |
                               SERCOM_USART_CTRLB_RXEN;
    while (SERCOM5->USART.SYNCBUSY.bit.CTRLB) { }

    /* Turn it on */
    SERCOM5->USART.CTRLA.reg |= SERCOM_USART_CTRLA_ENABLE;
    while (SERCOM5->USART.SYNCBUSY.bit.ENABLE) { }
}

static void uart_puts(const char *s)
{
    while (*s) {
        while (!SERCOM5->USART.INTFLAG.bit.DRE) { }  /* wait until ready */
        SERCOM5->USART.DATA.reg = *s++;              /* send one character */
    }
}

/* Print an unsigned number as text, e.g. 42 -> "42" */
static void uart_putu(uint32_t value)
{
    char buf[11];
    int i = 0;

    if (value == 0) {
        uart_puts("0");
        return;
    }
    while (value > 0) {                  /* peel digits off, last one first */
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {                      /* send them in the right order */
        char c[2] = { buf[--i], '\0' };
        uart_puts(c);
    }
}

/* ---------------- Tasks ---------------- */
static void vTask1(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        xSemaphoreTake(xLedMutex, portMAX_DELAY);
        led_on();
        ulTask1Runs++;

        TickType_t start = xTaskGetTickCount();          /* active wait: */
        while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(500)) { }

        xSemaphoreGive(xLedMutex);
        taskYIELD();
    }
}

static void vTask2(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        xSemaphoreTake(xLedMutex, portMAX_DELAY);
        led_off();
        ulTask2Runs++;
        xSemaphoreGive(xLedMutex);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void vTask3(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        uart_puts("t=");
        uart_putu(xTaskGetTickCount() * portTICK_PERIOD_MS);
        uart_puts(" ms | LED is ");
        uart_puts(led_is_on() ? "ON " : "OFF");
        uart_puts(" | Task1 runs=");
        uart_putu(ulTask1Runs);
        uart_puts(" | Task2 runs=");
        uart_putu(ulTask2Runs);
        uart_puts("\r\n");

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void print_test_info(void)
{
    uart_puts("\r\n=== Lab 2 Q3 - Test case ");
    uart_putu(TEST_CASE);
    uart_puts(TEST_CASE == 1 ? ": same priority, run to completion ===\r\n"
                             : ": case (c), priority inheritance ===\r\n");
    uart_puts("Priorities T1/T2/T3 = ");
    uart_putu(TASK1_PRIORITY); uart_puts("/");
    uart_putu(TASK2_PRIORITY); uart_puts("/");
    uart_putu(TASK3_PRIORITY);
    uart_puts(" | Preemption = ");
    uart_puts(configUSE_PREEMPTION ? "ON" : "OFF");
    uart_puts(" | Mutex (priority inheritance) = ON\r\n\r\n");
}

void setup(void)
{
    led_init();
    uart_init();
    print_test_info();

    xLedMutex = xSemaphoreCreateMutex();  

    xTaskCreate(vTask1, "Task1", 128, NULL, TASK1_PRIORITY, NULL);
    xTaskCreate(vTask2, "Task2", 128, NULL, TASK2_PRIORITY, NULL);
    xTaskCreate(vTask3, "Task3", 192, NULL, TASK3_PRIORITY, NULL);

    vTaskStartScheduler();     
}

void loop(void)
{
}