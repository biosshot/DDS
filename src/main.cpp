#include <Arduino.h>
#include <encoder.h>
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"
#include <stdio.h>

#include "hardware/dma.h"
#include "hardware/pio.h"

#include <TFT_eSPI.h> // Graphics and font library
#include <SPI.h>
#include "pico/time.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include "hardware/pwm.h"
#include "74HC4051D.h"
#include "pico/stdlib.h"

#define SIZE(x) (sizeof(x) / sizeof(x[0]))
#define MAX_BUFFER_SIZE 4000
#define PREVIEW_BUFFER_SIZE 256
#define R2R_TOP_VOLTAGE_MV 2800
#define FREQ_DIGITS 8
#define MAX_FREQ 15'000'000

HC4051D hc4051{9, 29, 10};

TFT_eSPI tft = TFT_eSPI(); // Invoke library, pins defined in User_Setup.h
TFT_eSprite sprite = TFT_eSprite(&tft);

Encoder encoder(28, 27, 26); // для работы c кнопкой

uint8_t ampl_k = 200;

const uint cpu_freq = 240'000;

enum
{
  SIN,
  SQUARE,
  TRIANGULAR,
  SAW,
  REVERSE_SAW,
  FORM_COUNT
};

enum
{
  MA_LEFT,
  MA_RIGHT,
  MA_PRESS_LEFT,
  MA_PRESS_RIGHT,
  MA_BTN
};

struct
{
  int8_t form = SIN;
  uint32_t frequncy = 1'000;
  uint16_t harmonic = 0;
  int16_t offset = 0;
  int16_t amplitude = 2000;
  uint8_t ampl_k = 200;
  float fdiv = 1;
  uint32_t _top_buffer = MAX_BUFFER_SIZE;
} signal;

uint8_t write_sample_points_data[MAX_BUFFER_SIZE];
uint8_t *samplePointsData = write_sample_points_data;

uint8_t sample_points_data[PREVIEW_BUFFER_SIZE];
int dmaDataChan;
int dmaCtrlChan;
PIO pio = pio1;
uint sm;

struct menu_item
{
  char text[24];
  uint16_t x, y, w, h;
  void (*function)(menu_item *, uint8_t);
  void (*render)(menu_item *);
  uint8_t hold = 0;
  uint8_t selected = 0;
  int32_t data = 0;
};

void stop_writer();
void run_writer();
void set_offset(int16_t offset_mv);

inline uint8_t __not_in_flash_func(reverse_bits)(uint8_t b)
{
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
  b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
  return b;
}

struct
{
  float div;
  uint8_t ch;
} divs[] = {{9.74, 2}, {4.87, 1}, {1.98, 0}, {1, 3}};

void generate_waveform(uint8_t *buffer, bool is_output = false)
{
  uint32_t buffer_size = is_output ? signal._top_buffer : PREVIEW_BUFFER_SIZE;

  static double values[MAX_BUFFER_SIZE]{0};

  for (int i = 0; i < buffer_size; i++)
  {
    float value{};
    switch (signal.form)
    {
    case 0: // SIN
      value = std::sin(2.0 * PI * i / buffer_size);
      break;
    case 1: // SQUARE
      value = (i < buffer_size / 2) ? 1 : -1;
      break;
    case 2: // Triangle
      value = (std::abs((static_cast<double>(i) / (buffer_size - 1)) - 0.5) * 4) - 1;
      break;
    case 3: // SAW
      value = ((static_cast<double>(i) / (buffer_size - 1)) * 2) - 1;
      break;
    case 4: // Reverse SAW
      value = ((1.0 - static_cast<double>(i) / (buffer_size - 1)) * 2) - 1;
      break;
    default:
      value = 0.0;
      break;
    }

    if (signal.harmonic > 0)
    {
      for (int h = 1; h <= signal.harmonic; h++)
      {
        double g = (1.0 / h) * std::sin(2.0 * PI * h * static_cast<double>(i) / buffer_size);
        value += g;
      }
    }

    values[i] = value;
  }

  double max = *std::max_element(&values[0], &values[buffer_size - 1]);
  double min = *std::min_element(&values[0], &values[buffer_size - 1]);
  double range = max - min;

  float ampl = is_output ? float(signal.ampl_k) : 120;

  for (int i = 0; i < buffer_size; ++i)
  {
    buffer[i] = uint8_t(((values[i] - min) / range) * ampl);

    if (is_output)
    {
      buffer[i] = reverse_bits(buffer[i]);
    }
  }
}

