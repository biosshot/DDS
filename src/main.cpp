// #include <INA219.h>
#include <Arduino.h>
#include <encoder.h>
// #include <digipot.h>
// #include <encoder.h>
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"
Encoder encoder(28, 27, 0); // для работы c кнопкой
// int value = 0;
#include <stdio.h>

#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"
#include "hardware/xosc.h"

#include <TFT_eSPI.h> // Graphics and font library
#include <SPI.h>
#include "pico/time.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>

#define SIZE(x) (sizeof(x) / sizeof(x[0]))

TFT_eSPI tft = TFT_eSPI(); // Invoke library, pins defined in User_Setup.h

TFT_eSprite sprite = TFT_eSprite(&tft);

#define BUFFER_SIZE 4096

enum form_type {Sin, Square, Triangular, Saw, ReverseSaw};
form_type Form;

uint8_t sample_points_data[BUFFER_SIZE];
int dmaDataChan;
int dmaCtrlChan;
PIO pio = pio0;
int sm;
int Frequncy{}, Harmonic{}, Offset{};
double Amplitude{};

enum
{
  MA_LEFT,
  MA_RIGHT,
  MA_BTN
};

struct menu_item
{
  char text[24];
  uint16_t x, y, w, h;
  void (*function)(menu_item *, uint8_t);
  void (*render)(menu_item *);
  uint8_t hold;
  uint8_t selected;
};
void generate_waveform(uint8_t buffer[BUFFER_SIZE], int waveform_type, double amplitude, int harmonics) {
  std::vector<double> values(BUFFER_SIZE, 0.0);
  for (int i = 0; i < BUFFER_SIZE; i++) {
    float value{};
      //чиста сигнал
      switch (waveform_type) {
      case 0: // sin
        value = std::sin(2.0 * PI * i / BUFFER_SIZE);
        break;
      case 1: // Square
        value = (i < BUFFER_SIZE / 2) ? 1 : -1;
        break;
      case 2: // Triangle
        value = (std::abs((static_cast<double>(i) / (BUFFER_SIZE - 1)) - 0.5) * 4) - 1;
        break;
      case 3: // Saw 
        value = ((static_cast<double>(i) / (BUFFER_SIZE - 1)) * 2) - 1;
        break;
      case 4: // Reverse Saw
        value = ((1.0 - static_cast<double>(i) / (BUFFER_SIZE - 1)) * 2) - 1;
      default:
         // Best Form
        break;
      }
      if (harmonics > 0) {
        for (int h = 1; h <= harmonics; h++) {
          double g = (1.0 / h) * std::sin(2.0 * PI * h * i / BUFFER_SIZE);
          value += g;
        }
      }
      values[i] = value;
    }
  double max = *std::max_element(values.begin(), values.end());
  double min = *std::min_element(values.begin(), values.end());
  double amp = std::abs(max) + std::abs(min);
  for (int i = 0; i < values.size(); i++) {
      buffer[i] = static_cast<uint8_t>((((values[i] + (amp / 2)) / amp) * amplitude));
    }
}
void render_items(menu_item *item,  char* value, char* unit)
{
  if (item->hold){
    sprite.setTextColor(TFT_RED);
    sprite.drawLine(item->w + 7, item->y - 7, item->w + 120, item->y - 7, TFT_DARKCYAN);
    sprite.drawLine(item->w + 7, item->y + item->h + 2, item->w + 120, item->y + item->h + 2, TFT_DARKCYAN);
  } else {
    sprite.setTextColor(TFT_WHITE);
    sprite.drawLine(item->w + 7, item->y - 7, item->w + 120, item->y - 7, TFT_LIGHTGREY);
    sprite.drawLine(item->w + 7, item->y + item->h + 2, item->w + 120, item->y + item->h + 2, TFT_LIGHTGREY);
  }

  sprite.fillRectHGradient(item->x - 11, item->y - 3, item->w / 2, item->h, TFT_BLACK, TFT_DARKCYAN);
  sprite.fillRectHGradient(item->x - 11 + (item->w / 2), item->y - 3, item->w / 2, item->h, TFT_DARKCYAN, TFT_BLACK);
  sprite.drawString(item->text, item->x,  item->y);

  sprite.drawString(value, item->w + 20,  item->y);
  sprite.drawString(unit, item->w + 95,  item->y+1);
};
void render_Form(menu_item *item)
{
  char form[32] = {0};
  switch (Form)
  {
    case Sin:
      strcpy(form, "\tSin");
      break;
    case Square:
      strcpy(form, "\tSquare");
      break;
    case Triangular:
      strcpy(form, "\tTriangular");
      break;
    case Saw:
      strcpy(form, "\tSaw");
      break;
    case ReverseSaw:
      strcpy(form, "\tReverse Saw");
      break;
    default:
      break;
  }
  char unit[32] = {0};
  render_items(item, form, unit);

}
void render_Ampl(menu_item *item)
{
  char amplitude[32] = { 0 };
  itoa(Amplitude, amplitude, 10);
  char unit[32] = {"Vpp"};
  render_items(item, amplitude, unit);
  
}
void render_Freq(menu_item *item)
{
  char frequency[32] = { 0 };
  itoa(Frequncy, frequency, 10);
  char unit[32] = {" Hz"};
  render_items(item, frequency, unit);
  
}
void render_Harm(menu_item *item)
{
  char harmonic[32] = { 0 };
  itoa(Harmonic, harmonic, 10);
  char unit[32] = {0};
  render_items(item, harmonic, unit);
  
}
void render_Offset(menu_item *item)
{
  char offset[32] = { 0 };
  itoa(Offset, offset, 10);
  char unit[32] = {"Vdc"};
  render_items(item, offset, unit);
  
}

