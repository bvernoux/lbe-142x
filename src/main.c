#include "lbe_device.h"
#include "lbe_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void print_usage(int model) {
	unsigned long max_freq = LBE_1421_MAX_FREQ;

	if (model == LBE_1420) {
		max_freq = LBE_1420_MAX_FREQ;
	} else if (model == LBE_MINI) {
		max_freq = LBE_MINI_MAX_FREQ;
	}

	printf("Usage: lbe-142x [OPTIONS]\n");
	printf("Options:\n");
	printf("  --f1 <freq> Set frequency for output 1 (1-%lu Hz) and save to flash\n", max_freq);
	printf("  --f1t <freq> Set temporary frequency for output 1\n");
	printf("  --f2 <freq> Set frequency for output 2 (1-%lu Hz) and save to flash (LBE-1421 only)\n", max_freq);
	printf("  --f2t <freq> Set temporary frequency for output 2 (LBE-1421 only)\n");
	printf("  --out <0|1> Enable or disable outputs\n");
	printf("  --pll <0|1> Set PLL(0) or FLL(1) mode\n");
	printf("  --pps <0|1> Enable or disable 1PPS on OUT1 (LBE-1421 only)\n");
	printf("  --pwr1 <0|1> Set OUT1 power level: normal(0) or low(1)\n");
	printf("  --pwr2 <0|1> Set OUT2 power level: normal(0) or low(1) (LBE-1421 only)\n");
	printf("  --blink Blink output LED(s) for 3 seconds\n");
	printf("  --status Display current device status\n");
}