void update_waveform()
{
  generate_waveform(sample_points_data);

  stop_writer();
  generate_waveform(write_sample_points_data, true);
  run_writer();
}

void set_freq(uint32_t freq)
{
  if (freq == 0)
  {
    stop_writer();
    return;
  }

  uint32_t freq_hz = (cpu_freq * 1000);
  uint32_t buff_size = (freq_hz) / freq;
  buff_size -= buff_size % 4;

  if (buff_size > MAX_BUFFER_SIZE)
  {
    buff_size = MAX_BUFFER_SIZE;
  }

  float fdiv = 1;

  fdiv = (float)freq_hz / (float)freq / (float)buff_size;

  signal._top_buffer = buff_size;
  signal.fdiv = fdiv;

  Serial.printf("top %d, div %f\n", signal._top_buffer, signal.fdiv);

  update_waveform();
}

void set_ampl(uint16_t out_ampl)
{
  signal.amplitude = out_ampl;

  float ampl = (out_ampl / 2.f) * 1.33;
  float dist = MAXFLOAT;
  uint8_t best_div = 0;

  for (uint8_t i = 0; i < SIZE(divs); ++i)
  {
    float t_ampl = (ampl * divs[i].div);

    if (t_ampl > R2R_TOP_VOLTAGE_MV)
    {
      continue;
    }

    float cdist = abs(R2R_TOP_VOLTAGE_MV - t_ampl);

    if (cdist < dist)
    {
      best_div = i;
      dist = cdist;
    }
  }

  signal.ampl_k = 255 * ((ampl * divs[best_div].div) / 3300);

  hc4051.set_channel(divs[best_div].ch);

  update_waveform();
}

void render_items(menu_item *item, char *value, char *unit)
{

  uint8_t line_color;

  if (item->selected)
  {
    sprite.setTextColor(TFT_CYAN);
    line_color = TFT_DARKCYAN;
  }
  else if (item->hold)
  {
    sprite.setTextColor(TFT_YELLOW);
    line_color = TFT_DARKCYAN;
  }
  else
  {
    sprite.setTextColor(TFT_WHITE);
    line_color = TFT_LIGHTGREY;
  }

  sprite.drawLine(item->w + 7, item->y - 7, item->w + 120, item->y - 7, line_color);
  sprite.drawLine(item->w + 7, item->y + item->h + 2, item->w + 120, item->y + item->h + 2, line_color);

  sprite.fillRectHGradient(item->x - 11, item->y - 3, item->w / 2, item->h, TFT_BLACK, TFT_DARKCYAN);
  sprite.fillRectHGradient(item->x - 11 + (item->w / 2), item->y - 3, item->w / 2, item->h, TFT_DARKCYAN, TFT_BLACK);
  sprite.drawString(item->text, item->x, item->y);

  sprite.drawString(value, item->w + 20, item->y);
  sprite.drawString(unit, item->w + 95, item->y + 1);
};

void render_form(menu_item *item)
{
  char form[32] = {0};
  switch (signal.form)
  {
  case SIN:
    strcpy(form, "\tSIN");
    break;
  case SQUARE:
    strcpy(form, "\tSQUARE");
    break;
  case TRIANGULAR:
    strcpy(form, "\tTRIANGULAR");
    break;
  case SAW:
    strcpy(form, "\tSAW");
    break;
  case REVERSE_SAW:
    strcpy(form, "\tReverse SAW");
    break;
  default:
    break;
  }
  char unit[32] = {0};
  render_items(item, form, unit);
}

void render_ampl(menu_item *item)
{
  char amplitude[32] = {0};
  itoa(signal.amplitude, amplitude, 10);
  char unit[32] = {"mVpp"};
  render_items(item, amplitude, unit);
}

