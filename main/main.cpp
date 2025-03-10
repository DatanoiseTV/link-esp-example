#include <ableton/Link.hpp>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <driver/timer.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <protocol_examples_common.h>

#include "esp_wifi.h"

#define LED GPIO_NUM_2
#define PRINT_LINK_STATE false
#define USB_MIDI true

#define UART_PORT UART_NUM_1
#define TX_PIN 15
#define RX_PIN 12

#define BUF_SIZE 40
#define FRAME_DUR_US 250 // 40 * 250 us = 10 ms total buffer duration
#define FRAME_DUR (FRAME_DUR_US / 1000000.f) // frame duration in seconds

QueueHandle_t gBuf;

void IRAM_ATTR timer_group0_isr(void *userParam)
{
  timer_group_clr_intr_status_in_isr(TIMER_GROUP_0, TIMER_0);
  timer_group_enable_alarm_in_isr(TIMER_GROUP_0, TIMER_0);

  uint8_t clk;
  if (xQueueReceiveFromISR(gBuf, &clk, 0) && clk) {
#ifdef USB_MIDI
      uint8_t data[4] = {0x0f, 0xf8, 0x00, 0x00};
      uart_write_bytes(UART_PORT, (char*) data, 4);
#else
      uint8_t data[1] = {0xf8};
      uart_write_bytes(UART_PORT, (char*) data, 1);
#endif
  }
}

void timerGroup0Init(int timerPeriodUS, void* userParam)
{
  timer_config_t config = {.alarm_en = TIMER_ALARM_EN,
    .counter_en = TIMER_PAUSE,
    .intr_type = TIMER_INTR_LEVEL,
    .counter_dir = TIMER_COUNT_UP,
    .auto_reload = TIMER_AUTORELOAD_EN,
    .divider = 80};

  timer_init(TIMER_GROUP_0, TIMER_0, &config);
  timer_set_counter_value(TIMER_GROUP_0, TIMER_0, 0);
  timer_set_alarm_value(TIMER_GROUP_0, TIMER_0, timerPeriodUS);
  timer_enable_intr(TIMER_GROUP_0, TIMER_0);
  // Allocate interrupt with high priority ESP_INTR_FLAG_LEVEL3
  timer_isr_register(TIMER_GROUP_0, TIMER_0, &timer_group0_isr, userParam,
    ESP_INTR_FLAG_LEVEL3, nullptr);

  timer_start(TIMER_GROUP_0, TIMER_0);
}

void printTask(void *userParam)
{
  while (true) {
    auto link = static_cast<ableton::Link *>(userParam);
    const auto quantum = 4.0;
    const auto sessionState = link->captureAppSessionState();
    const auto numPeers = link->numPeers();
    const auto time = link->clock().micros();
    const auto beats = sessionState.beatAtTime(time, quantum);
    std::cout << std::defaultfloat << "| peers: " << numPeers << " | "
              << "tempo: " << sessionState.tempo() << " | " << std::fixed
              << "beats: " << beats << " |" << std::endl;
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

void initUartPort(uart_port_t port, int txPin, int rxPin)
{
  uart_config_t uart_config = {
      .baud_rate = 31250,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 122,
  };

  if (USB_MIDI) {
    uart_config.baud_rate = 115200;
  }

  uart_param_config(port, &uart_config);
  uart_set_pin(port, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  uart_driver_install(port, 512, 0, 0, NULL, 0);
}

float generatePhase(float linkPhase, float tempo) {
  static float phasor;

  // Calculate phase increment (beats/s times ppqn)
  float dO = tempo / 60.f * 24.f;

  // Subtract from phase retrieved from Link
  dO += (linkPhase - phasor) * 15.f; // gain phase error

  // Mutiply with frame duration
  dO = dO * FRAME_DUR;

  phasor = fmodf(phasor + dO, 1.0f);
  return phasor;
}

void tickTask(void *userParam)
{
  ableton::Link link(120.0f);
  link.enable(true);

  initUartPort(UART_PORT, TX_PIN, RX_PIN);

  if (PRINT_LINK_STATE) {
    xTaskCreate(printTask, "print", 8192, &link, 1, nullptr);
  }

  // Initialize time offset
  std::chrono::microseconds offset = link.clock().micros();

  while (true) {
    const auto state = link.captureAudioSessionState();

    const float linkPhase = state.phaseAtTime(offset, 1./24.) * 24.; // 24 ppqn
    const float phase = generatePhase(linkPhase, state.tempo());
    static float lastPhase;

    uint8_t clk = phase - lastPhase < -0.5f; // check for falling edge
    lastPhase = phase;

    // Send clock value to buffer
    xQueueSend(gBuf, &clk, portMAX_DELAY);
    offset += std::chrono::microseconds(FRAME_DUR_US);
  }
}

extern "C" void app_main()
{
  // Run init task on core 1, to ensure that the clock interrupt is running on core 1
  xTaskCreatePinnedToCore([](void*){
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    esp_wifi_set_ps(WIFI_PS_NONE);

    gBuf = xQueueCreate(BUF_SIZE, 1);

    TaskHandle_t tickTaskHandle;
    xTaskCreate(tickTask, "tick", 8192, nullptr, 10, &tickTaskHandle);

    timerGroup0Init(FRAME_DUR_US, &tickTaskHandle);

    // Delete this task
    vTaskDelete(nullptr);
  }, "init-link", 4096, nullptr, 1, nullptr, 1);

  // Delete this task
  vTaskDelete(nullptr);
}
