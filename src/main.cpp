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


#define SIZE(x) (sizeof(x) / sizeof(x[0]))

TFT_eSPI tft = TFT_eSPI(); // Invoke library, pins defined in User_Setup.h

TFT_eSprite sprite = TFT_eSprite(&tft);

#define BUFFER_SIZE 4096

uint8_t sample_points_data[BUFFER_SIZE];
int dmaDataChan;
int dmaCtrlChan;
PIO pio = pio0;
int sm;

int selectedOption = 0;

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
void render_items(menu_item *item)
{
  if (item->hold){
    tft.setTextColor(TFT_YELLOW);
  } else {
    tft.setTextColor(TFT_WHITE);
  }
  tft.setTextSize (2);
  tft.fillRectHGradient(item->x - 22, item->y - 3, item->w / 2, item->h, TFT_BLACK, TFT_DARKCYAN);
  tft.fillRectHGradient(item->x - 22 + (item->w / 2), item->y - 3, item->w / 2, item->h, TFT_DARKCYAN, TFT_BLACK);
  tft.drawString(item->text, item->x,  item->y);
};
menu_item menu_items[] = {
    menu_item{"Форма", 22, 30, 100, 20, NULL, render_items},
    menu_item{"Ампл.", 22, 70, 100, 20, NULL, render_items},
    menu_item{"Част.", 22, 110, 100, 20, NULL, render_items},
    menu_item{"Гарм.", 22, 150, 100, 20, NULL, render_items},
    menu_item{"Смещ.", 22, 190, 100, 20, NULL, render_items}

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

  sprite.createSprite(TFT_WIDTH, TFT_HEIGHT);
  sprite.fillSprite(TFT_BLACK);
}

void render_menu()
{
  for (size_t i = 0; i < SIZE(menu_items); ++i)
  {
    menu_item *item = &menu_items[i];
    item->render(item);
  }
}
void parse_input()
{
  encoder.tick();

  static int16_t hold_menu_item = 0;
  menu_item *item = &menu_items[hold_menu_item];
  item->hold = 1;

  if (encoder.isLeft())
  {
      item->hold = 0;

      hold_menu_item = hold_menu_item - 1;

      if (hold_menu_item < 0)
      {
        hold_menu_item = SIZE(menu_items) - 1;
      }
  }
  else if (encoder.isRight())
  {
      item->hold = 0;

      hold_menu_item = hold_menu_item + 1;

      if (hold_menu_item >= SIZE(menu_items))
      {
        hold_menu_item = 0;
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
void setup()
{
  Serial.begin(9600);

  tft_init();

  gpio_set_dir(8, 0);

  encoder.setType(TYPE2);

  for (int i = 0; i < BUFFER_SIZE; ++i)
  {
    sample_points_data[i] = (i % 2 == 0) ? 0 : 255;
  }

  dmaDataChan = dma_claim_unused_channel(true);
  dmaCtrlChan = dma_claim_unused_channel(true);

  sm = pio_claim_unused_sm(pio, true);

  init_writer();

  run_writer();
}

void loop()
{
  parse_input();
  render_menu();
}
