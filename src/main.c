#include "pico/stdlib.h"
#include "app.h"
#include "hw/keyboard.h"
#include "hw/lcd.h"
int main(void) {
    stdio_init_all();
    kbd_init();
    lcd_init();
    app_init();
    while (true) {
        app_poll();
        sleep_ms(1);
    }
}