void render_Graph(menu_item *item)
{
  sprite.drawRect(item->x, item->y, item->w, item->h, TFT_DARKCYAN);
  generate_waveform(sample_points_data, Form, Amplitude, Harmonic);
  for (int i = 0, j = 0; i < 255; i++, j += 16) {
    int xo = item->x + i;
    int yo = item->y + item->h  - sample_points_data[j]/3;
    int x = item->x + i+1;
    int y = item->y + item->h - sample_points_data[j+16]/3;
    if (x >= item-> x + item->w){ i = 255;}
    sprite.drawLine(xo, yo, x, y, TFT_DARKCYAN);
  }
}

menu_item menu_items[] = {
    menu_item{"\tForm", 11, 30, 55, 14, NULL, render_Form},
    menu_item{"\tAmpl", 11, 70, 55, 14, NULL, render_Ampl},
    menu_item{"\tFreq", 11, 110, 55, 14, NULL, render_Freq},
    menu_item{"\tHarm", 11, 150, 55, 14, NULL, render_Harm},
    menu_item{"\tOffset", 11, 190, 55, 14, NULL, render_Offset},
    menu_item{"\tGraph", 190, 25, 120, 120, NULL, render_Graph}
};


void init_writer()
{
  dma_channel_config ctrlChanConfig = dma_channel_get_default_config(dmaCtrlChan);
  channel_config_set_transfer_data_size(&ctrlChanConfig, DMA_SIZE_32);
  channel_config_set_read_increment(&ctrlChanConfig, false);
  channel_config_set_write_increment(&ctrlChanConfig, false);
  channel_config_set_chain_to(&ctrlChanConfig, dmaDataChan);
  channel_config_set_irq_quiet(&ctrlChanConfig, true);
  // channel_config_set_high_priority(&ctrlChanConfig, true);
  channel_config_set_enable(&ctrlChanConfig, true);
  dma_channel_configure(
      dmaCtrlChan,
      &ctrlChanConfig,
      &dma_hw->ch[dmaDataChan].read_addr,
      &sample_points_data,
      1,
      false);

  dma_channel_config dataChanConfig = dma_channel_get_default_config(dmaDataChan);
  channel_config_set_transfer_data_size(&dataChanConfig, DMA_SIZE_32);
  channel_config_set_read_increment(&dataChanConfig, true);
  channel_config_set_write_increment(&dataChanConfig, false);
  channel_config_set_dreq(&dataChanConfig, pio_get_dreq(pio, sm, true));
  channel_config_set_chain_to(&dataChanConfig, dmaCtrlChan);
  channel_config_set_irq_quiet(&dataChanConfig, true);
  // channel_config_set_high_priority(&dataChanConfig, true);
  channel_config_set_enable(&dataChanConfig, true);
  dma_channel_configure(
      dmaDataChan,
      &dataChanConfig,
      &pio->txf[sm],
      sample_points_data,
      BUFFER_SIZE / 4,
      false);

  for (int i = 0; i < 8; ++i)
  {
    pio_gpio_init(pio, i);
  }
  pio_sm_set_consecutive_pindirs(pio, sm, 0, 8, true);

  uint16_t out_instr = pio_encode_out(pio_pins, 8);
  pio_program_t pioProgram = pio_program_t{
      .instructions = &out_instr,
      .length = 1,
      .origin = -1,
  };
  uint offset = pio_add_program(pio, &pioProgram);

  // Configure the state machine
  pio_sm_config smConfig = pio_get_default_sm_config();
  sm_config_set_out_pins(&smConfig, 0, 8);
  sm_config_set_out_shift(&smConfig, true, true, 32);
  sm_config_set_wrap(&smConfig, offset, offset);
  sm_config_set_clkdiv(&smConfig, 1024);
  pio_sm_init(pio, sm, offset, &smConfig);

  dma_start_channel_mask((1u << dmaDataChan) | (1u << dmaCtrlChan));
}

