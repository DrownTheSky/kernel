#ifndef __OLED_COMMON_H__
#define __OLED_COMMON_H__

#include <linux/gpio/consumer.h>

#define OLED_WIDTH  128
#define OLED_HIGH   64

typedef struct oled {
	int major;
	struct class *class;
	struct gpio_descs *gpiod;
} oled_t;

struct oled_operation {
    void (*init)(void);
    void (*display)(uint8_t);
    void (*clear)(void);
    void (*str)(uint8_t, uint8_t, uint8_t, const char *);
};

struct oled_operation *get_oled_operation(void);

#endif /* __OLED_COMMON_H__ */