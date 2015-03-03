/*********************************************
 * vim:sw=8:ts=8:si:et
 * To use the above modeline in vim you must have "set modeline" in your .vimrc
 * Author: Guido Socher
 * Copyright: GPL V2
 * See http://www.gnu.org/licenses/gpl.html
 *
 * Chip type           : Atmega168 or Atmega328 with ENC28J60
 *********************************************/
#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include "ip_arp_udp_tcp.h"
#include "websrv_help_functions.h"
#include "enc28j60.h"
#include "dhcp_client.h"
#include "timeout.h"
#include "net.h"
#include "lcd.h"
#include "timeconversions.h"

// Note: This software implements a NTP clock with LCD display and 
// built-in web server.
// The web server is at "myip" and the NTP client tries to access "ntpip".
// 
// Please modify the following lines. mac and ip have to be unique
// in your local area network. You can not have the same numbers in
// two devices. Just increase MACSTEP for every new device:
#ifndef MACSTEP
#define MACSTEP 0x28
#endif
static uint8_t mymac[6] = {0x54,0x55,0x58,0x10,0x50,MACSTEP};
// how did I get the mac addr? Translate the first 3 numbers into ascii is: TUX
//
// --------------- normally you don't change anything below this line
// change summer/winter time and your timezone here (GMT +1 is Germany, France etc... in winter), unit is hours times 10. The value here is just the
// compile time default and you can change it at run-time just by
// pressing the push button on the board.
//
//int8_t hours_offset_to_utc=+10;  // +10 means +1.0 hours = +1 hour
//
// US/Canada eastern time in summer (-4 hours):
int16_t hours_offset_to_utc=-40;
//
// listen port for tcp/www:
#define MYWWWPORT 80
//
// time.apple.com (any of 17.171.4.15 17.151.16.14 17.171.4.14 17.171.4.13 17.171.4.35)
static uint8_t ntpip[4] = {17,171,4,15};
// timeserver.rwth-aachen.de
//static uint8_t ntpip[4] = {134,130,4,17};
// time.nrc.ca
//static uint8_t ntpip[4] = {132,246,11,229};
//
// list of other servers:
// tick.utoronto.ca 128.100.56.135
// ntp.nasa.gov 198.123.30.132
// timekeeper.isi.edu 128.9.176.30
// ptbtime1.ptb.de 192.53.103.108
//
// -------- never change anything below this line ---------
// My own IP (DHCP will provide a value for it):
static uint8_t myip[4]={0,0,0,0};
// Default gateway (DHCP will provide a value for it):
static uint8_t gwip[4]={0,0,0,0};
#define TRANS_NUM_GWMAC 1
static uint8_t ntproutingmac[6]; // the gwmac in case ntp behind gw
// Netmask (DHCP will provide a value for it):
static uint8_t netmask[4];
//
static uint8_t init_state=0; // an initialisation state for various net work parameters that we need to get
// global string buffer
#define STR_BUFFER_SIZE 19
static char gStrbuf[STR_BUFFER_SIZE+1];
static uint16_t dat_p;

static uint8_t ntpclientportL=MACSTEP; // lower 8 bytes of local port number, use last digit of mac to get different starting numbers for different clocks
static uint8_t prev_minutes=99; // inititlaize to something that does not exist
static uint8_t haveNTPanswer=0; // 0=never sent an ntp req, 1=have time, 2=reqest sent no answer yet
static uint32_t sec_without_sync=0; // how many seconds we are running without sync; this is to adjust the local clock oscillator if needed
static uint8_t ntp_retry_count=0;
static uint8_t update_at_57_avoid_duplicates=0;
// this is were we keep time (in unix gmtime format):
// Note: this value may jump a few seconds when a new ntp answer comes.
// You need to keep this in mid if you build an alarm clock. Do not match
// on "==" use ">=" and set a state that indicates if you already triggered the alarm.
static uint32_t time=0;
static volatile uint8_t lcd_update_sec=0; // sec since LCD last updated
static volatile uint8_t sec=0;
static volatile uint8_t delay_sec=0;
static uint8_t display_24hclock=1;
static uint8_t display_utcoffset=1;
// eth/ip buffer:
#define BUFFER_SIZE 650
static uint8_t buf[BUFFER_SIZE+1];