void render_freq(menu_item *item)
{
  char frequency[FREQ_DIGITS + 1]{0};
  char frequency_tmp[FREQ_DIGITS + 1]{0};

  itoa(signal.frequncy, frequency_tmp, 10);
  uint8_t len = strlen(frequency_tmp);

  uint8_t free_char = FREQ_DIGITS - len;

  for (size_t i = 0; i < free_char; i++)
  {
    frequency[i] = '0';
  }

  for (size_t i = free_char; i < FREQ_DIGITS; i++)
  {
    frequency[i] = frequency_tmp[i - free_char];
  }

  char unit[32] = {" Hz"};
  render_items(item, frequency, unit);

  uint8_t one_digit_width = sprite.textWidth("0");

  sprite.drawString("_", item->w + 20 + (one_digit_width * (FREQ_DIGITS - 1 - item->data)), item->y + 5);
}

void render_harm(menu_item *item)
{
  char harmonic[32] = {0};
  itoa(signal.harmonic, harmonic, 10);
  char unit[32] = {0};
  render_items(item, harmonic, unit);
}

void render_offset(menu_item *item)

{
  char offset[32] = {0};
  itoa(signal.offset, offset, 10);
  char unit[32] = {"mVdc"};
  render_items(item, offset, unit);
}

void render_graph(menu_item *item)
{
  sprite.drawRect(item->x, item->y, item->w, item->h, TFT_DARKCYAN);
  sprite.drawLine(item->x, item->y + item->h / 2, item->x + item->w, item->y + item->h / 2, TFT_DARKCYAN);

  for (int i = 0; i < PREVIEW_BUFFER_SIZE - 1; i++)
  {
    int xo = item->x + ((static_cast<double>(i) / (PREVIEW_BUFFER_SIZE - 1)) * item->w);
    int x = item->x + ((static_cast<double>(i + 1) / (PREVIEW_BUFFER_SIZE - 1)) * item->w);
    int yo = item->y + item->h - static_cast<int>(sample_points_data[i]);
    int y = item->y + item->h - static_cast<int>(sample_points_data[i + 1]);
    sprite.drawLine(xo, yo, x, y, TFT_CYAN);
  }
}

void form_item_action(menu_item *item, uint8_t action)
{
  signal.form += action == MA_LEFT ? 1 : -1;
  signal.form = (signal.form + FORM_COUNT) % FORM_COUNT;

  update_waveform();
}

void ampl_item_action(menu_item *item, uint8_t action)
{
  signal.amplitude += action == MA_LEFT ? 50 : -50;
  signal.amplitude = min(5000, max(signal.amplitude, 0));

  set_ampl(signal.amplitude);
}

void freq_item_action(menu_item *item, uint8_t action)
{
  if (action == MA_PRESS_LEFT)
  {
    item->data += 1;
  }
  else if (action == MA_PRESS_RIGHT)
  {
    item->data -= 1;
  }

  item->data = (item->data + FREQ_DIGITS) % FREQ_DIGITS;
  // Serial.println(action);

  // Serial.println(item->data);

  if (action == MA_LEFT)
  {
    signal.frequncy += pow(10, item->data);
  }
  else if (action == MA_RIGHT)
  {
    signal.frequncy -= pow(10, item->data);
  }

  signal.frequncy = min(MAX_FREQ, max(signal.frequncy, 0));

  set_freq(signal.frequncy);
}

void harm_item_action(menu_item *item, uint8_t action)
{
  signal.harmonic += action == MA_LEFT ? 1 : -1;
  signal.harmonic = min(8, max(signal.harmonic, 0));

  update_waveform();
}

void offest_item_action(menu_item *item, uint8_t action)
{
  signal.offset += action == MA_LEFT ? 50 : -50;
  signal.offset = min(5000, max(signal.offset, -5000));
  // generate_waveform(sample_points_data);
  set_offset(signal.offset);
}

