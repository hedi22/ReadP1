/*
 _______ _________ _        _        _______  _______ 
(  ____ \\__   __/( (    /|| \    /\(  ____ \(  ___  )
| (    \/   ) (   |  \  ( ||  \  / /| (    \/| (   ) |
| |         | |   |   \ | ||  (_/ / | |      | |   | |
| | ____    | |   | (\ \) ||   _ (  | | ____ | |   | |
| | \_  )   | |   | | \   ||  ( \ \ | | \_  )| |   | |
| (___) |___) (___| )  \  ||  /  \ \| (___) || (___) |
(_______)\_______/|/    )_)|_/    \/(_______)(_______)

 _____                    __        __   _       ____  _ 
| ____|_ __ ___   ___  _ _\ \      / /__| |__   |  _ \/ |
|  _| | '_ ` _ \ / _ \| '_ \ \ /\ / / _ \ '_ \  | |_) | |
| |___| | | | | | (_) | | | \ V  V /  __/ |_) | |  __/| |
|_____|_| |_| |_|\___/|_| |_|\_/\_/ \___|_.__/  |_|   |_|

Reading P1 Companion Standard of Dutch Smart Meter through external RX with an Arduino.
You can't use SoftwareSerial because you need to change the default settings of the USART like partity and stop bits.
EN_TX should be enabled during reading of data, but disabled if you want to upload your sketch!
You need to convert the data signal with an 7404.

Based on emonBase - Documentation: http://openenergymonitor.org/emon/emonbase

Original authors: Trystan Lea and Glyn Hudson
Adjusted for reading P1 port and posting to emonWeb.org: Frank Oxener
Part of the: openenergymonitor.org project
Licenced under GNU GPL V3
http://openenergymonitor.org/emon/license

EtherCard Library by Jean-Claude Wippler and Andrew Lindsay
*/

// #define DEBUG     //comment out to disable serial printing to increase long term stability 
#define UNO       //anti crash wachdog reset only works with Uno (optiboot) bootloader, comment out the line if using delianuova
#define START_PARAMETERS  "auth_token=qqbUzX2V8UKUDwxZyBhR&P1="

#include <avr/wdt.h>
#include <EtherCard.h>  //https://github.com/jcw/ethercard
#include <SRAM9.h>

// ethernet interface mac address, must be unique on the LAN
byte mymac[6] = { 0x00,0x04,0xA3,0x21,0xC8,0x56};
byte sd;
byte free_stash_memory;

byte Ethernet::buffer[700];

char id_token[] PROGMEM = "qqbUzX2V8UKUDwxZyBhR";

//Domain name of remote webserver - leave blank if posting to IP address 

char website[] PROGMEM = "emonweb.org";
// static byte hisip[] = { 192, 168, 2, 211};    // un-comment for posting to static IP server (no domain name) 

const int redLED = A1;
const int requestPin =  6;

int ethernet_error = 0;                   // Etherent (controller/DHCP) error flag
int rf_error = 0;                         // RF error flag - high when no data received 
int ethernet_requests = 0;                // count ethernet requests without reply

int dhcp_status = 0;
int dns_status = 0;

int serial_input = 0;

int data_ready = 0;                       // Used to signal that emontx data is ready to be sent
int reset = 0;
int nof_times = 0;

static byte session;
String stringReply;

boolean start_p1_record;
Stash stash;                              //Use the RAM inside the ENC28J60 Ethernet controller to post data from P1 port
                                          //See http://jeelabs.org/2012/04/11/ethercard-improvements/

//**********************************************************************************************************************
// SETUP
//**********************************************************************************************************************
void setup () {

  pinMode(redLED, OUTPUT); digitalWrite(redLED,LOW);
  delay(100); digitalWrite(redLED,HIGH);                      // turn off redLED

  Serial.begin(9600);
  UCSR0C = (2<<UPM00)|(0<<USBS0)|(2<<UCSZ00)|(0<<UCPOL0);     // 7-E-1 needed for reading P1

  Serial.println("\n[webClient]");

  if (ether.begin(sizeof Ethernet::buffer, mymac) == 0) {
    Serial.println( "Failed to access Ethernet controller");
    ethernet_error = 1;
  }

  dhcp_status = 0;
  dns_status = 0;
  reset = 1;

  #ifdef UNO
    wdt_enable(WDTO_8S);
  #endif;

  pinMode(requestPin, OUTPUT);
  digitalWrite(requestPin, HIGH);

}