// set output to VCC, red LED off
#define LEDOFF PORTB|=(1<<PORTB1)
// set output to GND, red LED on
#define LEDON PORTB&=~(1<<PORTB1)
// to test the state of the LED
#define LEDISOFF PORTB&(1<<PORTB1)
// 
static char password[10]="secret"; // must be a-z and 0-9, will be cut to 8 char
//
uint8_t verify_password(char *str)
{
        // a simple password/cookie:
        if (strncmp(password,str,sizeof(password))==0){ // the len=sizeof(password) takes only effect if both strings are un-terminated and longer than password
                return(1);
        }
        return(0);
}

// Convert an integer which is representing a float into a string.
// Our display is always 5 digits long (including one
// decimal point position). The first char is + or -. There is
// one decimal point.
// The resulting string is fixed width and padded with leading space.
//
// The integer should not be larger than 999.
// The integer must be a positive number.
static void offset_to_dispstr(int16_t inum,char *outbuf){
        int8_t i,j;
        char s='+';
        char chbuf[8];
        if (inum<0){
                s='-';
                inum=inum*-1;
        }
        itoa(inum,chbuf,10); // convert integer to string
        i=strlen(chbuf);
        if (i>3) i=3; //overflow protection
        strcpy(outbuf,"  0.0");
        j=5;
        while(i){
                outbuf[j-1]=chbuf[i-1];
                i--;
                j--;
                if (j==4){
                        // jump over the pre-set dot
                        j--;
                }
        }
        if (j>1){
                outbuf[1]=s;
        }else{
                outbuf[0]=s;
        }
}

// delete any decimal point. If no decimal point is available then add a zero at the 
// end.
void deldot(char *s)
{
        char c;
        char *dst;
        uint8_t founddot=0;
        dst=s;
        while ((c = *s)) {
                if (c == '.'||c == ','){ // dot or comma
                        s++;
                        founddot=1;
                        continue;
                }
                *dst = c;
                dst++;
                s++;
        }
        if (founddot==0){
                *dst = '0';
                dst++;
        }
        *dst = '\0';
}


uint16_t http200ok(void)
{
        return(fill_tcp_data_p(buf,0,PSTR("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nPragma: no-cache\r\n\r\n")));
}

uint16_t http200okjs(void)
{
        return(fill_tcp_data_p(buf,0,PSTR("HTTP/1.0 200 OK\r\nContent-Type: application/x-javascript\r\n\r\n")));
}

// t1.js
uint16_t print_t1js(void)
{
        uint16_t plen;
        plen=http200okjs();
        // show this computers TZ offset
        plen=fill_tcp_data_p(buf,plen,PSTR("\
function tzi(){\n\
var t = new Date();\n\
var tzo=t.getTimezoneOffset()/60;\n\
var st;\n\
if (tzo>0) st=\"GMT-\"+tzo; else st=\"GMT+\"+tzo;\n\
document.write(\" [Info: your PC is \"+st+\"]\");\n\
}\n\
"));
        return(plen);
}

uint16_t print_webpage_ok(void)
{
        return(fill_tcp_data_p(buf,http200ok(),PSTR("<a href=/>OK</a>. Please power-cycle after IP-addr. change")));
}

uint8_t get_netmask_length(uint8_t *mask){
        uint8_t i,j;
        uint8_t l=0;
        i=0;
        while(i<4){
                j=0;
                while(j<8){
                        if (mask[i] & (1<<j)){
                                l++;
                        }else{
                                j=7;i=3;
                        }
                        j++;
                }
                i++;
        }
        return(l);
}

