/*
 * ECNG 3006 - Lab 2, Question 5
 * Three FreeRTOS tasks on the Arduino MKR ZERO (ATSAMD21G18A).
 * The mutex from Questions 2-4 is replaced by a BINARY SEMAPHORE.
 * The RunTime Trace from Question 4 is kept, so the two can be compared.
 *
 *   Task 1: take semaphore, LED on, actively wait 0.5 s, give, yield
 *   Task 2: take semaphore, LED off, give, task-delay 1 s
 *   Task 3: print a status message on the UART, task-delay 1 s
 *   Report: every 5 s, print what the trace macros recorded
 *
 * Needs: include/trace_hooks.h, src/trace.c, and
 *        build_flags = -I include -include trace_hooks.h  in platformio.ini
 */

#include <sam.h>
#include <FreeRTOS_SAMD21.h>
#include "trace_hooks.h"        /* TraceStats_t and the trace_ functions */

/* ---------------- Lock selection ----------------
 * 1 = binary semaphore (the Question 5 answer)
 * 0 = mutex (as in Questions 2-4), only to compare against
 */
#define USE_SEMAPHORE   1

/* ---------------- Test case selection ---------------- */

#define TEST_CASE   1   /* 1 = all the same priority
                           2 = distinct: Task 1 highest, then Task 2, then Task 3
                           3 = distinct: Task 3 highest, then Task 2, then Task 1 */

#if TEST_CASE == 1
static const int TASK1_PRIORITY = 1;
static const int TASK2_PRIORITY = 1;
static const int TASK3_PRIORITY = 1;
#elif TEST_CASE == 2
static const int TASK1_PRIORITY = 3;
static const int TASK2_PRIORITY = 2;
static const int TASK3_PRIORITY = 1;
#elif TEST_CASE == 3
static const int TASK1_PRIORITY = 1;
static const int TASK2_PRIORITY = 2;
static const int TASK3_PRIORITY = 3;
#else
#error "TEST_CASE must be 1, 2 or 3"
#endif

/* The report task sits above all three, so it can always print */
#if configMAX_PRIORITIES < 5
#error "Set configMAX_PRIORITIES to 5 or more in FreeRTOSConfig.h"
#endif
static const int REPORT_PRIORITY = configMAX_PRIORITIES - 1;

#define REPORT_PERIOD_MS    5000

/* ---------------- Hardware settings ---------------- */

/* LED on PB08 (on-board LED). Change if your LED is on another pin. */
#define LED_PORT    1u                  /* 0 = PORTA, 1 = PORTB */
#define LED_PIN     8u
#define LED_MASK    (1ul << LED_PIN)

/* UART on SERCOM5: TX = PB22 (pin 14), RX = PB23 (pin 13), 115200 baud */
#define UART_BAUD_REG   63019u          /* 65536 * (1 - 16 * 115200 / 48 MHz) */

/* ---------------- Shared variables ---------------- */

static SemaphoreHandle_t xLedLock;      /* guards the LED pin (semaphore or mutex) */

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

static void uart_putc(char c)
{
    while (!SERCOM5->USART.INTFLAG.bit.DRE) { }     /* wait until ready */
    SERCOM5->USART.DATA.reg = (uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}

/* Print an unsigned number as text, e.g. 42 -> "42" */
static void uart_putu(uint32_t value)
{
    char buf[11];
    int i = 0;

    if (value == 0) {
        uart_putc('0');
        return;
    }
    while (value > 0) {                 /* peel digits off, last one first */
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {                     /* send them in the right order */
        uart_putc(buf[--i]);
    }
}

/* ---------------- Tasks ---------------- */

/* Task 1: LED on, actively wait 0.5 s, then yield */
static void vTask1(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        xSemaphoreTake(xLedLock, portMAX_DELAY);
        led_on();

        TickType_t start = xTaskGetTickCount();          /* active wait: */
        while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(500)) { }

        xSemaphoreGive(xLedLock);
        taskYIELD();
    }
}

