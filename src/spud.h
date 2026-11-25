/** @file spud.h
    @defgroup spud SPUD
    @brief Spud definitions.

  Definitions for pins on Spud v1.XX 

*/

#ifndef SPUD_H
#define SPUD_H

#if SPUD_VERSION > 1

#else

/** \addtogroup spud

    @{    
*/

#define AI0 GPIO_NUM_33 //!< Analog input 0
#define AI1 GPIO_NUM_32 //!< Analog input 1
#define AI2 GPIO_NUM_35 //!< Analog input 2
#define AI3 GPIO_NUM_34 //!< Analog input 3
#define AI4 GPIO_NUM_39 //!< Analog input 4
#define AI5 GPIO_NUM_36 //!< Analog input 5

#define SW0 GPIO_NUM_23 //!< High power (10A) switch 0
#define SW1 GPIO_NUM_22 //!< High power (10A) switch 1
#define SW2 GPIO_NUM_21 //!< High power (10A) switch 2
#define SW3 GPIO_NUM_27 //!< High power (10A) switch 3
#define SW4 GPIO_NUM_26 //!< High power (10A) switch 4
#define SW5 GPIO_NUM_25 //!< High power (10A) switch 5

#define DO0 GPIO_NUM_4 //!< 5V/12V input/output 0
#define DO1 GPIO_NUM_16 //!< 5V/12V input/output 1
#define DO2 GPIO_NUM_18 //!< 5V/12V input/output 2
#define DO3 GPIO_NUM_19 //!< 5V/12V input/output 3

#define SDO GPIO_NUM_13 //!< SPI output (MOSI)
#define SDI GPIO_NUM_12 //!< SPI input (MISO)
#define SCLK  GPIO_NUM_14//!< SPI clock

#define CS0  GPIO_NUM_15 //!< SPI chip select CAN-0
#define INT0  GPIO_NUM_17 //!< SPI interrupt CAN-0

#define CS1  GPIO_NUM_2 //!< SPI chip select CAN-1
#define INT1  GPIO_NUM_5 //!< SPI interrupt CAN-1

#define CAN0_CS  GPIO_NUM_15 //!< SPI chip select CAN-0 (alias)
#define CAN0_INT  GPIO_NUM_17 //!< SPI interrupt CAN-0 (alias)

#define CAN1_CS  GPIO_NUM_2 //!< SPI chip select CAN-1
#define CAN1_INT  GPIO_NUM_5 //!< SPI interrupt CAN-1 (alias)

/** @brief CAN operating modes */
typedef enum {
  CAN_MODE_STANDARD = 0,   //!< Standard CAN 2.0B mode
  CAN_MODE_FD = 1,         //!< CAN FD mode
  CAN_MODE_LOOPBACK = 2,   //!< Loopback mode for testing
  CAN_MODE_LISTEN_ONLY = 3,//!< Listen-only mode (no ACK)
  CAN_MODE_NORMAL = 0      //!< Alias for standard mode
} can_mode_t;

/** @} */

#endif /* SPUD_VERSION == 1.0 */

#endif /* SPUD_H include guard */