// prepare the webpage by writing the data to the tcp send buffer
// Note that this is about as much as you can put on a html page.
// If you plan to build additional functions into this clock (e.g alarm clock) then
// add a new page under /alarm and do not expand this /config page.
uint16_t print_webpage_config(void)
{
        uint16_t plen;
        plen=http200ok();
        plen=fill_tcp_data_p(buf,plen,PSTR("<script src=t1.js></script>"));
        plen=fill_tcp_data_p(buf,plen,PSTR("<h2>NTP config</h2><pre>MAC addr: "));
        mk_net_str(gStrbuf,mymac,6,':',16);
        plen=fill_tcp_data(buf,plen,gStrbuf);
        plen=fill_tcp_data_p(buf,plen,PSTR("\n<form action=/cu method=get>Clock IP: "));
        mk_net_str(gStrbuf,myip,4,'.',10);
        plen=fill_tcp_data(buf,plen,gStrbuf);
        // show length of the network:
        plen=fill_tcp_data(buf,plen,"/");
        itoa(get_netmask_length(netmask),gStrbuf,10); // convert integer to string
        plen=fill_tcp_data(buf,plen,gStrbuf);
        plen=fill_tcp_data_p(buf,plen,PSTR("\nshow: <input type=checkbox name=hh"));
        if (display_24hclock){
                plen=fill_tcp_data_p(buf,plen,PSTR(" checked"));
        }
        plen=fill_tcp_data_p(buf,plen,PSTR(">24h <input type=checkbox name=uo"));
        if (display_utcoffset){
                plen=fill_tcp_data_p(buf,plen,PSTR(" checked"));
        }
        plen=fill_tcp_data_p(buf,plen,PSTR(">GMT offset\n"));
        plen=fill_tcp_data_p(buf,plen,PSTR("NTP srv IP:<input type=text name=ns value="));
        mk_net_str(gStrbuf,ntpip,4,'.',10);
        plen=fill_tcp_data(buf,plen,gStrbuf);
        plen=fill_tcp_data_p(buf,plen,PSTR(">\nGMT offset:<input type=text name=tz value="));
        offset_to_dispstr(hours_offset_to_utc,gStrbuf);
        plen=fill_tcp_data(buf,plen,gStrbuf);
        plen=fill_tcp_data_p(buf,plen,PSTR("><script>tzi()</script>\n"));
        plen=fill_tcp_data_p(buf,plen,PSTR("\nNew pw:<input type=text name=np>\n\npw:    <input type=password name=pw><input type=submit value=apply></form>\n"));
        return(plen);
}

// prepare the main webpage by writing the data to the tcp send buffer
uint16_t print_webpage(void)
{
        uint16_t plen;
        char day[16];
        char clock[12];
        plen=http200ok();
        plen=fill_tcp_data_p(buf,plen,PSTR("<h2>NTP clock</h2><pre>\n"));
        if (haveNTPanswer){ // 1 or 2
                gmtime(time+(int32_t)+360*(int32_t)hours_offset_to_utc,display_24hclock,day,clock);
                plen=fill_tcp_data_p(buf,plen,PSTR("Date: "));
                plen=fill_tcp_data(buf,plen,day);
                plen=fill_tcp_data(buf,plen,"\nTime: ");
                plen=fill_tcp_data(buf,plen,clock);
                plen=fill_tcp_data_p(buf,plen,PSTR(" (GMT"));
                offset_to_dispstr(hours_offset_to_utc,gStrbuf);
                plen=fill_tcp_data(buf,plen,gStrbuf);
                plen=fill_tcp_data_p(buf,plen,PSTR(")\n"));
                if (haveNTPanswer==2){
                        plen=fill_tcp_data_p(buf,plen,PSTR("Status: Last ntp sync is older than 1 hour\n"));
                }else{
                        plen=fill_tcp_data_p(buf,plen,PSTR("Status: OK\n"));
                }
        }else{
                plen=fill_tcp_data_p(buf,plen,PSTR("Status: Waiting for ntp answer\n"));
        }
        plen=fill_tcp_data_p(buf,plen,PSTR("</pre><br>\n"));
        plen=fill_tcp_data_p(buf,plen,PSTR("<a href=\"./config\">[config]</a> <a href=\"./\">[refresh]</a>\n"));
        return(plen);
}