menu_item menu_items[] = {
    menu_item{"\tForm", 11, 30, 55, 14, form_item_action, render_form},
    menu_item{"\tAmpl", 11, 70, 55, 14, ampl_item_action, render_ampl},
    menu_item{"\tFreq", 11, 110, 55, 14, freq_item_action, render_freq},
    menu_item{"\tHarm", 11, 150, 55, 14, harm_item_action, render_harm},
    menu_item{"\tOffset", 11, 190, 55, 14, offest_item_action, render_offset},
    menu_item{"\tGraph", 190, 25, 120, 120, NULL, render_graph}};

void run_writer()
{
  pio_sm_set_enabled(pio, sm, false);
  pio_sm_clear_fifos(pio, sm);
  pio_sm_restart(pio, sm);

  pio_sm_set_clkdiv(pio, sm, signal.fdiv);

  dma_channel_cleanup(dmaCtrlChan);
  dma_channel_cleanup(dmaDataChan);

  // Allocate the PIO state machine
  // Configure the control channel to restart the data channel when it's done
  auto ctrlChanConfig = dma_channel_get_default_config(dmaCtrlChan);
  channel_config_set_transfer_data_size(&ctrlChanConfig, DMA_SIZE_32);
  channel_config_set_read_increment(&ctrlChanConfig, false);
  channel_config_set_write_increment(&ctrlChanConfig, false);
  channel_config_set_chain_to(&ctrlChanConfig, dmaDataChan);
  channel_config_set_irq_quiet(&ctrlChanConfig, true);
  channel_config_set_high_priority(&ctrlChanConfig, true);
  channel_config_set_enable(&ctrlChanConfig, true);
  dma_channel_configure(
      dmaCtrlChan,
      &ctrlChanConfig,
      // Write to the read address of the data channel
      &dma_hw->ch[dmaDataChan].read_addr,
      // Read from the sample points data pointer
      (uint8_t **)&samplePointsData,
      // One 32-bit word
      1,
      // Don't start yet
      false);

  // Configure the data channel to output 32 bits at a time to the PIO state machine
  auto dataChanConfig = dma_channel_get_default_config(dmaDataChan);
  channel_config_set_transfer_data_size(&dataChanConfig, DMA_SIZE_32);
  channel_config_set_read_increment(&dataChanConfig, true);
  channel_config_set_write_increment(&dataChanConfig, false);
  channel_config_set_dreq(&dataChanConfig, pio_get_dreq(pio, sm, true));
  channel_config_set_chain_to(&dataChanConfig, dmaCtrlChan);
  channel_config_set_irq_quiet(&dataChanConfig, true);
  channel_config_set_high_priority(&dataChanConfig, true);
  channel_config_set_enable(&dataChanConfig, true);
  dma_channel_configure(
      dmaDataChan,
      &dataChanConfig,
      // Write to the FIFO
      &pio->txf[sm],
      // Read from the sample points data
      (uint8_t *)samplePointsData,
      // 4096 samples, 32 bits at a time
      signal._top_buffer / 4,
      // Don't start yet
      false);

  dma_channel_start(dmaCtrlChan);
  dma_channel_start(dmaDataChan);

  pio_sm_set_enabled(pio, sm, true);
}

void stop_writer()
{
  // dma_start_channel_mask((1u << dmaDataChan) | (1u << dmaCtrlChan));
  dma_channel_abort(dmaCtrlChan);
  dma_channel_abort(dmaDataChan);

  dma_channel_cleanup(dmaCtrlChan);
  dma_channel_cleanup(dmaDataChan);

  pio_sm_set_enabled(pio, sm, false);
}

void tft_init()
{
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  sprite.setTextSize(1);

  sprite.createSprite(TFT_HEIGHT, TFT_WIDTH);
  sprite.fillSprite(TFT_BLACK);
}

void render_menu()
{
  sprite.fillRect(0, 0, TFT_HEIGHT, TFT_WIDTH, TFT_BLACK);

  for (size_t i = 0; i < SIZE(menu_items); ++i)
  {
    menu_item *item = &menu_items[i];
    item->render(item);
  }

  sprite.pushSprite(0, 0);
}

static int16_t hold_menu_item = 0;

void execute_menu_function(menu_item *item, int action)
{
  if (item->selected && item->function)
  {
    item->function(item, action);
  }
}

