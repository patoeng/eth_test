# makefile, written by guido socher
# note atmega168 is no longer supported. Use version 1.0 for atmega168
MCU=atmega644
DUDECPUTYPE=m644p
#
# === Edit this and enter the correct device/com-port:
# linux (plug in the avrusb500 and type dmesg to see which device it is):
LOADCMD=avrdude -P /dev/ttyUSB0

# mac (plug in the programer and use ls /dev/tty.usbserial* to get the name):
#LOADCMD=avrdude -P /dev/tty.usbserial-A9006MOb

# windows (check which com-port you get when you plugin the avrusb500):
#LOADCMD=avrdude -P COM4

# All operating systems: if you have set the default_serial paramter
# in your avrdude.conf file correctly then you can just use this
# and you don't need the above -P option:
#LOADCMD=avrdude
# === end edit this
#
LOADARG=-p $(DUDECPUTYPE) -c usbasp -e -U flash:w:
#
CC=avr-gcc
OBJCOPY=avr-objcopy
# optimize for size:
CFLAGS=-g -mmcu=$(MCU) -Wall -Os -mcall-prologues
#-------------------
.PHONY: all main
#
all: main.hex 
	@echo "done"
#
main: main.hex
	@echo "done"
#
#-------------------
help: 
	@echo "Usage: make all|main.hex"
	@echo "or"
	@echo "make fuse|rdfuses"
	@echo "or"
	@echo "make load"
	@echo "or"
	@echo "Usage: make clean"
	@echo " "
	@echo "You have to set the low fuse byte to 0x60 on all new tuxgraphics boards".
	@echo "This can be done with the command (linux/mac if you use avrusb500): make fuse"
#-------------------
main.hex: main.elf 
	$(OBJCOPY) -R .eeprom -O ihex main.elf main.hex 
	avr-size main.elf
	@echo " "
	@echo "Expl.: data=initialized data, bss=uninitialized data, text=code"
	@echo " "

main.elf: main.o ip_arp_udp_tcp.o enc28j60.o websrv_help_functions.o lcd.o timeconversions.o dhcp_client.o
	$(CC) $(CFLAGS) -o main.elf -Wl,-Map,main.map main.o ip_arp_udp_tcp.o enc28j60.o websrv_help_functions.o lcd.o timeconversions.o dhcp_client.o
websrv_help_functions.o: websrv_help_functions.c websrv_help_functions.h ip_config.h 
	$(CC) $(CFLAGS) -Os -c websrv_help_functions.c
dhcp_client.o: dhcp_client.c  dhcp_client.h net.h enc28j60.h ip_arp_udp_tcp.h ip_config.h
	$(CC) $(CFLAGS) -Os -c dhcp_client.c
enc28j60.o: enc28j60.c timeout.h enc28j60.h
	$(CC) $(CFLAGS) -Os -c enc28j60.c
ip_arp_udp_tcp.o: ip_arp_udp_tcp.c net.h enc28j60.h ip_config.h
	$(CC) $(CFLAGS) -Os -c ip_arp_udp_tcp.c
main.o: main.c ip_arp_udp_tcp.h enc28j60.h timeout.h net.h websrv_help_functions.h ip_config.h lcd.h timeconversions.h lcd_hw.h
	$(CC) $(CFLAGS) -Os -c main.c
#
timeconversions.o : timeconversions.c timeconversions.h
	$(CC) $(CFLAGS) -Os -c timeconversions.c
lcd.o : lcd.c lcd.h lcd_hw.h
	$(CC) $(CFLAGS) -Os -c lcd.c
#------------------
#------------------
load: main.hex
	$(LOADCMD) $(LOADARG)main.hex
#
#-------------------
# Check this with make rdfuses
rdfuses:
	$(LOADCMD) -p $(DUDECPUTYPE) -c usbasp -v -q
#
fuse:
	@echo "warning: run this command only if you have an external clock on pin xtal1"
	@echo "The is the last chance to stop. Press crtl-C to abort"
	@sleep 2
	$(LOADCMD) -p  $(DUDECPUTYPE) -c usbasp -u -v -U lfuse:w:0x60:m
#-------------------
clean:
	rm -f *.o *.map *.elf  main.hex 
#-------------------
