#include "i2s_audio_speaker.h"

#ifdef USE_ESP32

#include <driver/i2s.h>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace i2s_audio {

static const size_t BUFFER_COUNT = 20;

static const char *const TAG = "i2s_audio.speaker";

void I2SAudioSpeaker::setup() {
  ESP_LOGCONFIG(TAG, "Setting up I2S Audio Speaker...");

  this->buffer_queue_ = xQueueCreate(BUFFER_COUNT, sizeof(DataEvent));
  this->event_queue_ = xQueueCreate(BUFFER_COUNT, sizeof(TaskEvent));
}

void I2SAudioSpeaker::start() { this->state_ = speaker::STATE_STARTING; }
void I2SAudioSpeaker::start_() {
  if (!this->parent_->try_lock()) {
    return;  // Waiting for another i2s component to return lock
  }
  this->state_ = speaker::STATE_RUNNING;

  xTaskCreate(I2SAudioSpeaker::player_task, "speaker_task", 8192, (void *) this, 1, &this->player_task_handle_);
}

void I2SAudioSpeaker::player_task(void *params) {
  I2SAudioSpeaker *this_speaker = (I2SAudioSpeaker *) params;

  TaskEvent event;
  event.type = TaskEventType::STARTING;
  xQueueSend(this_speaker->event_queue_, &event, portMAX_DELAY);

  i2s_driver_config_t config = {
    .mode = (i2s_mode_t) (this_speaker->parent_->get_i2s_mode() | I2S_MODE_TX),
    .sample_rate = 16000,
    .bits_per_sample = this_speaker->bits_per_sample_,
    .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 128,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = I2S_PIN_NO_CHANGE,
    .mclk_multiple = I2S_MCLK_MULTIPLE_DEFAULT,
    .bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT,
#if SOC_I2S_SUPPORTS_TDM
    .chan_mask = (i2s_channel_t) (I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1),
    .total_chan = 2,
    .left_align = false,
    .big_edin = false,
    .bit_order_msb = false,
    .skip_msk = false,
#endif
  };
#if SOC_I2S_SUPPORTS_DAC
  if (this_speaker->internal_dac_mode_ != I2S_DAC_CHANNEL_DISABLE) {
    config.mode = (i2s_mode_t) (config.mode | I2S_MODE_DAC_BUILT_IN);
  }
#endif

  esp_err_t err_driver = i2s_driver_install(this_speaker->parent_->get_port(), &config, 0, nullptr);

  if ((err_driver != ESP_OK)) {  //|| (err_clk != ESP_OK)) {
    event.type = TaskEventType::WARNING;
    event.err = err_driver;

    xQueueSend(this_speaker->event_queue_, &event, 0);
    event.type = TaskEventType::STOPPED;
    xQueueSend(this_speaker->event_queue_, &event, 0);
    while (true) {
      delay(10);
    }
  }

#if SOC_I2S_SUPPORTS_DAC
  if (this_speaker->internal_dac_mode_ == I2S_DAC_CHANNEL_DISABLE) {
#endif
    i2s_pin_config_t pin_config = this_speaker->parent_->get_pin_config();
    pin_config.data_out_num = this_speaker->dout_pin_;

    i2s_set_pin(this_speaker->parent_->get_port(), &pin_config);
#if SOC_I2S_SUPPORTS_DAC
  } else {
    i2s_set_dac_mode(this_speaker->internal_dac_mode_);
  }
#endif

  DataEvent data_event;

  event.type = TaskEventType::STARTED;
  xQueueSend(this_speaker->event_queue_, &event, portMAX_DELAY);

  int16_t buffer[BUFFER_SIZE / 2];

  while (true) {
    if (xQueueReceive(this_speaker->buffer_queue_, &data_event, 100 / portTICK_PERIOD_MS) != pdTRUE) {
      break;  // End of audio from main thread
    }
    if (data_event.stop) {
      // Stop signal from main thread
      xQueueReset(this_speaker->buffer_queue_);  // Flush queue
      break;
    }
    size_t bytes_written;

    memmove(buffer, data_event.data, data_event.len);
    size_t remaining = data_event.len / 2;
    size_t current = 0;

    const uint8_t vol = remap<uint8_t, float>(this_speaker->volume_, 0.0f, 1.0f, 0, 21);
    const uint8_t m_vol = VOLUME_TABLE[vol];

    while (remaining > 0) {
      int32_t sample = buffer[current];
      // Multiplies sample by the modified volume factor then shifts back. If m_vol = 64, this does nothing.
      int16_t volume_adjusted_sample = sample * m_vol >> 6;

      esp_err_t err;
      if (this_speaker->bits_per_sample_ == I2S_BITS_PER_SAMPLE_16BIT) {
        err = i2s_write(this_speaker->parent_->get_port(), &volume_adjusted_sample, sizeof(volume_adjusted_sample),
                        &bytes_written, (10 / portTICK_PERIOD_MS));
      } else {
        err = i2s_write_expand(this_speaker->parent_->get_port(), &volume_adjusted_sample,
                               sizeof(volume_adjusted_sample), I2S_BITS_PER_SAMPLE_16BIT,
                               this_speaker->bits_per_sample_, &bytes_written, (10 / portTICK_PERIOD_MS));
      }

      if (err != ESP_OK) {
        event = {.type = TaskEventType::WARNING, .err = err};
        xQueueSend(this_speaker->event_queue_, &event, portMAX_DELAY);
        continue;
      }
      remaining--;
      current++;
    }

    event.type = TaskEventType::PLAYING;
    xQueueSend(this_speaker->event_queue_, &event, portMAX_DELAY);
  }

  i2s_zero_dma_buffer(this_speaker->parent_->get_port());

  event.type = TaskEventType::STOPPING;
  xQueueSend(this_speaker->event_queue_, &event, portMAX_DELAY);

  i2s_driver_uninstall(this_speaker->parent_->get_port());

  event.type = TaskEventType::STOPPED;
  xQueueSend(this_speaker->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

void I2SAudioSpeaker::stop() {
  if (this->state_ == speaker::STATE_STOPPED)
    return;
  if (this->state_ == speaker::STATE_STARTING) {
    this->state_ = speaker::STATE_STOPPED;
    return;
  }
  this->state_ = speaker::STATE_STOPPING;
  DataEvent data;
  data.stop = true;
  xQueueSendToFront(this->buffer_queue_, &data, portMAX_DELAY);
}

void I2SAudioSpeaker::watch_() {
  TaskEvent event;
  if (xQueueReceive(this->event_queue_, &event, 0) == pdTRUE) {
    switch (event.type) {
      case TaskEventType::STARTING:
        ESP_LOGD(TAG, "Starting I2S Audio Speaker");
        break;
      case TaskEventType::STARTED:
        ESP_LOGD(TAG, "Started I2S Audio Speaker");
        break;
      case TaskEventType::STOPPING:
        ESP_LOGD(TAG, "Stopping I2S Audio Speaker");
        break;
      case TaskEventType::PLAYING:
        this->status_clear_warning();
        break;
      case TaskEventType::STOPPED:
        this->state_ = speaker::STATE_STOPPED;
        vTaskDelete(this->player_task_handle_);
        this->player_task_handle_ = nullptr;
        this->parent_->unlock();
        xQueueReset(this->buffer_queue_);
        ESP_LOGD(TAG, "Stopped I2S Audio Speaker");
        break;
      case TaskEventType::WARNING:
        ESP_LOGW(TAG, "Error writing to I2S: %s", esp_err_to_name(event.err));
        this->status_set_warning();
        break;
    }
  }
}

void I2SAudioSpeaker::loop() {
  switch (this->state_) {
    case speaker::STATE_STARTING:
      this->start_();
      break;
    case speaker::STATE_RUNNING:
    case speaker::STATE_STOPPING:
      this->watch_();
      break;
    case speaker::STATE_STOPPED:
      break;
  }
}

size_t I2SAudioSpeaker::play(const uint8_t *data, size_t length) {
  if (this->state_ != speaker::STATE_RUNNING && this->state_ != speaker::STATE_STARTING) {
    this->start();
  }
  size_t remaining = length;
  size_t index = 0;
  while (remaining > 0) {
    DataEvent event;
    event.stop = false;
    size_t to_send_length = std::min(remaining, BUFFER_SIZE);
    event.len = to_send_length;
    memcpy(event.data, data + index, to_send_length);
    if (xQueueSend(this->buffer_queue_, &event, 0) != pdTRUE) {
      return index;
    }
    remaining -= to_send_length;
    index += to_send_length;
  }
  return index;
}

bool I2SAudioSpeaker::has_buffered_data() const { return uxQueueMessagesWaiting(this->buffer_queue_) > 0; }

}  // namespace i2s_audio
}  // namespace esphome

#endif  // USE_ESP32