void run_writer()
{
  pio_sm_set_enabled(pio, sm, true);
}

void stop_writer()
{
  pio_sm_set_enabled(pio, sm, false);
}

void tft_init()
{
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  sprite.setTextSize (1);

  sprite.createSprite(TFT_HEIGHT, TFT_WIDTH);
  sprite.fillSprite(TFT_BLACK);
}

void render_menu()
{
  for (size_t i = 0; i < SIZE(menu_items); ++i)
  {
    menu_item *item = &menu_items[i];
    item->render(item);
  }
  sprite.pushSprite(0, 0);
}

void parse_input()
{
  static int16_t hold_menu_item = 0;
  menu_item *item = &menu_items[hold_menu_item];
  item->hold = 1;
/*
  if (!(encoder.isTurn() || encoder.isClick()))
  {
    return;
  }
*/
  if (encoder.isLeft())
  {
    Serial.print("Levo\n");
    if (item->selected)
    {
      if (item->function)
        item->function(item, MA_LEFT);
    }
    else
    {
      item->hold = 0;

      hold_menu_item = hold_menu_item - 1;

      if (hold_menu_item < 0)
      {
        hold_menu_item = SIZE(menu_items) - 1;
      }
    }
  }
  else if (encoder.isRight())
  {
    Serial.print("Pravo\n");
    if (item->selected)
    {
      if (item->function)
        item->function(item, MA_RIGHT);
    }
    else
    {
      item->hold = 0;

      hold_menu_item = hold_menu_item + 1;

      if (hold_menu_item >= SIZE(menu_items))
      {
        hold_menu_item = 0;
      }
    }
  }
  /*else if (encoder.isClick())
  {
    if (encoder.isDouble())
    {
      item->selected = !item->selected;
    }
    else
    {
      if (item->function)
        item->function(item, MA_BTN);
    }
  }*/
}

bool __not_in_flash_func(timer_cb)(struct repeating_timer *t)
{ encoder.tick(); return true;
  parse_input();
}


void setup()
{
  Form = Sin;
  Amplitude = 1000; Frequncy = 1000;
  static struct repeating_timer timer;
  add_repeating_timer_ms(5, timer_cb, NULL, &timer);
  tft_init();
  gpio_set_dir(8, 0);

  encoder.setType(TYPE2);

  dmaDataChan = dma_claim_unused_channel(true);
  dmaCtrlChan = dma_claim_unused_channel(true);

  sm = pio_claim_unused_sm(pio, true);

  init_writer();

  run_writer();

}

void loop()
{
  render_menu();
}
