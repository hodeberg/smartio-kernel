#! /bin/sh

# export gpio pins to userspace
for i in 28 27 204 205 236 237 14 165 212 213 214
do
  echo $i > /sys/class/gpio/export
done


# Set all buffers tristate
echo low > /sys/class/gpio/gpio214/direction

#Select i2c to shield instead of A signals
echo low > /sys/class/gpio/gpio204/direction
echo low > /sys/class/gpio/gpio205/direction

# Set I2C pins as inputs
echo in > /sys/class/gpio/gpio14/direction
echo in > /sys/class/gpio/gpio165/direction

#disable outputs
echo low > /sys/class/gpio/gpio236/direction
echo low > /sys/class/gpio/gpio237/direction

#disable pullup resistors
echo in > /sys/class/gpio/gpio212/direction
echo in > /sys/class/gpio/gpio213/direction

#Choose the I2C function on the ASIC pins
echo mode1 > /sys/kernel/debug/gpio_debug/gpio27/current_pinmux
echo mode1 > /sys/kernel/debug/gpio_debug/gpio28/current_pinmux

#enable buffers
echo high > /sys/class/gpio/gpio214/direction