void update_hold_menu_item(menu_item *item, int direction)
{
  item->hold = 0;
  hold_menu_item += direction;
  hold_menu_item = (hold_menu_item + SIZE(menu_items)) % SIZE(menu_items);
}

void encoder_turn(menu_item *item)
{

  if (encoder.isLeftH())
  {
    if (item->selected)
      execute_menu_function(item, MA_PRESS_LEFT);
  }
  else if (encoder.isLeft())
  {
    if (!item->selected)
      update_hold_menu_item(item, -1);
    else
      execute_menu_function(item, MA_LEFT);
  }
  else if (encoder.isRightH())
  {
    if (item->selected)
      execute_menu_function(item, MA_PRESS_RIGHT);
  }
  else if (encoder.isRight())
  {
    if (!item->selected)
      update_hold_menu_item(item, 1);
    else
      execute_menu_function(item, MA_RIGHT);
  }
}

void parse_input()
{
  menu_item *item = &menu_items[hold_menu_item];
  item->hold = 1;
  bool is_click = encoder.isDouble();

  encoder_turn(item);

  if (is_click)
  {
    item->selected = !item->selected;
    // Serial.println("Click");
  }
}

bool __not_in_flash_func(timer_cb)(struct repeating_timer *t)
{
  encoder.tick();
  parse_input();
  return true;
}

struct pwm_calc_result
{
  double dutycycle;
  double frequency;
  int div;
  int steps;
  int high;
};

pwm_calc_result calc_pwm(double frequency, double dutycycle)
{
  uint32_t pwm_clock = clock_get_hz(clk_sys);

  double norm = pwm_clock / frequency;
  int div = (int)(norm / 65536) + 1;
  double newf = pwm_clock / div;
  int steps = (int)(newf / frequency);
  int high = (int)(dutycycle * steps / 100.0);

  return {dutycycle, frequency, div, steps, high};
}

void set_offset(int16_t offset_mv)
{
  uint8_t pwm_offset_pin = 8;

  gpio_set_function(pwm_offset_pin, GPIO_FUNC_PWM);

  double duty = ((1.f / (6600.f)) * (offset_mv - 25) + 0.5) * 100.f;

  pwm_calc_result par = calc_pwm(100'000, duty);

  uint slice = pwm_gpio_to_slice_num(pwm_offset_pin);
  pwm_set_clkdiv_mode(slice, PWM_DIV_FREE_RUNNING);
  pwm_set_clkdiv_int_frac(slice, par.div, 0);
  pwm_set_wrap(slice, par.steps);
  pwm_set_gpio_level(pwm_offset_pin, par.high);
  pwm_set_enabled(slice, true);
}

void setup()
{
  set_sys_clock_khz(cpu_freq, 1);

  tft_init();

  encoder.setType(TYPE2);

  set_offset(0);

  sm = pio_claim_unused_sm(pio, true);
  dmaDataChan = dma_claim_unused_channel(true);
  dmaCtrlChan = dma_claim_unused_channel(true);

  // Configure pins 0-7 to to PIO output
  for (auto i = 0; i < 8; ++i)
  {
    pio_gpio_init(pio, i);
  }

  pio_sm_set_consecutive_pindirs(pio, sm, 0, 8, true);

  // Build a simple PIO program that outputs 8 bits at a time
  std::uint16_t out_instr = pio_encode_out(pio_pins, 8);
  auto pioProgram = pio_program_t{
      .instructions = &out_instr,
      .length = 1,
      .origin = -1,
  };
  auto offset = pio_add_program(pio, &pioProgram);

  // Configure the state machine
  auto smConfig = pio_get_default_sm_config();
  sm_config_set_out_pins(&smConfig, 0, 8);
  sm_config_set_out_shift(&smConfig, true, true, 32);
  sm_config_set_wrap(&smConfig, offset, offset);
  sm_config_set_clkdiv(&smConfig, 1);
  pio_sm_init(pio, sm, offset, &smConfig);

  set_ampl(signal.amplitude);
  set_freq(signal.frequncy);

  update_waveform();

  run_writer();

  static struct repeating_timer timer;
  add_repeating_timer_ms(1, timer_cb, NULL, &timer);
}

void loop()
{
  render_menu();
}