int main(int argc, char *argv[]) {
	struct lbe_device *dev;
	struct lbe_status status;
	enum lbe_model model;
	int changed = 0;
	unsigned long max_freq = LBE_1421_MAX_FREQ;

	printf("lbe-142x v1.1 17 Feb 2024 Leo Bodnar LBE-142x GPS locked clock source config\n");

	dev = lbe_open_device();
	if (!dev) {
		fprintf(stderr, "Failed to open LBE-142x device\n");
		return 1;
	}

	model = lbe_get_model(dev);

	if (argc == 1) {
		print_usage(model);
		return 1;
	}

	printf("Connected to LBE-%s\n",
	       model == LBE_1420 ? "1420" :
	       model == LBE_MINI ? "Mini" : "1421 dual output");

	if (model == LBE_1420) {
		max_freq = LBE_1420_MAX_FREQ;
	} else if (model == LBE_MINI) {
		max_freq = LBE_MINI_MAX_FREQ;
	}

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--f1") == 0 || strcmp(argv[i], "--f2") == 0 || 
			strcmp(argv[i], "--f1t") == 0 || strcmp(argv[i], "--f2t") == 0) {
			if (i + 1 < argc) {
				int out_no = (argv[i][3] == '1') ? 1 : 2;
				int temp = (argv[i][4] == 't');
				
				if (out_no == 2 && (model == LBE_1420 || model == LBE_MINI)) {
					fprintf(stderr, "This model does not support output 2\n");
					continue;
				}

				uint32_t new_freq = atoi(argv[++i]);
				if (new_freq >= 1 && new_freq <= max_freq) {
					if (temp) {
						if (lbe_set_frequency_temp(dev, out_no, new_freq) == 0) {
							printf("  Setting OUT%d temporary frequency: %u Hz\n", out_no, new_freq);
							changed = 1;
						}
					} else {
						if (lbe_set_frequency(dev, out_no, new_freq) == 0) {
							printf("  Setting OUT%d frequency and saving to flash: %u Hz\n", out_no, new_freq);
							changed = 1;
						}
					}
				} else {
					fprintf(stderr, "Invalid frequency: %u (range: 1-%lu Hz)\n", new_freq, max_freq);
				}
			}
		} else if (strcmp(argv[i], "--out") == 0) {
			if (i + 1 < argc) {
				int enable = atoi(argv[++i]);
				if (enable == 0 || enable == 1) {
					if (lbe_set_outputs_enable(dev, enable) == 0) {
						printf("  Set output(s) to %s\n", enable ? "enabled" : "disabled");
						changed = 1;
					}
				} else {
					fprintf(stderr, "Invalid output state: %d\n", enable);
				}
			}
		} else if (strcmp(argv[i], "--pll") == 0) {
			if (i + 1 < argc) {
				int fll_mode = atoi(argv[++i]);
				if (fll_mode == 0 || fll_mode == 1) {
					if (lbe_set_pll_mode(dev, fll_mode) == 0) {
						printf("  Set %s mode\n", fll_mode ? "FLL" : "PLL");
						changed = 1;
					}
				} else {
					fprintf(stderr, "Invalid PLL/FLL mode: %d\n", fll_mode);
				}
			}
		} else if (strcmp(argv[i], "--pps") == 0) {
			if (model != LBE_1421_DUALOUT) {
				fprintf(stderr, "1PPS on OUT1 control is only supported on LBE-1421\n");
				continue;
			}
			if (i + 1 < argc) {
				int enable = atoi(argv[++i]);
				if (enable == 0 || enable == 1) {
					if (lbe_set_1pps(dev, enable) == 0) {
						printf("  Set 1PPS on OUT1 to %s\n", enable ? "enabled" : "disabled");
						changed = 1;
					}
				} else {
					fprintf(stderr, "Invalid 1PPS state: %d\n", enable);
				}
			}
		} else if (strcmp(argv[i], "--pwr1") == 0 || strcmp(argv[i], "--pwr2") == 0) {
			int out_no = argv[i][5] - '0';
			if (out_no == 2 && (model == LBE_1420 || model == LBE_MINI)) {
				fprintf(stderr, "This model does not support output 2\n");
				continue;
			}
			if (i + 1 < argc) {
				int low_power = atoi(argv[++i]);
				if (low_power == 0 || low_power == 1) {
					if (lbe_set_power_level(dev, out_no, low_power) == 0) {
						printf("  Set OUT%d power to %s\n", out_no, low_power ? "low" : "normal");
						changed = 1;
					}
				} else {
					fprintf(stderr, "Invalid power level: %d\n", low_power);
				}
			}
		} else if (strcmp(argv[i], "--blink") == 0) {
			if (lbe_blink_leds(dev) == 0) {
				printf("  Blink LED(s)\n");
				changed = 1;
			}
		} else if (strcmp(argv[i], "--status") == 0) {
			if (lbe_get_device_status(dev, &status) == 0) {
				printf("Device Status (0x%02X):\n", status.raw_status);
				printf("  GPS Lock: %s\n", (status.raw_status & LBE_GPS_LOCK_BIT) ? "Yes" : "No");
				printf("  PLL Lock: %s\n", status.pll_locked ? "Yes" : "No");
				printf("  OUT1 Frequency: %u Hz\n", status.frequency1);
				/* Antenna, outputs-enabled, power level, and mode are
				 * not reliably decoded on the Mini. Only show them for
				 * 1420/1421 where the feature-report bitmap matches. */
				if (model != LBE_MINI) {
					printf("  Antenna: %s\n", status.antenna_ok ? "OK" : "Short Circuit");
					printf("  Output(s) Enabled: %s\n", status.outputs_enabled ? "Yes" : "No");
					printf("  OUT1 Power Level: %s\n", status.out1_power_low ? "Low" : "Normal");
				}
				if (model == LBE_1421_DUALOUT) {
					printf("  OUT2 Frequency: %u Hz\n", status.frequency2);
					printf("  OUT2 Power Level: %s\n", status.out2_power_low ? "Low" : "Normal");

					printf("  1PPS on OUT1: %s\n", status.pps_enabled ? "Enabled" : "Disabled");
				}
				if (model != LBE_MINI) {
					printf("  Mode: %s\n", status.fll_enabled ? "FLL" : "PLL");
				}
			}
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			print_usage(model);
		}
	}

	if (!changed) {
		printf("No changes made\n");
	}

	lbe_close_device(dev);
	return 0;
}