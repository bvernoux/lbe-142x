#ifndef LBE_COMMON_H
#define LBE_COMMON_H

// Device definitions
#define VID_LBE 0x1dd2
#define PID_LBE_1420 0x2443
#define PID_LBE_1421 0x2444 // LBE-1421 Dual Output
#define PID_LBE_1423 0x226f // LBE-1423 differential pps

/* Device status bits */
#define LBE_GPS_LOCK_BIT  (1 << 0)
#define LBE_PLL_LOCK_BIT  (1 << 1)
#define LBE_ANT_OK_BIT    (1 << 2)
#define LBE_LED1_BIT      (1 << 3)
#define LBE_LED2_BIT      (1 << 4)
#define LBE_OUT1_EN_BIT   (1 << 5)
#define LBE_OUT2_EN_BIT   (1 << 6)
#define LBE_PPS_EN_BIT    (1 << 7)

/* Command codes */
#define LBE_142X_EN_OUT      0x01
#define LBE_142X_BLINK_OUT   0x02
#define LBE_1421_SET_F1_TEMP 0x05
#define LBE_1421_SET_F1      0x06
#define LBE_1421_SET_F2_TEMP 0x09
#define LBE_1421_SET_F2      0x0A
#define LBE_142X_SET_PLL     0x0B
#define LBE_1421_SET_PPS     0x0C
#define LBE_1421_SET_PWR1    0x0D
#define LBE_1421_SET_PWR2    0x0E

/* Compatibility for LBE-1420 */
#define LBE_1420_SET_F1_TEMP 0x03
#define LBE_1420_SET_F1      0x04
#define LBE_1420_SET_PWR1    0x07

/* Max supported frequency in Hz */
#define LBE_1420_MAX_FREQ 1600000000UL
#define LBE_1421_MAX_FREQ 1400000000UL

#endif // LBE_COMMON_H
