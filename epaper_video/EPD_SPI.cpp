#include "EPD_SPI.h"
#include <SPI.h>

// Hardware SPI rather than the bit-banged loop this file shipped with.
//
// The old EPD_WR_Bus toggled SCK and MOSI with digitalWrite, eight bits at a
// time, and framed CS around every single byte: 26 digitalWrite calls per
// byte, so 390,000 of them to push one 15,000-byte frame. Measured on this
// board that was ~239ms of a 659ms frame -- more than the exposure.
//
// 20MHz is conservative for a UC8176 and leaves plenty of margin on the
// ribbon. MISO is -1 because the panel is write-only, and SS is -1 because CS
// is framed by hand: the controller wants it around a whole transfer, not
// around each byte.
static SPISettings epdSpi(20000000, MSBFIRST, SPI_MODE0);

void EPD_GPIOInit(void)
{
  pinMode(RES, OUTPUT);
  pinMode(DC, OUTPUT);
  pinMode(CS, OUTPUT);
  pinMode(BUSY, INPUT);
  EPD_CS_Set();

  SPI.begin(SCK, -1, MOSI, -1);   // sck, miso (unused), mosi, ss (manual)
  SPI.beginTransaction(epdSpi);   // held for the life of the sketch: nothing
                                  // else on this board touches the SPI bus
}

/**
   @brief       IO模拟SPI发送一个字节数据
   @param       dat: 需要发送的字节数据
   @retval      无
*/
void EPD_WR_Bus(uint8_t dat)
{
  EPD_CS_Clr();
  SPI.transfer(dat);
  EPD_CS_Set();
}

/**
   @brief       Write a whole buffer as data, with CS framed once around it.
   @param       buf: bytes to send   len: how many
   @retval      none

   This is the one that matters. Framing CS per byte costs two digitalWrite
   calls per byte -- 30,000 for a frame -- which is most of what is left once
   the shifting itself is done in hardware. A frame is contiguous, so it can go
   out as a single transfer.
*/
void EPD_WR_DATA_BULK(const uint8_t *buf, uint32_t len)
{
  EPD_DC_Set();
  EPD_CS_Clr();
  SPI.writeBytes(buf, len);
  EPD_CS_Set();
}

/**
   @brief       向液晶写寄存器命令
   @param       reg: 要写的命令
   @retval      无
*/
void EPD_WR_REG(uint8_t reg)
{
  EPD_DC_Clr();
  EPD_WR_Bus(reg);
  EPD_DC_Set();
}

/**
   @brief       向液晶写一个字节数据
   @param       dat: 要写的数据
   @retval      无
*/
void EPD_WR_DATA8(uint8_t dat)
{
  EPD_DC_Set();
  EPD_WR_Bus(dat);
  EPD_DC_Set();
}