// analyse the url given
// return values: are used to show different pages (see main program)
//
int8_t analyse_get_url(char *str)
{
        uint8_t updateerr=0;
        int16_t i=0;
        // the first slash:
        if (str[0] == '/' && str[1] == ' '){
                // end of url, display just the web page
                return(0);
        }
        if (strncmp("/t1.js",str,6)==0){
                dat_p=print_t1js();
                return(10);
        }
        if (strncmp("/config",str,7)==0){
                dat_p=print_webpage_config();
                return(10);
        }
        if (strncmp("/cu?",str,4)==0){
                if (find_key_val(str,gStrbuf,STR_BUFFER_SIZE,"pw")){
                        urldecode(gStrbuf);
                        if (verify_password(gStrbuf)){
                                if (find_key_val(str,gStrbuf,STR_BUFFER_SIZE,"ns")){
                                        urldecode(gStrbuf);
                                        if (parse_ip(ntpip,gStrbuf)!=0){
                                                updateerr=1;
                                        }
                                }
                                display_24hclock=0;
                                if (find_key_val(str,gStrbuf,STR_BUFFER_SIZE,"hh")){
                                        display_24hclock=1;
                                }
                                display_utcoffset=0;
                                if (find_key_val(str,gStrbuf,STR_BUFFER_SIZE,"uo")){
                                        display_utcoffset=1;
                                }
                                if (find_key_val(str,gStrbuf,STR_BUFFER_SIZE,"tz")){
                                        urldecode(gStrbuf);
                                        deldot(gStrbuf);
                                        i=atoi(gStrbuf);
                                        if (i> -140 && i < 140){
                                                hours_offset_to_utc=i;
                                        }else{
                                                updateerr=1;
                                        }
                                }
                                if (find_key_val(str,gStrbuf,STR_BUFFER_SIZE,"np")){
                                        urldecode(gStrbuf);
                                        strncpy(password,gStrbuf,8);
                                        password[8]='\0';
                                }
                                // parse_ip does store also partially
                                // wrong values if it returns an error
                                // but it is still better to inform the user
                                if (updateerr){
                                        dat_p=fill_tcp_data_p(buf,http200ok(),PSTR("<a href=/config>Error</a>"));
                                        return(10);
                                }
                                // store in eeprom:
                                eeprom_write_byte((uint8_t *)0x100,19); // magic number
                                eeprom_write_block((uint8_t *)ntpip,(void *)0x111,sizeof(ntpip));
                                eeprom_write_block((char *)password,(void *)0x119,sizeof(password));
                                eeprom_write_byte((uint8_t *)0x130,display_utcoffset);
                                eeprom_write_byte((uint8_t *)0x131,display_24hclock);
                                // hours_offset_to_utc can be changed as well by button and has its own magic number:
                                eeprom_write_byte((uint8_t *)0x101,19); // magic number
                                // we store it as a positive number to make sure there are no signed/unsigned problems:
                                eeprom_write_word((uint16_t *)0x116,(300+hours_offset_to_utc));
                                prev_minutes=99; // refresh the full display
                                return(1);
                        }
                }
        }
        return(-1); // Unauthorized
}

void print_time_to_lcd(void)
{
        char day[16];
        char clock[12];
        uint8_t minutes;
        char lindicator=' '; // link indicator on the right, /=down, |=waiting for ntp answer

        // returns day and time-string in seperate variables:
        minutes=gmtime(time+(int32_t)+360*(int32_t)hours_offset_to_utc,display_24hclock,day,clock);
        if (prev_minutes!=minutes){
                // update complete display including day
              
        }
        // write first line
       
        if (display_utcoffset){
                offset_to_dispstr(hours_offset_to_utc,gStrbuf);
               
        }else{
                
        }
        // write to second line
       
        lindicator=' ';
        if (haveNTPanswer!=1){
                // request sent no answer yet
                lindicator='|';
        }
        if (enc28j60linkup()==0){
                // link down
                lindicator='/';
        }
     
        prev_minutes=minutes;
        // before every hour
        if (minutes==55 && update_at_57_avoid_duplicates==0){  
                update_at_57_avoid_duplicates=1;
        }
        if (minutes==57 && update_at_57_avoid_duplicates==1){  
                // mark that we will wait for new
                // ntp update
                haveNTPanswer=2;
                ntp_retry_count=0;
                update_at_57_avoid_duplicates=0;
        }
}