//**********************************************************************************************************************
// LOOP
//**********************************************************************************************************************
void loop () {

  stringReply = "";

 //-----------------------------------------------------------------------------------------------------------------
  // Receive response from server
  //-----------------------------------------------------------------------------------------------------------------

  const char* reply = ether.tcpReply(session);
  if (reply != 0){
    int size = strlen(reply);
    #ifdef DEBUG
      Serial.println(size);
      Serial.println(">>>> RESPONSE RECEIVED -----");
      Serial.println(reply);
    #endif
  }

  dhcp_dns();  // handle dhcp and dns setup - see dhcp_dns tab

  // Display error states on status LED
  if (ethernet_error == 1 || ethernet_requests > 0) digitalWrite(redLED,LOW);
    else digitalWrite(redLED,HIGH);

  #ifdef UNO
    wdt_reset();
  #endif

  //-----------------------------------------------------------------------------------------------------------------
  // Receive data from P1
  //-----------------------------------------------------------------------------------------------------------------
  
  serial_input = 0;

  while (Serial.available() > 0) {

    digitalWrite(redLED, LOW);
    char inChar = (char)Serial.read();

    serial_input += 1;

    #ifdef DEBUG
      Serial.write(inChar);
    #endif

    if (inChar == '/') {
      start_p1_record = true;
    }

    if (start_p1_record == true) {
      stash.print(inChar);
    }

    if (inChar == '!') {
      stash.save();
      digitalWrite(redLED, HIGH);
      data_ready = 1;
      start_p1_record = false;
    }

    if (serial_input > 800) {
      delay(10000); //Unable to determine the end of P1 datagram
    }

    free_stash_memory = Stash::freeCount();
    if (free_stash_memory < 20){
      delay(10000);
    }

  }

  //-----------------------------------------------------------------------------------------------------------------
  // Send data via ethernet
  //-----------------------------------------------------------------------------------------------------------------

  ether.packetLoop(ether.packetReceive());

  if (data_ready) {

    nof_times += 1; 

    //Connection needs time to build up, so no post RST on the first few times
    if (reset == 1 && nof_times > 2) {
      #ifdef DEBUG
        Serial.print("Reset true!!!!");
      #endif
      stash.print("&RST=1&REA=UNK");
      reset = 0;
    }

    stash.save();

    Stash::prepare(PSTR("POST http://$F/p1 HTTP/1.0" "\r\n"
                        "Host: $F" "\r\n"
                        "User-Agent: Ginkgo P1 reader 0.3" "\r\n"
                        "Content-Length: $D" "\r\n"
                        "ID_token: $F" "\r\n"
                        "\r\n"
                        "$H"),
                        website, website, stash.size(), id_token, sd);

    // send the packet - this also releases all stash buffers once done

    session = ether.tcpSend();

    #ifdef DEBUG
      Serial.println("=SESSION");
      Serial.println(session);
    #endif

    free_stash_memory = Stash::freeCount();

    #ifdef DEBUG
      Serial.println();
      Serial.print("Number of bytes free in Stash memory: ");
      Serial.println(free_stash_memory, DEC);
      Serial.println();
      Serial.println();
    #endif

    //No response from server consumes memory from the Stash,
    //so if no memory left, reset the arduino (delay is longer than WDTO_8S)
    if (free_stash_memory == 0){
      storeState(0x03);
      delay(10000);
    }

    data_ready =0;

    sd = stash.create();
    stash.print(START_PARAMETERS);
  }

}

void storeState (byte state){
  SRAM9.writestream(0);
  SRAM9.RWdata(state);
  SRAM9.closeRWstream();
}