/* Task 2: LED off, then task-delay 1 s */
static void vTask2(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        xSemaphoreTake(xLedLock, portMAX_DELAY);
        led_off();
        xSemaphoreGive(xLedLock);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Task 3: print a status message, then task-delay 1 s */
static void vTask3(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        /* Pause task switching while printing, so the report task can't
         * cut into the middle of this line */
        vTaskSuspendAll();
        uart_puts("t=");
        uart_putu(xTaskGetTickCount() * portTICK_PERIOD_MS);
        uart_puts(" ms | LED is ");
        uart_puts(led_is_on() ? "ON\r\n" : "OFF\r\n");
        xTaskResumeAll();

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* One line of the report for one task */
static void print_task_stats(const char *name, const TraceStats_t *s)
{
    uart_puts(name);
    uart_puts(": runs=");
    uart_putu(s->runs);
    uart_puts(" | lock blocks=");
    uart_putu(s->mutexBlocks);
    uart_puts(" (longest ");
    uart_putu(s->mutexWaitMax * portTICK_PERIOD_MS);
    uart_puts(" ms) | delay blocks=");
    uart_putu(s->delayBlocks);
    uart_puts(" | longest wait for CPU=");
    uart_putu(s->cpuWaitMax * portTICK_PERIOD_MS);
    uart_puts(" ms");
    if (s->runs == 0) {
        uart_puts("  <-- NEVER RAN");
    }
    uart_puts("\r\n");
}

/* Report task: every 5 s, print what the trace macros have recorded */
static void vReportTask(void *pvParameters)
{
    (void)pvParameters;
    TraceStats_t stats[TRACE_SLOTS];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(REPORT_PERIOD_MS));

        trace_get_stats(stats);         /* snapshot of the counters */

        vTaskSuspendAll();
        uart_puts("\r\n--- Trace report at t=");
        uart_putu(xTaskGetTickCount() * portTICK_PERIOD_MS);
        uart_puts(" ms (totals since start) ---\r\n");
        print_task_stats("Task1 ", &stats[1]);
        print_task_stats("Task2 ", &stats[2]);
        print_task_stats("Task3 ", &stats[3]);
        print_task_stats("Report", &stats[4]);
        uart_puts("Idle/other tasks: switched in ");
        uart_putu(stats[0].runs);
        uart_puts(" times\r\n\r\n");
        xTaskResumeAll();
    }
}

/* ---------------- Start-up ---------------- */

static void print_test_info(void)
{
    uart_puts("\r\n=== Lab 2 Q5 - ");
    uart_puts(USE_SEMAPHORE ? "BINARY SEMAPHORE" : "MUTEX");
    uart_puts(" - Test case ");
    uart_putu(TEST_CASE);
    uart_puts(TEST_CASE == 1 ? ": same priority ===\r\n"
                             : ": distinct priorities ===\r\n");
    uart_puts("Priorities T1/T2/T3 = ");
    uart_putu(TASK1_PRIORITY); uart_puts("/");
    uart_putu(TASK2_PRIORITY); uart_puts("/");
    uart_putu(TASK3_PRIORITY);
    uart_puts(" | Report task = ");
    uart_putu(REPORT_PRIORITY);
    uart_puts(" | Preemption = ");
    uart_puts(configUSE_PREEMPTION ? "ON\r\n\r\n" : "OFF\r\n\r\n");
}

static void error_halt(const char *msg)
{
    uart_puts("ERROR: ");
    uart_puts(msg);
    uart_puts("\r\n");
    for (;;) { }
}

void setup(void)
{
    TaskHandle_t h1, h2, h3, hReport;

    led_init();
    uart_init();
    print_test_info();

#if USE_SEMAPHORE
    /* A binary semaphore starts EMPTY (taken), so give it once to make
     * the LED available. No owner, and no priority inheritance. */
    xLedLock = xSemaphoreCreateBinary();
    if (xLedLock != NULL) {
        xSemaphoreGive(xLedLock);
    }
#else
    /* A mutex starts available, has an owner, and uses priority inheritance */
    xLedLock = xSemaphoreCreateMutex();
#endif
    if (xLedLock == NULL) {
        error_halt("could not create the semaphore/mutex");
    }

    if (xTaskCreate(vTask1, "Task1", 128, NULL, TASK1_PRIORITY, &h1) != pdPASS ||
        xTaskCreate(vTask2, "Task2", 128, NULL, TASK2_PRIORITY, &h2) != pdPASS ||
        xTaskCreate(vTask3, "Task3", 192, NULL, TASK3_PRIORITY, &h3) != pdPASS ||
        xTaskCreate(vReportTask, "Report", 256, NULL, REPORT_PRIORITY, &hReport) != pdPASS) {
        error_halt("could not create the tasks (out of heap?)");
    }

    /* Tell the trace code which task is which, and which lock to watch */
    trace_register_task(1, h1);
    trace_register_task(2, h2);
    trace_register_task(3, h3);
    trace_register_task(4, hReport);
    trace_register_mutex(xLedLock);

    vTaskStartScheduler();      /* hands control to FreeRTOS, never returns */

    error_halt("scheduler returned");
}

void loop(void)
{
    /* never reached */
}