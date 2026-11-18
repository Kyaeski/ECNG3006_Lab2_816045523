/*
 * ECNG 3006 - Lab 2, Question 2
 * Three FreeRTOS tasks and one mutex on the Arduino MKR ZERO (ATSAMD21G18A)
 *
 *   Task 1: take mutex, LED on, actively wait 0.5 s, give mutex, yield
 *   Task 2: take mutex, LED off, give mutex, task-delay 1 s
 *   Task 3: print a status message on the UART, task-delay 1 s
 */

#include <sam.h>
#include <FreeRTOS_SAMD21.h>

/* LED on PB08 (on-board LED). Change if your LED is on another pin. */
#define LED_PORT    1u                  /* 0 = PORTA, 1 = PORTB */
#define LED_PIN     8u
#define LED_MASK    (1ul << LED_PIN)

/* UART on SERCOM5: TX = PB22 (pin 14), RX = PB23 (pin 13), 115200 baud */
#define UART_BAUD_REG   63019u          /* 65536 * (1 - 16 * 115200 / 48 MHz) */

/* Each task has its own priority, set by a const int */
static const int TASK1_PRIORITY = 1;
static const int TASK2_PRIORITY = 1;
static const int TASK3_PRIORITY = 1;

/* The mutex that guards the LED pin */
static SemaphoreHandle_t xLedMutex;

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

/* ---------------- Tasks ---------------- */

/* Task 1: LED on, actively wait 0.5 s, then yield */
static void vTask1(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        xSemaphoreTake(xLedMutex, portMAX_DELAY);
        led_on();

        TickType_t start = xTaskGetTickCount();          /* active wait: */
        while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(500)) { }

        xSemaphoreGive(xLedMutex);
        taskYIELD();
    }
}

/* Task 2: LED off, then task-delay 1 s */
static void vTask2(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        xSemaphoreTake(xLedMutex, portMAX_DELAY);
        led_off();
        xSemaphoreGive(xLedMutex);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Task 3: print a status message, then task-delay 1 s */
static void vTask3(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        if (led_is_on()) {
            uart_puts("Status: LED is ON\r\n");
        } else {
            uart_puts("Status: LED is OFF\r\n");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------------- Start-up ---------------- */

void setup(void)
{
    led_init();
    uart_init();
    uart_puts("Lab 2 starting\r\n");

    xLedMutex = xSemaphoreCreateMutex();

    xTaskCreate(vTask1, "Task1", 128, NULL, TASK1_PRIORITY, NULL);
    xTaskCreate(vTask2, "Task2", 128, NULL, TASK2_PRIORITY, NULL);
    xTaskCreate(vTask3, "Task3", 128, NULL, TASK3_PRIORITY, NULL);

    vTaskStartScheduler();      /* hands control to FreeRTOS, never returns */
}

void loop(void)
{
    /* never reached */
}