// interrupt, step seconds counter
ISR(TIMER1_COMPA_vect){
        lcd_update_sec++;
        if (delay_sec) delay_sec--;
        sec++;
        if (sec>5){
                sec=0;
                dhcp_6sec_tick();
        }
        if (LEDISOFF){
                LEDON;
        }else{
                LEDOFF;
        }
}

// Generate a 1s clock signal as interrupt form the 12.5MHz system clock
// Since we have that 1024 prescaler we do not really generate  second
// but 1.00000256000655361677s. In other words we should add one second
// to our counter every 108 hours if we want a more accurate clock. 
// That is however not really needed as we update via ntp every hour. 
void timer_init(void)
{
        /* write high byte first for 16 bit register access: */
        TCNT1H=0;  /* set counter to zero*/
        TCNT1L=0;
        // Mode 4 table 14-4 page 132. CTC mode and top in OCR1A
        // WGM13=0, WGM12=1, WGM11=0, WGM10=0
        TCCR1A=(0<<COM1B1)|(0<<COM1B0)|(0<<WGM11);
        TCCR1B=(1<<CS12)|(1<<CS10)|(1<<WGM12)|(0<<WGM13); // crystal clock/1024

        // divide crystal clock:
        // At what value to cause interrupt. You can use this for calibration
        // of the clock. Theoretical value for 12.5MHz: 12207=0x2f and 0xaf
        // Since we count from zeor we have to subtract one: 0x2f and 0xae
        OCR1AH=0x2f;
        OCR1AL=0xae;
        // interrupt mask bit:
        TIMSK1 = (1 << OCIE1A);
}

// the __attribute__((unused)) is a gcc compiler directive to avoid warnings about unsed variables.
void arpresolver_result_callback(uint8_t *ip __attribute__((unused)),uint8_t reference_number,uint8_t *mac){
        uint8_t i=0;
        if (reference_number==TRANS_NUM_GWMAC){
                // copy mac address over:
                while(i<6){ntproutingmac[i]=mac[i];i++;}
                init_state=3;
        }
}

