#! /bin/sh

# export gpio pins to userspace
for i in 13 253 221 214
do
  echo $i > /sys/class/gpio/export
done


# Set all buffers tristate
echo low > /sys/class/gpio/gpio214/direction


# Set level shifter as input
echo low > /sys/class/gpio/gpio253/direction

# Disable pullup
#echo in > /sys/class/gpio/gpio221/direction

# Set alert pin as inputs
echo in > /sys/class/gpio/gpio13/direction


#Choose the GPIO function on the ASIC pins
#echo mode0 > /sys/kernel/debug/gpio_debug/gpio13/current_pinmux

#enable buffers
echo high > /sys/class/gpio/gpio214/direction