int main(void){
        int8_t i;
        uint16_t plen=0;
        uint8_t offset_utc_changed=0;
        uint16_t button_debounce=0;
        // set the clock speed to "no pre-scaler" (8MHz with internal osc or 
        // full external speed)
        // set the clock prescaler. First write CLKPCE to enable setting of clock the
        // next four instructions.
        CLKPR=(1<<CLKPCE); // change enable
        CLKPR=0; // "no pre-scaler"
        _delay_loop_1(0); // 60us

        // lcd display:
       
		DDRB|=(1<<DDB2);
        PORTB &=~(1<<PORTB2);
        // enable PD1, as input, push button
        DDRD&= ~(1<<DDD1);
        PORTD|= (1<<PIND1); // internal pullup resistor on, jumper goes to ground
		
		
        _delay_loop_2(60000); // 60us
		_delay_loop_2(60000); // 60us
        _delay_loop_2(60000); // 60us
		_delay_loop_2(60000); // 60us
        _delay_loop_2(60000); // 60us
		_delay_loop_2(60000); // 60us
        _delay_loop_2(60000); // 60us
		 
        PORTB |= (1<<PORTB2);// enable PB0, as input, config reset
        /*initialize enc28j60*/
        enc28j60Init(mymac);
        enc28j60clkout(2); // change clkout from 6.25MHz to 12.5MHz
        _delay_loop_1(0); // 60us
        
        /* Magjack leds configuration, see enc28j60 datasheet, page 11 */
        // LEDB=yellow LEDA=green
        //
        // 0x476 is PHLCON LEDA=links status, LEDB=receive/transmit
        // enc28j60PhyWrite(PHLCON,0b0000 0100 0111 01 10);
        enc28j60PhyWrite(PHLCON,0x476);
        
        DDRB|= (1<<DDB1); // LED, enable PB1, LED as output
        LEDON;
        // read eeprom values unless the button is pressed at startup:
        if (bit_is_clear(PIND,PIND1)){
                eeprom_write_byte((uint8_t *)0x100,0xf); // del magic number
                eeprom_write_byte((uint8_t *)0x101,0xf); // del magic number
        }else{
                if (eeprom_read_byte((uint8_t *)0x100) == 19){
                        // ok magic number matches accept values
                        eeprom_read_block((uint8_t *)ntpip,(void *)0x111,sizeof(ntpip));
                        eeprom_read_block((char *)password,(void *)0x119,sizeof(password));
                        password[8]='\0'; // make sure it is terminated, should not be necessary
                        display_utcoffset=(int8_t)eeprom_read_byte((uint8_t *)0x130);
                        display_24hclock=(int8_t)eeprom_read_byte((uint8_t *)0x131);
                }
                if (eeprom_read_byte((uint8_t *)0x101) == 19){
                        hours_offset_to_utc=eeprom_read_word((uint16_t *)0x116);
                        // we store it as a positive number to make sure there are no signed/unsigned problems:
                        hours_offset_to_utc-=300;
                }
        }
      
        while (enc28j60linkup()==0){
                // wait a bit then try again:
                plen=0xFFF;
                while(plen){
                        _delay_loop_1(0); // 60us
                        plen--;
                }
        }
        // we need the timer for the dhcp-resend tick:
        timer_init(); 
        sei(); // interrupt on, clock starts ticking now
      
        // DHCP handling. Get the initial IP
        i=0;
        init_mac(mymac);
        while(i==0){
                plen=enc28j60PacketReceive(BUFFER_SIZE, buf);
                buf[BUFFER_SIZE]='\0';
                i=packetloop_dhcp_initial_ip_assignment(buf,plen,mymac[5]);
        }
        // we have an IP:
        dhcp_get_my_ip(myip,netmask,gwip); 
        client_ifconfig(myip,netmask); // we do not need to call init_udp_or_www_server if the client is already configured and we can use the server functions now.
       
        mk_net_str(gStrbuf,myip,4,'.',10);
        
        delay_sec=5; //we use this to change the display only in 5 sec from now
        // web server listen port:
        www_server_port(MYWWWPORT);

        //
        while(1){
                plen=enc28j60PacketReceive(BUFFER_SIZE, buf);
                buf[BUFFER_SIZE]='\0'; // http is an ascii protocol. Make sure we have a string terminator.
                // DHCP renew IP:
                plen=packetloop_dhcp_renewhandler(buf,plen); // for this to work you have to call dhcp_6sec_tick() every 6 sec
                dat_p=packetloop_arp_icmp_tcp(buf,plen);
                if(dat_p==0){
                        // no http request
                        if (plen>0){
                                // possibly a udp message
                                goto UDP;
                        }
                        // we are idle here (no incomming packet to process).
                        if (button_debounce) button_debounce--;
                        // we store the changed hours_offset_to_utc only
                        // a few seconds after the user stopped pressing 
                        // buttons:
                        if (offset_utc_changed==1 && delay_sec==0){
                                offset_utc_changed=0;
                                // hours_offset_to_utc has its own magic number:
                                eeprom_write_byte((uint8_t *)0x101,19); // magic number
                                eeprom_write_word((uint16_t *)0x116,(300+hours_offset_to_utc));
                        }
                        if (init_state==0 && delay_sec==0){
                                // this is just a wait state to wait
                                // until sec==0 and show my ip in the mean time 
                                init_state=1;
                                delay_sec=0;
                        }
                        if (init_state==1 && delay_sec==0){
                                delay_sec=8; // retry after 8 sec if no asnwer
                                if (route_via_gw(ntpip)){
                                        // ntpip is behind the GW
                                        // find the mac address of the gateway (e.g your dsl router).
                                        get_mac_with_arp(gwip,TRANS_NUM_GWMAC,&arpresolver_result_callback);
                                      
                                }else{
                                        // the ntp server we want to contact is on the local lan. find the mac:

                                        get_mac_with_arp(ntpip,TRANS_NUM_GWMAC,&arpresolver_result_callback);
                                    
                                }
                        }
                        // init_state==2 is not used
                        if (init_state==3){
                               
                                ntpclientportL++; // new src port
                                client_ntp_request(buf,ntpip,ntpclientportL,ntproutingmac);
                                delay_sec=15; // retry after 15 sec if no asnwer
                                haveNTPanswer=0;
                                init_state=4;
                        }
                        if (init_state==4){
                                // update the LCD
                                if (haveNTPanswer && lcd_update_sec){
                                        sec_without_sync+=lcd_update_sec;
                                        time+=lcd_update_sec;
                                        // we compensate the clock a bit
                                        // if we are running for a long
                                        // time without sync. We need to
                                        // add one sec about every 108 hours
                                        // since we are running slower:
                                        if (sec_without_sync>390624){
                                                time++;
                                                sec_without_sync=0;
                                        }
                                        lcd_update_sec=0;
                                        print_time_to_lcd();
                                        continue;
                                }
                                if (button_debounce==0 && bit_is_clear(PIND,PIND1)){
                                        button_debounce=0x2eff;
                                        hours_offset_to_utc+=10;
                                        // http://en.wikipedia.org/wiki/List_of_UTC_time_offset
                                        // there is utc -12 to utc +14
                                        if (hours_offset_to_utc> 140){
                                                hours_offset_to_utc-=270;
                                        }
                                        offset_utc_changed=1;
                                        delay_sec=4;
                                        print_time_to_lcd();
                                        continue;
                                }
                                // retry at initial time assignment:
                                if (haveNTPanswer==0 && delay_sec==0){
                                        ntpclientportL++; // new src port
                                        client_ntp_request(buf,ntpip,ntpclientportL,ntproutingmac);
                                        delay_sec=30; // retry after 30 sec if no asnwer
                                }
                                // retry while the clock is already up and running:
                                if (haveNTPanswer==2 && delay_sec==0){
                                        if (ntp_retry_count<2){
                                                ntpclientportL++; // new src port
                                                client_ntp_request(buf,ntpip,ntpclientportL,ntproutingmac);
                                                ntp_retry_count++;
                                        }else{
                                                ntp_retry_count=0;
                                                if (route_via_gw(ntpip)){
                                                        // ntpip is behind the GW
                                                        // find the mac address of the gateway (e.g your dsl router).
                                                        get_mac_with_arp(gwip,TRANS_NUM_GWMAC,&arpresolver_result_callback);
                                                }else{
                                                        // the ntp server we want to contact is on the local lan. find the mac:

                                                        get_mac_with_arp(ntpip,TRANS_NUM_GWMAC,&arpresolver_result_callback);
                                                }
                                        }
                                        delay_sec=60; // retry after 60 sec if no asnwer
                                }
                        }
                        continue;
                }
                // dat_p !=0
                // tcp port 80 begin
                if (strncmp("GET ",(char *)&(buf[dat_p]),4)!=0){
                        // head, post and other methods:
                        dat_p=http200ok();
                        dat_p=fill_tcp_data_p(buf,dat_p,PSTR("<h1>200 OK</h1>"));
                        goto SENDTCP;
                }
                i=analyse_get_url((char *)&(buf[dat_p+4]));
                // for possible status codes see:
                // http://www.w3.org/Protocols/rfc2616/rfc2616-sec10.html
                if (i==-1){
                        dat_p=fill_tcp_data_p(buf,0,PSTR("HTTP/1.0 401 Unauthorized\r\nContent-Type: text/html\r\n\r\n<h1>401 Unauthorized</h1>"));
                        goto SENDTCP;
                }
                if (i==1){
                        dat_p=print_webpage_ok();
                        goto SENDTCP;
                }
                if (i==10){
                        goto SENDTCP;
                }
                dat_p=print_webpage();
SENDTCP:
                www_server_reply(buf,dat_p); // send web page data
                continue;
                // tcp port 80 end
UDP:
                // UDP/NTP protocol handling and idel tasks
                // check if ip packets are for us:
                if(eth_type_is_ip_and_my_ip(buf,plen)){
                        if (client_ntp_process_answer(buf,&time,ntpclientportL)){
                                sec_without_sync=0;
                                lcd_update_sec=0;
                                // convert to unix time:
                                if ((time & 0x80000000UL) ==0){
                                        // 7-Feb-2036 @ 06:28:16 UTC it will wrap:
                                        time+=2085978496;
                                }else{
                                        // from now until 2036:
                                        time-=GETTIMEOFDAY_TO_NTP_OFFSET;
                                }
                                haveNTPanswer=1;
                                ntp_retry_count=0;
                        }
                }
        }
        return (0);
}
