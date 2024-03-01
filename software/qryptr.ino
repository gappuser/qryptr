//TODOS
//better prompts
//on screen arrows to indicate back/forth
//voltage readings of battery
//write special characters with control button
//multiple IDs
//use camera without character encoding. Currently, the GM-803 will always apply character encoding (UTF-8) to any byte. This means we need to apply base64 encoding before encoding into a QR code.
//Section 7.8 of the GM-803 manual seems to suggest there is a way to turn off character encoding, but this is not clear yet. We'd prefer to send raw bytes instead of base64-encoded raw bytes because it would allow for longer messages
//add option for symmetric encryption of private key (password prompt to make sure the private key is not kept in plain form on the memory chip)
//add <start> and <end> tags for messages and keys, so that upon decryption, it is clear whether a valid message was received or whether a error should be displayed


//Pin assignment
//Keyboard
//PICO_GP2_ROW1                                   
//PICO_GP3_ROW2  
//PICO_GP4_ROW3  
//PICO_GP5_ROW4  
//PICO_GP6_ROW5
//PICO_GP7_COL1
//PICO_GP8_COL2
//PICO_GP9_COL3
//PICO_GP10_COL4
//PICO_GP11_COL5
//PICO_GP12_COL6
//PICO_GP13_COL7
//PICO_GP14_COL8
//PICO_GP15_COL9
//PICO_GP16_COL10
//Display
//PICO_GP17_DISP_CS_3 
//PICO_GP18_DISP_SCL_1 
//PICO_GP19_DISP_MOSI_2 
//GP_20 GPIOSHUTDOWN        (todo for soft shutdown/timeout shutdown)
//PICO_GP21_SHIFT
//PICO_GP22_CTRL
//GP_23 unused
//GP_24 unused
//GP25 PI_LED for testing purposes
//GP28 ADC2 random input for ring oscillator (rosc)
//GP29 ADC3 10kOhm to battery for voltage indicator. Needs a resistor voltage divider still.


//CHARACTER ENCODING
//GM803 will always apply a character encoding, will not send pure bytes from QR code. So ecc keys need to be encoded with base64 (33% overhead).
//the terminal character is not an alphanumeric character. which causes ricmoo/qrcode to encode as BYTES, with all limitations that come along with that..
//strlen(), which is used by ricmoo/qrcode, only works if a null terminator is present.
//but that null terminator will be interpreted as a byte, which causes ricmoo/qrcode to never switch to alphanumeric mode..
//so, you have to use base64 encoding.


//FLASH WRITE STUFF
//always need to erase first.
//erase minimum is 4096 bytes
//write is 256 bytes minium. But can you only program bytes from a certain sector that was erased.
//have one fixed location for pub and private key (sector -2 and -3 before sector -200)
//https://www.makermatrix.com/blog/read-and-write-data-with-the-pi-pico-onboard-flash/
//we save contacts on individual sectors, 200 sectors from the end of the available memory. So we can have 200 contacts.

//Cryptographic procedures follow this structure:
//https://crypto.stackexchange.com/questions/101420/eccdh-direct-or-with-temporary-ecc-keypairs

extern "C" {
  #include <hardware/sync.h>
  #include <hardware/flash.h>
};

#include <Arduino.h>
#include <U8g2lib.h>
#include <qrcode.h>
#include <SPI.h>
#include <types.h>
#include <stdio.h>
#include <stdlib.h>
#include <gpio.h>
#include <uart.h>
#include <string.h>


//Cryptography libraries from Rhys Weatherly and Brandon Wiley
//https://www.arduino.cc/reference/en/libraries/crypto/
#include <Crypto.h>
#include <Curve25519.h>
#include <RNG.h>
#include <SHA256.h>
#include <HKDF.h>
#include <hardware/structs/rosc.h>
#include <ChaChaPoly.h>
#include <Base64.h>


#define MAX_PLAINTEXT_LEN 332
#define FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

//old 128x64 st7920 lcd, someone may still want to try this as an alternative display
//U8G2_ST7920_128X64_F_SW_SPI u8g2(U8G2_R0, /* clock=*/ 13, /* data=*/ 11, /* CS=*/ 10, /* reset=*/ 8);

//Works only with the official rp2040 mbed arduino core, not with the earle philhower core, because you cannot change the HW SPI pin assignment. And the original pin assignment is very slow.
//u8g2 sendBuffer takes about 196ms, found no room for improvement.
U8G2_LS027B7DH01_400X240_F_4W_HW_SPI u8g2(U8G2_R0, 17, U8X8_PIN_NONE);


//UART variables, for use with the GM-803 camera
#define UART0_ID        uart0
#define UART0_BAUD_RATE 9600
#define DATA_BITS 8
#define STOP_BITS 1
#define PARITY    UART_PARITY_NONE

//GPIO pins 12, 14, 15 are hardware SPI pins based on mbed2040 library (other rp2040 arduino implementation could only use software SPI pins for display initialization)
int pinrow1 = 2;
int pinrow2 = 3;
int pinrow3 = 4;
int pinrow4 = 5;
int pinrow5 = 6;
int pincolumn1 = 7;
int pincolumn2 = 8;
int pincolumn3 = 9;
int pincolumn4 = 10;
int pincolumn5 = 11;
int pincolumn6 = 12;
int pincolumn7 = 13;
int pincolumn8 = 14;
int pincolumn9 = 15;
int pincolumn10 = 16;
int pinshift = 21;
int pincontrol= 22; 

int rowPins[5] = {pinrow1, pinrow2, pinrow3, pinrow4, pinrow5};  
int columnPins[10] = {pincolumn1, pincolumn2, pincolumn3, pincolumn4, pincolumn5, pincolumn6, pincolumn7, pincolumn8, pincolumn9, pincolumn10};  
int debouncetime = 40;    //40ms    //useless since rendering screen takes 150ms.


//buffer required by main loop, needs to be global
//char mainloopbuf[10];   this is too little. it crashes.
char mainloopbuf[100];  
int mainloopbufpos = 0;
int prevmainloopbufpos = 0;
int * mainloopbufpospointer = &mainloopbufpos;
int * prevmainloopbufpospointer = &prevmainloopbufpos;

long lastdebouncetime[5][22] = {0};
int lastinputstate[5][22] = {1};

bool rendermainagain = true;

//these are global for now and used by prompt, selectContact, (main) loop. Not pretty but practical for now. 
int menustate = 0;
int prevmenustate = 0;   
int menuhighlight = 0;    
int state = 0; 

//for enabling scrolling
int startpositionarrowed = 0;   //for scrolling through the page of writing a message. the starting line, taking into account up and down arrows.
int uparrows = 0;               //keep track of number of up arrows pressed
int downarrows = 0;             //keep track of number of down arrows pressed

//ORIGINAL SETUP 19 height
//u8g2.setFont(u8g2_font_profont29_mf);
//int menurowcount = 5;   
//int screenwidth = 400;
//int screenheight = 240;
//int menuitemboxy = 40;
//int menuitemboxytextoffset = 30;
//int headermargin = 8;
//int menuitemboxxtextoffset = 2;

//SMALLER SETUP 14 pixel height
//u8g2.setFont(u8g2_font_profont22_mf);
int menurowcount = 6;   
int screenwidth = 400;
int screenheight = 240;
int menuitemboxy = 34;
int menuitemboxytextoffset = 24;
int headermargin = 8;
int menuitemboxxtextoffset = 2;
int maxcharacters = 32;           //maximum characters on single line given this font size.


//u8g2.setFont(u8g2_font_profont17_mf);    //11 pixel height
//u8g2.setFont(u8g2_font_profont15_mf);    //9 pixel height

int mainmenuitemstotal = 7;   //including 0, total number of items in mainmenustrings
char * mainmenustrings[] = {"Generate new ID", "Share my ID", "Add contact ID", "Write message", "Read message", "Delete contact", "Replace contact", "Reset"};

bool displaydecryptmessage = false;
bool globalbackspace = false;
bool firstrender = true;

//We will use int char values 17, 18, 19, 20 for up, right, down, left
//B is backspace
char keyboardlowercase[5][10] = { { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0' },
                                  { 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p' },
                                  { 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 'B' },
                                  { 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '?'},
                                  { '0', '0', '0', ' ', '/', '_', 20, 17, 19, 18}};
                                  
char keyboarduppercase[5][10] = { { '!', '@', '#', '$', '%', '^', '&', '*', '(', ')' },
                                  { 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P' },
                                  { 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 'B' },
                                  { 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '/'},
                                  { '0', '0', '0', ' ', '|', '_', 20, 17, 19, 18}};
                                  
struct TestVector
{
    const char *name;
    uint8_t key[32];
    uint8_t plaintext[MAX_PLAINTEXT_LEN];
    uint8_t ciphertext[MAX_PLAINTEXT_LEN];
    uint8_t authdata[16];
    uint8_t iv[16];
    uint8_t tag[16];
    size_t authsize;
    size_t datasize;
    size_t tagsize;
    size_t ivsize;
};                                 
                                  


void setup_uart(){
  uart_init(UART0_ID, UART0_BAUD_RATE);
  gpio_set_function(0, GPIO_FUNC_UART);
  gpio_set_function(1, GPIO_FUNC_UART);
  
  // Set UART flow control CTS/RTS, we don't want these, so turn them off (for GM-803)
  uart_set_hw_flow(UART0_ID, false, false);
  
  // Set our data format, as required by GM-803 camera
  uart_set_format(UART0_ID, DATA_BITS, STOP_BITS, PARITY);  
}

void setupInput(){

  pinMode(2, OUTPUT);
  pinMode(3, OUTPUT);  // Set the button as an input
  pinMode(4, OUTPUT);  // Set the button as an input
  pinMode(5, OUTPUT);  // Set the button as an input
  pinMode(6, OUTPUT);  // Set the button as an input
  
  pinMode(7, INPUT_PULLUP);  // Set the button as an input
  pinMode(8, INPUT_PULLUP);  // Set the button as an input
  pinMode(9, INPUT_PULLUP);  // Set the button as an input
  pinMode(10, INPUT_PULLUP);  // Set the button as an input
  pinMode(11, INPUT_PULLUP);  // Set the button as an input
  pinMode(12, INPUT_PULLUP);  // Set the button as an input
  pinMode(13, INPUT_PULLUP);  // Set the button as an input
  pinMode(14, INPUT_PULLUP);  // Set the button as an input
  pinMode(15, INPUT_PULLUP);  // Set the button as an input
  pinMode(16, INPUT_PULLUP);  // Set the button as an input
  pinMode(21, INPUT_PULLUP);  // Set the button as an input
  pinMode(22, INPUT_PULLUP);  // Set the button as an input

  digitalWrite(2, HIGH);  // Pull the button high
  digitalWrite(3, HIGH);  // Pull the button high
  digitalWrite(4, HIGH);  // Pull the button high
  digitalWrite(5, HIGH);  // Pull the button high
  digitalWrite(6, HIGH);  // Pull the button high

}

void setDriveStrenthLow(){
   gpio_set_drive_strength (2, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (3, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (4, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (5, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (6, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (7, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (8, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (9, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (10, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (11, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (12, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (13, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (14, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (15, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (16, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (21, GPIO_DRIVE_STRENGTH_2MA);
   gpio_set_drive_strength (22, GPIO_DRIVE_STRENGTH_2MA);   
}
void setup()
{
  setup_uart();

  for(int i = 0;i<5;i++){
    for(int j = 0; j<22;j++){
      lastdebouncetime[i][j] = 0;
      lastinputstate[i][j] = 1;
    }
  }
  
  u8g2.begin();
  u8g2.setDisplayRotation(U8G2_R0);
  u8g2.clearBuffer();  
  //this is the original for menurowcount5
  //u8g2.setFont(u8g2_font_profont29_mf);   //19 pixel height, no overlap, mono, full
  u8g2.setFont(u8g2_font_profont22_mf);                //14 pixel height, no overlap, mono, full
  pinMode(20, OUTPUT);    //GPIO shutdown for software-controlled shutdown after timeout
  digitalWrite(20, LOW);  //Pull shutdown low.
  //builtin led
  pinMode(25, OUTPUT);
  
  //adc's for sampling battery voltage. Needs a voltage divider on pcb still.
  //adc_init();
  //adc_gpio_init(29);
  //adc_select_input(3);
 
  setupInput();
  
  //set all keyboard pins to 2mA..
  setDriveStrenthLow();
}

//These camera control functions are not working yet. The hex strings come from the GM-803 camera module manual
void cameraReset(){
  setup_uart();
  const char camreset[] = "\x7E\x00\x08\x01\x00\xD9\x50\xAB\xCD";
  uart_puts(UART0_ID, camreset);
  delay(50);
  uart_deinit(UART0_ID);
}
void cameraSleep(){
    setup_uart();
    const char camsleep[] = "\x7E\x00\x08\x01\x00\xD9\xA5\xAB\xCD";
    uart_puts(UART0_ID, camsleep);
    delay(50);
    uart_deinit(UART0_ID);
}
void cameraWake(){
    setup_uart();
    const char camwake[] = "\x7E\x00\x08\x01\x00\xD9\x00\xAB\xCD";
    uart_puts(UART0_ID, camwake);
    delay(50);
    uart_deinit(UART0_ID);
}
//turning off UTF-8 encoding, and sending pure data (pure bytes) could potentially allow us to not use base64 encoding, saving 33% space in our QR codes.
void cameraPureData(){
    const unsigned char camsleep[] = {0x7E, 0x00, 0x08, 0x01, 0x00, 0x60, 0x00, 0xAB, 0xCD };
}
void cameraWithProtocol(){
    const unsigned char camwake[] = {0x7E, 0x00, 0x08, 0x01, 0x00, 0x60, 0x01, 0xAB, 0xCD };
}

//helper functions to print keys to serial output when writing/debugging.
void printuCharAsHex(unsigned char c) {
  char hexCar[2];
  sprintf(hexCar, "%02X", c);
  Serial.print(hexCar);
}

void printNarrayuchar(unsigned char num[], int size1) {
  for(int i=0; i<size1; i++){
    printuCharAsHex(num[i]);
  }
  Serial.println("");
}

void printHex(uint8_t num) {
  char hexCar[2];
  sprintf(hexCar, "%02X", num);
  Serial.print(hexCar);
}

void printNarray(uint8_t num[], int size1) {
  for(int i=0; i<size1; i++){
    printHex(num[i]);
  }
  Serial.println("");
}

//our entropy source, ring oscillator (rosc)
void rosc_single_byte(byte * out){
    *out = rosc_hw->randombit & 1;                      //out is a pointer to a byte. initialize it, do AND with a bit from the random register
    for(int k=0; k<8; k++) {                            //8 times.. do.. (could be any other value here, but at least 8 so we fill a whole byte.
      *out = (*out << 1) ^ (rosc_hw->randombit & 1);    //left shift one position, XOR with another random byte.
    }
}

int prompt(char * promptmessage, char * options[], int totaloptions){
  int returnvalue;
    
  delay(400);                     //to prevent a right arrow to be carried over from the previous "next" button press
  char writemessagebuf[10000];
  int writemessagebufpos = 0;   
  int prevwritemessagebufpos = 0;
  int * writemessagebufpospointer = &writemessagebufpos;
  int * prevwritemessagebufpospointer = &prevwritemessagebufpos;

  rendermainagain = true; //to render it at least once..
  menustate = 0;
  prevmenustate = 0;
  menuhighlight = 0;

  while(writemessagebuf[*writemessagebufpospointer-1] != 18 && writemessagebuf[*writemessagebufpospointer-1] != 20){ //no right or left arrow.. this sort of works..
      collectInput(writemessagebuf, writemessagebufpospointer, prevwritemessagebufpospointer);
      evaluateBuffers(writemessagebuf, *writemessagebufpospointer, *prevwritemessagebufpospointer, totaloptions-1);      //this checks what is the last character in a specific buffer for a specific state.. and depending on that, sets the state..      
      renderMenu(options, promptmessage, totaloptions);                              
  }
  if(writemessagebuf[*writemessagebufpospointer-1] == 20){ //20 is left arrow. if right arrow, don't set state.
    returnvalue = 0;            //aborted (left arrow) or selected "no"
  }
  if(writemessagebuf[*writemessagebufpospointer-1] == 18){ //18 is right arrow. if right arrow, set the recipient
    returnvalue = menustate;    //0 for no, 1 for yes
  }
  resetCharBuffer(writemessagebuf, writemessagebufpospointer, prevwritemessagebufpospointer);
  return returnvalue;  
}

void deleteContact(int targetcontact, int totalcontacts){
  char targetRecipient[17];
  int targetRecipientLength = 0;
  int * targetRecipientLengthPointer = &targetRecipientLength;
  readRecipientNameAndLength(targetcontact,targetRecipient,targetRecipientLengthPointer);

  //are you sure
  char * options[] = {"No", "Yes"};
  char* str = "Delete ";
  char dest[32];
  
  strcpy(dest, str );
  strcat(dest, targetRecipient);

  int goahead = 0;
  goahead = prompt(dest, options, 2);
  if(goahead == 1){
    eraseSector(targetcontact);
    readRecipientNameAndLength(totalcontacts-1,targetRecipient,targetRecipientLengthPointer);

    unsigned char lastcontactpublickey[32] = {0};
    readKey(lastcontactpublickey, 32, totalcontacts-1, 0);

    saveKey(lastcontactpublickey, 32, targetcontact, 0, targetRecipient, 16);
    eraseSector(totalcontacts-1);
  }
}

void readmessage(int targetcontact){
  displayMessage("Scanning QR code",14,32);
  bool camerainterrupted = false;
  int readlength = 424;
  char camerainputbuffer[readlength];

  camerainterrupted = readCamera(readlength, camerainputbuffer);      
  if(!camerainterrupted){
    delay(100);
    int decodedLength1 = Base64.decodedLength(camerainputbuffer, readlength);
    char decodedString[decodedLength1];
    Base64.decode(decodedString, camerainputbuffer, readlength);
    uint8_t unsignedDecodedString[decodedLength1] = {0};
    for(int i = 0;i<decodedLength1;i++){
      unsignedDecodedString[i] = decodedString[i];
    }
    decryptmessage(unsignedDecodedString, targetcontact);
    delay(200);
  }
  else {
    displayMessage("Interrupted read",2,32);
    awaitAnyButton();
    resetCharBuffer(mainloopbuf,mainloopbufpospointer, prevmainloopbufpospointer);
    delay(200);     
  } 
}

void decryptmessage(unsigned char * encryptedbuffer, int targetcontact){
  //reverse procedure from encryptmessage, see encryptmessage for
  
  //Sender long term key, from targetrecipient
  unsigned char senderlongtermpublickeyarray[32] = {0};
  readKey(senderlongtermpublickeyarray, 32, targetcontact, 0);

  //Our private key
  unsigned char recipientlongtermprivatekeyarray[32] = {0};
  readKey(recipientlongtermprivatekeyarray, 32, -3, 0);

  //Our public key
  unsigned char recipientlongtermpublickeyarray[32] = {0};
  readKey(recipientlongtermpublickeyarray, 32, -2, 0);

  //Ephemeral public key
  static uint8_t ephemeralkeypairpublicuint[32];
  for(int i = 0;i<32;i++){
    ephemeralkeypairpublicuint[i]=encryptedbuffer[i];
  }

  //Make some copies, all necessary
  uint8_t ephemeralkeypairpublicuint2[32];
  for(int i = 0;i<32;i++){
    ephemeralkeypairpublicuint2[i]=ephemeralkeypairpublicuint[i];
  }

  uint8_t senderlongtermpublickeyarray2[32];
  for(int i = 0;i<32;i++){
    senderlongtermpublickeyarray2[i]=senderlongtermpublickeyarray[i];
  }
  uint8_t senderlongtermpublickeyarray3[32];
  for(int i = 0;i<32;i++){
    senderlongtermpublickeyarray3[i]=senderlongtermpublickeyarray[i];
  }
  uint8_t recipientlongtermprivatekeyarray3[32];
  for(int i = 0;i<32;i++){
    recipientlongtermprivatekeyarray3[i]=recipientlongtermprivatekeyarray[i];
  }
  

  //compute the ephemeral shared secret using my long-term private key and the senders ephemeral public key.
  Curve25519::dh2(ephemeralkeypairpublicuint, recipientlongtermprivatekeyarray);
  //now ephemeralkeypairpublicuint became the ephemeral shared secret

  //compute the long-term shared secret using their long-term private key and the senders long-term public key
  Curve25519::dh2(senderlongtermpublickeyarray2, recipientlongtermprivatekeyarray3);
  //now senderlongtermpublickeyarray became the long term secret key..

  //derive the encryption key using the KDF

  //Concatenate ephemeral shared secret and long term shared secret
  uint8_t keyderivationfunctioninput[64];
  for(int i = 0;i<32;i++){
    keyderivationfunctioninput[i]=ephemeralkeypairpublicuint[i];
  }
  for(int i = 32;i<64;i++){
    keyderivationfunctioninput[i]=senderlongtermpublickeyarray2[i-32];
  }

  //Concatenate all public keys for the kdf info field, in this order
  //ephemeralkeypairpublicuint2
  //recipientlongtermpublickeyarrayuint3
  //senderlongtermpublickeyarray3

  //but check if the sender did it correctly as well, with the "originals".

  uint8_t kdfinfofield[96];
  for(int i = 0;i<32;i++){
    kdfinfofield[i]=ephemeralkeypairpublicuint2[i];
  }
  for(int i = 32;i<64;i++){
    kdfinfofield[i]=recipientlongtermpublickeyarray[i-32];
  }
  for(int i = 64;i<96;i++){
    kdfinfofield[i]=senderlongtermpublickeyarray3[i-64];
  }

  //KDF
  uint8_t kdfoutput[32];
  
  HKDF<SHA256> hkdf;
  hkdf.setKey(keyderivationfunctioninput, sizeof(64));
  hkdf.extract(kdfoutput, sizeof(kdfoutput), kdfinfofield, 96);


  static TestVector const testVectorChaChaPoly_1 PROGMEM = {
      .name        = "ChaChaPoly #1",
      .key         = {},
      .plaintext   = {},
      .ciphertext  = {},
      .authdata    = {},
      .iv          = {},
      .tag         = {},
      .authsize    = 12,
      .datasize    = 114,
      .tagsize     = 16,
      .ivsize      = 12
  };

  ChaChaPoly chachapolycipher;
  ChaChaPoly * chachapolycipherpointer;
  chachapolycipherpointer = &chachapolycipher;
  TestVector testVector;
  const struct TestVector *test;
  memcpy_P(&testVector, &testVectorChaChaPoly_1, sizeof(TestVector));
  test = &testVector;
  chachapolycipherpointer->clear();

  bool setkeyreturn = false;
  //not sure what setkeyreturn was supposed to do.
  setkeyreturn = chachapolycipherpointer->setKey(kdfoutput, 32);

  bool setivreturn = false;
  setivreturn = chachapolycipherpointer->setIV(test->iv, test->ivsize);

  //we change the key every time, so we can have a static IV here.

  //pad to x chars always
  //425 is total bytes with mode 13,0
  //318 is base64 reduced length of 425 (/4, *3).
  
  int totallength = 318;
  int targetlength = totallength - 32;

  //Message is after ephemeral public key.
  static byte message[286];
  for(int i = 32;i<318;i++){
    message[i-32]=encryptedbuffer[i];
  }
  static byte outputbuffer[286] = {0};
  
  chachapolycipherpointer->decrypt(outputbuffer, message, targetlength);

  //how to deal with unsuccesful decrypt? It might crash?
  //optional chachapolycipher->addAuthData(buffer2, 128);
  
  char result[286];
  for(int i = 0;i<285;i++){
    result[i] = outputbuffer[i];
  }

  int p1 = 285;
  int p2 = 285;
  int * pp1 = &p1;
  int * pp2 = &p2;
  
  displaydecryptmessage = true;
  readdecryptedmessageloop(targetcontact, result, pp1, pp2); 
  delay(300);  
}


void encryptmessage(char * messagebuf, int * messagebufsize, int recipient){
  //We follow the this procedure for encrypting our message: https://crypto.stackexchange.com/questions/101420/eccdh-direct-or-with-temporary-ecc-keypairs
   
  //The sender generates an ephemeral key pair.
  //The sender computes an ephemeral shared secret using their ephemeral private key and the recipient's long-term public key. The ephemeral private key is then erased from memory.
  //The sender also computes a long-term shared secret using their long-term private key and the recipient's long-term public key.
  //The sender concatenates the ephemeral shared secret and long-term shared secret to form the input keying material for a KDF. 
  //The output keying material is used as the key to encrypt a message using an AEAD.
  //The sender's ephemeral public key is prepended to the ciphertext, and the ciphertext is sent to the recipient.
  //The total is encoded as a QR code and displayed
  
  //reverse at receiver

  //Sender's long term private key is conventionally at sector -3
  unsigned char senderlongtermprivatekeyarray[32] = {0};
  readKey(senderlongtermprivatekeyarray, 32, -3, 0);

  //Sender's long term public key is conventionally at sector -2
  unsigned char senderlongtermpublickeyarray[32] = {0};
  readKey(senderlongtermpublickeyarray, 32, -2, 0);
  
  //Recipient long term key is read from contacts at sector "recipient", passed into this method.
  unsigned char recipientlongtermpublickeyarray[32] = {0};
  readKey(recipientlongtermpublickeyarray, 32, recipient, 0);
      
  //Generate ephemeral keypair, just for this message.
  static uint8_t ephemeralkeypairpublicuint[32];
  static uint8_t ephemeralkeypairprivateuint[32];
  while(!RNG.available(32)){
    uint8_t rosc_single_byte2;
    uint8_t * rosc_byte_byte_pointer2 = &rosc_single_byte2;  
    rosc_single_byte(rosc_byte_byte_pointer2);   
    //1 bit entropy credit per single byte, very conservative.   
    RNG.stir(rosc_byte_byte_pointer2, 1, 1);
  }
  Curve25519::dh1(ephemeralkeypairpublicuint, ephemeralkeypairprivateuint);

  //make some copies, all neccessary..
  uint8_t recipientlongtermpublickeyarrayuint[32];
  for(int i = 0;i<32;i++){
    recipientlongtermpublickeyarrayuint[i]=recipientlongtermpublickeyarray[i];
  }

  uint8_t recipientlongtermpublickeyarrayuint2[32];
  for(int i = 0;i<32;i++){
    recipientlongtermpublickeyarrayuint2[i]=recipientlongtermpublickeyarrayuint[i];
  }
  
  uint8_t recipientlongtermpublickeyarrayuint3[32];
  for(int i = 0;i<32;i++){
    recipientlongtermpublickeyarrayuint3[i]=recipientlongtermpublickeyarrayuint[i];
  }
  
  uint8_t senderlongtermprivatekeyuint[32];
  for(int i = 0;i<32;i++){
    senderlongtermprivatekeyuint[i]=senderlongtermprivatekeyarray[i];
  }

  uint8_t senderlongtermpublickeyuint[32];
  for(int i = 0;i<32;i++){
    senderlongtermpublickeyuint[i]=senderlongtermpublickeyarray[i];
  }

  //generate the shared secret, put in the first argument, bit strange way of doing it, but that is how the library works.
  //Curve25519::dh2(alice_k, bob_f);
  //should validate public key actually still?
  
  //Ephemeral shared secret
  //The sender computes an ephemeral shared secret using their ephemeral private key and the recipient's long-term public key. The ephemeral private key is then erased from memory.
  Curve25519::dh2(recipientlongtermpublickeyarrayuint, ephemeralkeypairprivateuint);
  //now recipientlongtermpublickeyarrayuint became the ephemeral shared secret

  //Long term shared secret
  //The sender also computes a long-term shared secret using their long-term private key and the recipient's long-term public key.
  Curve25519::dh2(recipientlongtermpublickeyarrayuint2, senderlongtermprivatekeyuint);
  //now recipientlongtermpublickeyarrayuint2 became the long term secret key..

  //The sender concatenates the ephemeral shared secret and long-term shared secret to form the input keying material for a KDF. 
  uint8_t keyderivationfunctioninput[64];
  for(int i = 0;i<32;i++){
    keyderivationfunctioninput[i]=recipientlongtermpublickeyarrayuint[i];
  }
  for(int i = 32;i<64;i++){
    keyderivationfunctioninput[i]=recipientlongtermpublickeyarrayuint2[i-32];
  }

  //Concatenate all public keys for the key derivation function (kdf) info field, in this order
  //ephemeralkeypairpublicuint
  //recipientlongtermpublickeyarrayuint3
  //senderlongtermpublickeyuint

  uint8_t kdfinfofield[96];
  for(int i = 0;i<32;i++){
    kdfinfofield[i]=ephemeralkeypairpublicuint[i];
  }
  for(int i = 32;i<64;i++){
    kdfinfofield[i]=recipientlongtermpublickeyarrayuint3[i-32];
  }
  for(int i = 64;i<96;i++){
    kdfinfofield[i]=senderlongtermpublickeyuint[i-64];
  }
 
  //KDF: include all three public keys in the info parameter/context material/public context/public keys. salt is also publicly known
  //https://hal.inria.fr/hal-01463822/document (page 4)
  //https://www.rfc-editor.org/rfc/rfc5869
  //should salt still?
  //info binds the derived key material to the public keys if you include them.
  //Make sure you include the sender's and recipient's public key in the key derivation.
  //"typically including all 3 public keys involved": https://neilmadden.blog/2018/11/26/public-key-authenticated-encryption-and-why-you-want-it-part-ii/
  //(for each public key, there are several publicly computable public keys that are equivalent to it. That is why you should include them). https://www.rfc-editor.org/rfc/rfc7748#section-7 

  uint8_t kdfoutput[32];
  
  HKDF<SHA256> hkdf;
  hkdf.setKey(keyderivationfunctioninput, sizeof(64));
  hkdf.extract(kdfoutput, sizeof(kdfoutput), kdfinfofield, 96);

  Serial.println("KDF output is" );
  printNarray(kdfoutput, 32);
  

  static TestVector const testVectorChaChaPoly_1 PROGMEM = {
      .name        = "ChaChaPoly #1",
      .key         = {},
      .plaintext   = {},
      .ciphertext  = {},
      .authdata    = {},
      .iv          = {},
      .tag         = {},
      .authsize    = 12,
      .datasize    = 114,
      .tagsize     = 16,
      .ivsize      = 12
  };


  ChaChaPoly chachapolycipher;
  ChaChaPoly * chachapolycipherpointer;
  chachapolycipherpointer = &chachapolycipher;
  TestVector testVector;
  const struct TestVector *test;
  memcpy_P(&testVector, &testVectorChaChaPoly_1, sizeof(TestVector));
  test = &testVector;
  chachapolycipherpointer->clear();

  bool setkeyreturn = false;
  setkeyreturn = chachapolycipherpointer->setKey(kdfoutput, 32);

  bool setivreturn = false;
  setivreturn = chachapolycipherpointer->setIV(test->iv, test->ivsize);
  
  //uint8_t kdfoutput[32];
  //we change the key every time, so we can have a static initialization vector (iv) here.

  //pad to 299 chars always
  
  int buffersize = *messagebufsize;

  //425 is total bytes with mode 13,0
  //318 is base64 reduced length of 425 (/4, *3).
  
  int totallength = 318;
  int targetlength = totallength - 32;
  byte inputbuffer[targetlength] = {0};
  byte outputbuffer[targetlength] = {0};

  //fill up inputbuffer with message
  for(int i = 0;i<buffersize;i++){
      inputbuffer[i]=messagebuf[i];
  }
  //pad inputbuffer with spaces
  for(int i = buffersize;i<targetlength;i++){
      inputbuffer[i]=' ';
  }

  //encrypt returns void..
  chachapolycipherpointer->encrypt(outputbuffer, inputbuffer, targetlength);

  //optional?
  //chachapolycipher->addAuthData(buffer2, 128);

  //The sender's ephemeral public key is prepended to the ciphertext, and the ciphertext is sent to the recipient.
  byte totalbuffer[totallength];
  for(int i = 0;i<32;i++){
      totalbuffer[i]=ephemeralkeypairpublicuint[i];
  }
  for(int i = 32;i<totallength;i++){
      totalbuffer[i]=outputbuffer[i-32];
  }

  uint8_t arr2[318] = {0};
  for(int i = 0;i<totallength;i++){
    arr2[i]=totalbuffer[i];
  }
  char arr3[318] = {0};
    for(int i = 0;i<318;i++){
      arr3[i] = arr2[i];
  }
  //425 should be encoded length..
  //and you add a null terminating character, and pass 426 bytes/chars to displayBufferAsQR.
  //so you can have maximum 318 characters as input, including ephemeral key.
 
  //base64 part..
  int inputStringLength = 318;
  int encodedLength1 = Base64.encodedLength(inputStringLength);
  char encodedString[encodedLength1+1];
  Base64.encode(encodedString, arr3, inputStringLength);
  encodedString[encodedLength1]='\0';
  
  remove_char(encodedString, '=');
  int noPadLength = sizeof(encodedString);

  //whatever string you pass, as long as you pass size+1 it will make a valid QR code out of it.
  //that means, literal strings are always nicely null '\0' terminated.
  //this will work, mode 13/0 is 425 bytes, plus one terminator is 426.
  displayBufferAsQR(encodedString, encodedLength1+1, 13, 0, 3, 16, 80, 0);
}


//from stackoverflow, cannot remember where I found this.
//removes all occurrences of the character r from string
void remove_char(char *string, char r)
{
  int pos = 0;
  while (string[pos] != '\0')
  {
    if (string[pos] == r)
    {
      int newpos = pos;
      while (string[newpos] != '\0')
      {
        string[newpos] = string[newpos+1];
        newpos++;
      }
    }
    else pos++;
  }
}

//it still crashes on a max length of a word..
void writeMessageLoop(int targetcontact){
  //char targetRecipient[16];   //this crashes at strcat(dest, targetRecipient); unclear why.
  char targetRecipient[17];
  int targetRecipientLength = 0;
  int * targetRecipientLengthPointer = &targetRecipientLength;
  readRecipientNameAndLength(targetcontact,targetRecipient,targetRecipientLengthPointer);

  char displaystrings[30][300];     //what is the logic here? 30 lines of length 300? i guess so, then. Because ~250 is the max anyway. and 30*300 is fine..

  //max size of written message
  char writemessageinputbuf3[500]; 
  int writemessageinputbufpos3 = 0;
  int prevwritemessageinputbufpos3 = 0;
  int * writemessageinputpospointer3 = &writemessageinputbufpos3;
  int * prevwritemessageinputpospointer3 = &prevwritemessageinputbufpos3;

  rendermainagain = true; //to render it at least once..
  menustate = 0;
  prevmenustate = 0;
  menuhighlight = 0;

  char* str = "Message for ";
  char dest[32];

  strcpy(dest, str );
  strcat(dest, targetRecipient);

  //check how long this takes?
  //with millis, start measuring a bit.. collectinput and processDisplayWritingText
  long currenttime, inputtime, betweentime, displaytime1, displaytime2, displaytime3;
  int loopdisplaycounter = 0;

  while(writemessageinputbuf3[*writemessageinputpospointer3-1] != 18 && writemessageinputbuf3[*writemessageinputpospointer3-1] != 20){ //no right or left arrow.. this sort of works..
        loopdisplaycounter++;
        collectInput(writemessageinputbuf3, writemessageinputpospointer3, prevwritemessageinputpospointer3);
        //add length of message
        char mssglengthbuffer[3];
        int maxlengthprint = writemessageinputbufpos3;
        if(maxlengthprint > 299){
          maxlengthprint = 299;
        }
        sprintf(mssglengthbuffer,"%d",maxlengthprint);

    //only display if something was added to the buffer, when backspace was pressed, or upon first render
    if(*writemessageinputpospointer3 != *prevwritemessageinputpospointer3 || globalbackspace || firstrender){
            u8g2.clearBuffer();          // clear the internal memory
            u8g2.setFontMode(1);  /* activate transparent font mode */    //addition
            u8g2.setDrawColor(1);
            u8g2.drawBox(0, 0, screenwidth, menuitemboxy);                //addition
    
            u8g2.setDrawColor(2);
            u8g2.drawStr(0+menuitemboxxtextoffset,menuitemboxytextoffset,dest);
            u8g2.drawStr(350,menuitemboxytextoffset,mssglengthbuffer);
    
            //headermargin additions
            u8g2.setDrawColor(0); /* color 0 for the headermargin */
            u8g2.drawBox(0, menuitemboxy, screenwidth, headermargin);
            
            u8g2.setDrawColor(1); /* color 1 for white background */
            u8g2.drawBox(0, menuitemboxy+headermargin, screenwidth, 240-(menuitemboxy+headermargin));      
            u8g2.setDrawColor(2);
            processDisplayStrings(writemessageinputbuf3, writemessageinputpospointer3, prevwritemessageinputpospointer3);
            //updateDisplayArea from u8g2 library not working.
            u8g2.sendBuffer();
            globalbackspace = false;
            firstrender = false;
      }
  }
  if(writemessageinputbuf3[*writemessageinputpospointer3-1] == 18){            //if enter, or right arrow
  //here it will just take the first 299 characters..
  encryptmessage(writemessageinputbuf3, writemessageinputpospointer3, targetcontact);
  }
  //is this really necessary?
  //should not even be necessary.
  resetCharBuffer(writemessageinputbuf3, writemessageinputpospointer3, prevwritemessageinputpospointer3);
  firstrender = true;

  delay(300);
}

void readdecryptedmessageloop(int targetcontact, char * readmessagebuf, int * readmessagebufpospointer, int * prevreadmessagebufpospointer){
  char displaystrings[30][300];

  char targetRecipient[17];     //char targetRecipient[16];   //this crashes at strcat(dest, targetRecipient)
  int targetRecipientLength = 0;
  int * targetRecipientLengthPointer = &targetRecipientLength;
  readRecipientNameAndLength(targetcontact,targetRecipient,targetRecipientLengthPointer);

  char* str = "message from: ";
  char dest[32];
  
  strcpy(dest, str );
  strcat(dest, targetRecipient);
 
  while(readmessagebuf[*readmessagebufpospointer-1] != 18 && readmessagebuf[*readmessagebufpospointer-1] != 20){ //no right or left arrow.. this sort of works..

            collectInput(readmessagebuf, readmessagebufpospointer, prevreadmessagebufpospointer);

            u8g2.clearBuffer();
            u8g2.setFontMode(1);
            u8g2.setDrawColor(1);
            u8g2.drawBox(0, 0, screenwidth, menuitemboxy);

            u8g2.setDrawColor(2);
            u8g2.drawStr(0+menuitemboxxtextoffset,menuitemboxytextoffset,dest);
            
            //headermargin additions
            u8g2.setDrawColor(0);
            u8g2.drawBox(0, menuitemboxy, screenwidth, headermargin);
            
            u8g2.setDrawColor(1);
            u8g2.drawBox(0, menuitemboxy+headermargin, screenwidth, 240-(menuitemboxy+headermargin));      
            u8g2.setDrawColor(2);
            processDisplayStrings(readmessagebuf, readmessagebufpospointer, prevreadmessagebufpospointer);
            u8g2.sendBuffer();
            globalbackspace = false;
            firstrender = false;
    }

 //empty plaintext from memory
 for(int i = 0;i<280;i++){
    readmessagebuf[i] = 0;
 }
 *readmessagebufpospointer = 0;
 *prevreadmessagebufpospointer = 0; 

}

//modify the first line that does not end on a space and does is not a full line of characters, which we want to allow.
//if found, insert those spaces, modify the passed buffer.
//return amount of spaces added, 0 when done.
int modifyFirstLine(char * passedbuffer, int passedbuffertotallength, int passedbufferend){
  
  int totallines = passedbufferend/maxcharacters;
  //find first line with a space..
  int firstlinewithspace = -1;
  int firstspace = -1;
  
  //find first line that does not end in space.
  //and is not all characters
  int firstillegallinewithoutspace = -1;

  bool fullcharacterline = true;
  for(int i = 0;i<totallines;i++){
    if(passedbuffer[((i+1)*(maxcharacters))-1]!=' '){   //if last character of line i is not a space, found an potential illegal line.
      for(int j = 0;j<maxcharacters;j++){                              
        if(passedbuffer[i*maxcharacters+j]==' '){       
          fullcharacterline = false;                    
          break;                                        //found at least one space, so line i is not a full-character line, which we want to allow
        }
      }
      if(!fullcharacterline){
        firstillegallinewithoutspace = i;
      }
      if(firstillegallinewithoutspace!=-1){
        break;                                          //break from loop because we found the first illegal line.
      } 
    }
  }

  if(firstillegallinewithoutspace == -1){
    //all lines are legal, done
    return 0;
  }
  else{
    //find last space of this line (32-j of firstillegallinewithoutspace)
    //from end of passedbuffer until last space of firstillegallinewithoutspace (passedbuffer[z]=passedbuffer[z-j]), i.e. shift right all chars j positions
    //insert j spaces from at that position
    //return amount that buffer has increased in this function  (32-lastspace)

    //find last space
    int lastspace = -1;
    for(int i = 31;i>=0;i--){
      if(passedbuffer[((firstillegallinewithoutspace)*maxcharacters)+i]==' '){
        lastspace = i;
        break;
      }
    }

    if(lastspace == -1){
      //illegal condition, there should always be a space..
    }

    int shiftright = maxcharacters-lastspace-1;     //added -1 to prevent space in front of line
    int positionoflastspace = ((firstillegallinewithoutspace)*maxcharacters)+lastspace;   
    //shift right
    for(int i = passedbufferend+shiftright;i>positionoflastspace;i--){
      passedbuffer[i]=passedbuffer[i-shiftright];
    }
    //insert some spaces there..
    for(int i = positionoflastspace;i<positionoflastspace+shiftright;i++){
      passedbuffer[i]=' ';
    }  
    return shiftright;    
  }
}

void processDisplayStrings(char * writemessagebuf, int * writemessagebufpospointer, int * prevwritemessagebufpospointer){
  //40 lines max, 40 length max
  char displaystrings[80][160];
  char copybuffer[500];

  int amountOfLines = *writemessagebufpospointer/maxcharacters;
  //needs additional amountOfLines.. since amountOfLines could increase with
  int modifiedAmountOfLines = 0;

    
  for(int i = 0;i<80;i++){
    displaystrings[i][0]='\0';
  }
  
  //first, fill a copy
  for(int i = 0;i<*writemessagebufpospointer;i++){
    copybuffer[i]=writemessagebuf[i];
    copybuffer[i+1]='\0';
  }

  int modifiedBufferTotalLength = *writemessagebufpospointer;
  
  bool continueme = true;
  int addingSpaces = 0;
  while(continueme){
    addingSpaces = modifyFirstLine(copybuffer, 500, modifiedBufferTotalLength);
    if(addingSpaces !=0){
          modifiedBufferTotalLength+=addingSpaces;
    }
    else if(addingSpaces==0){
      continueme = false;
    }
    else{
      //invalid
    }
  }

  modifiedAmountOfLines = modifiedBufferTotalLength/maxcharacters;
  
  for(int i = 0;i<=modifiedAmountOfLines;i++){
    for(int j = 0;j<32;j++){                          //this always "fills up" a line completely.. should be fine? or will garbage show up?
      displaystrings[i][j]=copybuffer[i*32+j];
      displaystrings[i][j+1]='\0';                  
    }
  }

  determineScrollStatus(writemessagebuf, writemessagebufpospointer, prevwritemessagebufpospointer, modifiedAmountOfLines);

  char empty[1] = {'\0'};
  if(*writemessagebufpospointer == 0){
    u8g2.drawStr(0+menuitemboxxtextoffset,(1)*menuitemboxy + menuitemboxytextoffset + headermargin,empty);
  }

  else if(modifiedAmountOfLines<menurowcount){      //-1 here writes garbage lines on first line.. because it goes to next statement.. so SHOULD be correct.
    //original, like this it draws a space first.
    for(int i = 0;i<=modifiedAmountOfLines;i++){
        u8g2.drawStr(0+menuitemboxxtextoffset,(i+1)*menuitemboxy + menuitemboxytextoffset + headermargin,displaystrings[i]);
    }
  }
  else {  //modifiedAmountOfLines is greater      //so here, startpositionarr is still -1.. which is not correct..
    int linecounterextra = 0;
    for(int i = startpositionarrowed;i<=modifiedAmountOfLines;i++){
        u8g2.drawStr(0+menuitemboxxtextoffset,(linecounterextra+1)*menuitemboxy+menuitemboxytextoffset+headermargin,displaystrings[i]);
        linecounterextra++;
    }
  }     
}

void determineScrollStatus(char * writemessagebuf, int * writemessagebufpospointer, int * prevwritemessagebufpospointer, int modifiedrowcount){
  if (writemessagebuf[*writemessagebufpospointer-1] == 17 && *writemessagebufpospointer > *prevwritemessagebufpospointer) {          //up arrow
        if(startpositionarrowed > 0){
          uparrows++;
        }
        //make sure one arrow gets removed from the buffer itself.
        writemessagebuf[*writemessagebufpospointer-1]=' ';
        if(*writemessagebufpospointer <= 0){      //this works like a charm, actually! :)
          *writemessagebufpospointer=0;
          *prevwritemessagebufpospointer=0;
        }
        else{
          --*writemessagebufpospointer;
          --*prevwritemessagebufpospointer;
        }
    }
    if (writemessagebuf[*writemessagebufpospointer-1] == 19 && *writemessagebufpospointer > *prevwritemessagebufpospointer) {          //down arrow
        if(startpositionarrowed < (modifiedrowcount+1) - menurowcount){
          downarrows++; 
        }
        //make sure one arrow gets removed from the buffer itself.
        writemessagebuf[*writemessagebufpospointer-1]=' ';
        if(*writemessagebufpospointer <= 0){
          *writemessagebufpospointer=0;
          *prevwritemessagebufpospointer=0;
        }
        else{
          --*writemessagebufpospointer;
          --*prevwritemessagebufpospointer;
        }              
    }
    if(modifiedrowcount+1>menurowcount){
      startpositionarrowed = (modifiedrowcount+1) - menurowcount - uparrows + downarrows;
    }
    else{
      startpositionarrowed = 0;
    }
    if(startpositionarrowed < 0){
      startpositionarrowed = 0;
    }
    //to display message from the start..
    //something like firstrender this..
    if(displaydecryptmessage){
      startpositionarrowed = 0;
      if(uparrows != 0 || downarrows != 0){
        displaydecryptmessage = false;
        uparrows = modifiedrowcount - menurowcount;    //to start from the beginning instead of the end..
        startpositionarrowed = modifiedrowcount - menurowcount - uparrows + downarrows;
      }
    }
    //Serial.println("menurowcount " + String(menurowcount) + " modifiedrowcount " + modifiedrowcount + " startpositionarr " + String(startpositionarrowed) + " uparrows " + String(uparrows) + " downarrows " + String(downarrows)); 
}

//read keypad, debounce input.
void collectInput(char *inputbuffer, int* inputbufferpositionpointer, int* inputbufferpreviouspositionpointer){
  //active low, low reading is input detected.
  int rowreading = 0;
  int colreading = 0;
  int output = 0;
  
  for (int row=0; row<5; row++)
  {
    pinMode(rowPins[row], OUTPUT);    //set the row pin as an output
    digitalWrite(rowPins[row], LOW);  //pull that row pin low
    for (int col=0; col<10; col++)
    {
      int reading = digitalRead(columnPins[col]);
      if (!reading) //active low
      {
        if(lastinputstate[row][col]!=reading){                //and it's not the same as before
          if ((millis() - lastdebouncetime[row][col]) > debouncetime){  //and enough time has passed
              rowreading = row;
              colreading = col;
              //lastinputstate[row][col] = reading;
              lastinputstate[row][col] = 1;                   //reset to inactive right away. otherwise reset further up to prevent retriggers
              output = (((row+1)*100) + (col+1));             //set the status bit of the keypad return value
              lastdebouncetime[row][col] = millis();          //update last detected keypress for debouncing
          }
        }
      }
    }
    pinMode(rowPins[row], INPUT);     //reset the row pin as an input
    digitalWrite(rowPins[row], HIGH); //pull the row pin high
  }
  
  if (output != 0){
    //set inputbufferpreviouspositionpointer because sendKeyPress updates inputbufferpositionpointer
    *inputbufferpreviouspositionpointer = *inputbufferpositionpointer; 
    sendKeyPress(rowreading, colreading, inputbuffer, inputbufferpositionpointer, inputbufferpreviouspositionpointer);
  }
  else{ 
    //no input, so prev=current. 
    *inputbufferpreviouspositionpointer = *inputbufferpositionpointer;
  }
}


//evaluateBuffers was originally written to cooperate with renderMenu, figure out which arrows were pressed and set menustate accordingly.
//renderMenu/evaluateBuffers also used for displaying select contact menu and prompt.
void evaluateBuffers(char passedbuffer[], int passedbufferpositionpointer, int passedpreviousbufferpositionpointer, int targetmenuitemstotal){
  //characters 17, 18, 19, 20 are used conventionally for up, right, down, left arrows
  //UP ARROW
  if (passedbuffer[passedbufferpositionpointer-1] == 17 && passedbufferpositionpointer > passedpreviousbufferpositionpointer) {
    if (menustate <= 0 ){
      menustate = 0;                    //for ending at the end. no scrolling around
    }
    else{
      menustate--;
    }
  }  
  //DOWN ARROW
  else if (passedbuffer[passedbufferpositionpointer-1] == 19 && passedbufferpositionpointer > passedpreviousbufferpositionpointer){     //down arrow    && menustate != prevmenustate
    if (menustate >= 0 && menustate < targetmenuitemstotal){   
      menustate++;        
    }
    else if (menustate >= targetmenuitemstotal){
      menustate = targetmenuitemstotal;
    }
    else{
  
    }
  }    
  //RIGHT ARROW
  else if (passedbuffer[passedbufferpositionpointer-1] == 18 && passedbufferpositionpointer > passedpreviousbufferpositionpointer){
    //if in main menu, set the state.. ok that makes sense..
    //if in any other menu, those "right arrows" are caught locally for example for nameInputLoop and prompt and selectContact
    if(state == 0){
      state=menustate+1;
    }
  }
  //LEFT ARROW
  else if (passedbuffer[passedbufferpositionpointer-1] == 20 && passedbufferpositionpointer > passedpreviousbufferpositionpointer){
    state=0;
  }
  else{
    //some other character received while scrolling the passed menu. Ignore.
  }                               
}

bool nameInputLoop(char passedbuffer[], int *passedbufferpositionpointer, int *passedpreviousbufferpositionpointer){
  bool breakreturn = false;    
  //header
  u8g2.clearBuffer();   
  u8g2.setFontMode(1);
  u8g2.setDrawColor(1);
  u8g2.drawBox(0, 0, screenwidth, menuitemboxy);
  u8g2.setDrawColor(0);
  u8g2.drawStr(0+menuitemboxxtextoffset, menuitemboxytextoffset, "Enter contact name");

  //headermargin additions
  u8g2.setDrawColor(0); //color 0 for the headermargin (black)
  u8g2.drawBox(0, menuitemboxy, screenwidth, headermargin);  

  //white box
  u8g2.setDrawColor(1); // color 1 for white background (white)
  u8g2.drawBox(0, menuitemboxy+headermargin, screenwidth, 240-(menuitemboxy+headermargin));    
  u8g2.sendBuffer();
  
  while(passedbuffer[*passedbufferpositionpointer-1] != 18 && passedbuffer[*passedbufferpositionpointer-1] != 20){          //if no left or right arrow, continue
    collectInput(passedbuffer, passedbufferpositionpointer, passedpreviousbufferpositionpointer);
    if(*passedbufferpositionpointer != *passedpreviousbufferpositionpointer || globalbackspace || firstrender){
      u8g2.clearBuffer();
      u8g2.setDrawColor(1); 
      u8g2.drawBox(0, 0, screenwidth, menuitemboxy);
      u8g2.setDrawColor(0);
      u8g2.drawStr(0+menuitemboxxtextoffset, menuitemboxytextoffset, "Enter contact name");
    
      //headermargin additions
      u8g2.setDrawColor(0); //color 0 for the headermargin (black)
      u8g2.drawBox(0, menuitemboxy, screenwidth, headermargin);  
    
      //white box
      u8g2.setDrawColor(1); // color 1 for white background (white)
      u8g2.drawBox(0, menuitemboxy+headermargin, screenwidth, 240-(menuitemboxy+headermargin));
        
      char mssglengthbuffer[2];
      int maxlengthprint = *passedbufferpositionpointer;
      if(maxlengthprint > 16){
        maxlengthprint = 16;
      }
      sprintf(mssglengthbuffer,"%d",maxlengthprint);
      //without this println, backspace does not work correctly and random commas show up..
      Serial.println("mssglengthbuffer is "+String(mssglengthbuffer));
       
      char dest[5];
      strcpy(dest, mssglengthbuffer );
      strcat(dest, "/16");
      u8g2.setDrawColor(0); // color 0 for black text
      u8g2.drawStr(320,menuitemboxytextoffset,dest);
      char name16[16];
      name16[0]=' ';        //otherwise garbage characters show up.
      name16[1]=' ';        //otherwise garbage characters show up.
      for(int i = 0;i<maxlengthprint;i++){
        name16[i]=passedbuffer[i];
      }
      u8g2.drawStr(0+menuitemboxxtextoffset, menuitemboxy + menuitemboxytextoffset, name16);
      u8g2.sendBuffer(); 
      firstrender = false;
      globalbackspace = false;
    }
  }

  //characters 17, 18, 19, 20 are used conventionally for up, right, down, left arrows
  //if left arrow
  if(passedbuffer[*passedbufferpositionpointer-1] == 20){
    breakreturn = true;
  }
  else if(*passedbufferpositionpointer<= 2){   //name needs to be at least length 2
    displayMessage("Name should be at least 2 chars.",0,32);
    awaitAnyButton();
    breakreturn = true;
  }
  else if(nameExists(passedbuffer, passedbufferpositionpointer)){
    displayMessage("Name exists.",0,32);
    awaitAnyButton();
    breakreturn = true;  
  }
  else{     //valid name, make sure to only take 16 chars
    if(*passedbufferpositionpointer > 16){
        *passedbufferpositionpointer = 16;
    }
  }
  firstrender = true;
  return breakreturn;
}

bool nameExists(char passedbuffer[], int *passedbufferpositionpointer){
  bool returncheck = false;
  
  //every sector represents a possible contact, so firstEmptySector equals the amount of existing contacts
  int amountOfContacts = firstEmptySector();
  
  //this determines the length of every name and puts it in nameLenghts
  uint8_t nameLengths[amountOfContacts];
  collectNameLengths(amountOfContacts, nameLengths);
  for(int i = 0;i<amountOfContacts;i++){
    size_t mysize = nameLengths[i];
    Serial.println("lengths is "+String(mysize));
  }

  char** strings = (char**)malloc(amountOfContacts * sizeof(char*));    
  for (int i = 0; i < amountOfContacts; i++){
    strings[i] = (char*)malloc((32+1) * sizeof(char));    //for now, every string can be of size 32.
  }  

  //this will put the names in the strings array
  collectNames(amountOfContacts, strings);  
  for (int i = 0; i < amountOfContacts; i++){
  }

  //for all names
  for(int i = 0;i<amountOfContacts;i++){
    //create a new comparename of correct length
    char comparename[nameLengths[i]];
    for(int j = 0;j<nameLengths[i];j++){
      comparename[j]=strings[i][j];
    }
    uint8_t comparename1length = nameLengths[i];
    uint8_t comparename2length = *passedbufferpositionpointer-1;
        
    char comparename2[comparename2length];
    for(int j = 0;j<comparename2length;j++){
      comparename2[j]=passedbuffer[j];
    }

    //Any name that differs in length is not identical
    if(comparename1length == comparename2length){
      if( strncmp(comparename, comparename2, comparename1length) == 0){
        returncheck = true;
        break;
      }
    }
  }
  return returncheck;
}

void readContactIntoSector(int targetsector, char* nameinputbuf, int nameinputbufpos){
  Serial.println("in read contact into sector " + String(targetsector) + " " + String(nameinputbuf) + " " + String(nameinputbufpos));
    if(targetsector == -1){
      displayMessage("Memory full. Delete contacts",2,32);
      awaitAnyButton();
      returnToMain();          
    }
    else{
      displayMessage("Scanning QR code",14,32);
      char targetbuffer[44];
      bool camerainterrupted = false; 
      camerainterrupted = readCamera(44, targetbuffer);       //read 44 chars (base64 encoded 32 bytes) into targetbuffer
      if(!camerainterrupted){
        int inputStringLength = sizeof(targetbuffer);      
        int decodedLength1 = Base64.decodedLength(targetbuffer, inputStringLength);
        char decodedString[decodedLength1];
        Base64.decode(decodedString, targetbuffer, inputStringLength);
        uint8_t saveinput[32] = {0};
        for(int i = 0;i<32;i++){
          saveinput[i] = decodedString[i];
        }
        saveKey(saveinput, 32, targetsector, 0, nameinputbuf, nameinputbufpos-1);      
        displayMessage("Contact added",2,32);       
        awaitAnyButton();
        returnToMain();
      }
      else{
          displayMessage("Read interrupted",2,32);
          awaitAnyButton();
          returnToMain();
      }
    }
}

void loop(){
  //collectinput for the main menu (mainloopbuf)
  collectInput(mainloopbuf, mainloopbufpospointer, prevmainloopbufpospointer);
  //check last character main menu buffer (stfbuf), set state accordingly
  evaluateBuffers(mainloopbuf, *mainloopbufpospointer, *prevmainloopbufpospointer, mainmenuitemstotal);      
  //act upon the state 
  executeMenuAction(state);
}

void executeMenuAction(int passedMenuState){
  switch (passedMenuState){
  case 0:{  //no action, just render the menu
    renderMenu(mainmenustrings, "        qryptr 1.25", mainmenuitemstotal);
    break;
  }
  case 1:{  //generate new keypair for user. this represent the cryptographic identity of the user.
    bool idexists = true;
    idexists = !isSectorEmpty(-2) && !isSectorEmpty(-3);    //pub and priv keys reside at sector -2 and -3 conventionally
    if(!idexists){
      generateKeypair();                              
      displayMessage("New ID generated",16,32);
      awaitAnyButton();
    }
    else{
      int generatecontinue = 0;
      char* options[] = {"No", "Yes"};
      char* str = "Found existing key. Overwrite?";
      generatecontinue = prompt(str, options, 2);
      if(generatecontinue == 1){
        generateKeypair();                              
        displayMessage("New ID generated",16,32);
        awaitAnyButton();
      }
    }
    returnToMain();
    break;  
  }
  case 2: {//share my public key as QR code on display
    bool idexists = true;
    idexists = !isSectorEmpty(-2) && !isSectorEmpty(-3);    //pub and priv keys reside at sector -2 and -3 conventionally

    if(idexists){

      //this could be done more elegantly, instead of having 2 arrays.
      uint8_t targetarray[32] = {0};
      readKey(targetarray, 32, -2, 0);                           
      char targetarray2[32] = {0};
      for(int i = 0;i<32;i++){
        targetarray2[i] = targetarray[i];
      }
      
      //We encode bytes of the public key as base64 characters before encoding it as a QR code.
      int inputStringLength = sizeof(targetarray2);
      int encodedLength1 = Base64.encodedLength(inputStringLength);
      Serial.println("encodedLength1 is "+String(encodedLength1));
      char encodedString[encodedLength1];
      Base64.encode(encodedString, targetarray2, inputStringLength);      
      remove_char(encodedString, '=');
      int noPadLength = sizeof(encodedString);

      int newLength = sizeof(encodedString);
      displayBufferAsQR(encodedString, newLength, 6, 0, 5, 16, 84, 0);    
      awaitAnyButton();
    }
    else{
        displayMessage("No ID found",16,32);
        awaitAnyButton();
    }
    returnToMain();
    break;
  }
  case 3:{      //add contact id
    char nameinputbuf[100]; 
    int nameinputbufpos = 0;
    int prevnameinputbufpos = 0;
    int * nameinputpospointer = &nameinputbufpos;
    int * prevnameinputpospointer = &prevnameinputbufpos;

    bool breakreturn = nameInputLoop(nameinputbuf, nameinputpospointer, prevnameinputpospointer);
    
    if(breakreturn){
      returnToMain();
      break;
    }
    else {                                               //succesfully put name in nameinputbuf (max 32). So read the QR code, and put the name plus key into the first empty sector.
      int targetsector = firstEmptySector();
      readContactIntoSector(targetsector, nameinputbuf, nameinputbufpos);
      break;
    }
    }
    case 4: {       //write message
      int amountOfContacts = firstEmptySector();
      if(amountOfContacts == 0){
        displayMessage("No contacts",2,32);
        awaitAnyButton();
        returnToMain();
        break;
      } 
      //in this case, we don't know how many contacts we have. so how do we allocate enough memory?
      //https://stackoverflow.com/questions/7652293/how-do-i-dynamically-allocate-an-array-of-strings-in-c
      char** strings = (char**)malloc(amountOfContacts * sizeof(char*));    
      for (int i = 0; i < amountOfContacts; i++){
        strings[i] = (char*)malloc((32+1) * sizeof(char));    //for now, every contact string can be of size 32, including null terminator.
      }
      //will put all the names in strings
      collectNames(amountOfContacts, strings);  
      int targetcontact = selectContact(strings, amountOfContacts, "select recipient");
      if(targetcontact!=-1){
          delay(500);
          writeMessageLoop(targetcontact);    
      }
      returnToMain();
      for (int i = 0; i < amountOfContacts; i++){
        free(strings[i]);
      }
      free(strings);
      break;
    }
    case 5: {       //read message
      int amountOfContacts = firstEmptySector();
      if(amountOfContacts == 0){
        displayMessage("No contacts",2,32);
        awaitAnyButton();
        returnToMain();
        break;
      } 
      char** strings = (char**)malloc(amountOfContacts * sizeof(char*));    
      for (int i = 0; i < amountOfContacts; i++){
        strings[i] = (char*)malloc((32+1) * sizeof(char));
      }
      collectNames(amountOfContacts, strings);  
      int targetcontact = selectContact(strings, amountOfContacts, "select sender");
      if(targetcontact!=-1){
          readmessage(targetcontact);    
      }
      returnToMain();
      for (int i = 0; i < amountOfContacts; i++){
        free(strings[i]);
      }
      free(strings);
      break;
    }
    case 6: {     //delete contact
      int amountOfContacts = firstEmptySector();
      if(amountOfContacts == 0){
        displayMessage("No contacts",2,32);
        awaitAnyButton();
        returnToMain();
        break;
      } 
      char** strings = (char**)malloc(amountOfContacts * sizeof(char*));    
      for (int i = 0; i < amountOfContacts; i++){
        strings[i] = (char*)malloc((32+1) * sizeof(char));    //for now, every string can be of size 32.
      }
      collectNames(amountOfContacts, strings);  
      int targetcontact = selectContact(strings, amountOfContacts, "delete contact");
      if(targetcontact!=-1){
          deleteContact(targetcontact, amountOfContacts);    
      }
      returnToMain();
      for (int i = 0; i < amountOfContacts; i++){
        free(strings[i]);
      }
      free(strings);
      break;
    }
    case 7: {     //replace contact 
      int amountOfContacts = firstEmptySector();
      if(amountOfContacts == 0){
        displayMessage("No contacts",2,32);
        awaitAnyButton();
        returnToMain();
        break;
      } 
      char** strings = (char**)malloc(amountOfContacts * sizeof(char*));    
      for (int i = 0; i < amountOfContacts; i++){
        strings[i] = (char*)malloc((32+1) * sizeof(char));    //for now, every string can be of size 32.
      }
      collectNames(amountOfContacts, strings);  
      int targetcontact = selectContact(strings, amountOfContacts, "replace contact");
      if(targetcontact!=-1){
          char targetRecipient[33];
          int targetRecipientLength = 0;
          int * targetRecipientLengthPointer = &targetRecipientLength;
          readRecipientNameAndLength(targetcontact,targetRecipient,targetRecipientLengthPointer);
          readContactIntoSector(targetcontact, targetRecipient, *targetRecipientLengthPointer);
      }
      returnToMain();
      for (int i = 0; i < amountOfContacts; i++){
        free(strings[i]);
      }
      free(strings);
      break;
    }
    case 8: {       //reset all, useful for resetting the entire memory
      char * options[] = {"No", "Yes"};
      char* str = "Reset everything? ";
      int confirm = 0;
      char* str2 = "Your contacts and keys will be lost ";
      int confirm2 = 0;
      confirm = prompt(str, options, 2);
      if(confirm == 1){
        confirm2 = prompt(str2, options, 2);
        if(confirm2 == 1){
          displayMessage("This may take a while",2,32);
          resetMemory();
          displayMessage("Everything reset",2,32);
          awaitAnyButton();
        }
      }
      returnToMain();
      break;
    }
    case 9: {
      //refer here for the ascii codes
      //https://github.com/olikraus/u8g2/wiki/fntgrpprofont
      char a1 = 224;
      char a2 = 225;
      char a3 = 226;
      char a4 = 227;
      char chararray1[4] = {a1, a2, a3, a4};
      displayMessage(chararray1, 10, 40);
      awaitAnyButton(); 
      returnToMain();
      break;
    }  
  }
}

//We still use several global variables. Reset them when returning from executeMenuAction.
void returnToMain(){
      startpositionarrowed = 0;
      uparrows = 0;
      downarrows = 0;
      menustate = 0;
      prevmenustate = 0;
      menuhighlight = 0;
      state = 0;
      rendermainagain = true;
      resetCharBuffer(mainloopbuf,mainloopbufpospointer, prevmainloopbufpospointer);
      delay(250);
}

void collectNames(int sectorposition, char ** targetarray){
  for(int i = 0;i<sectorposition;i++){
    int flash_target_offset_2 = (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE*200)+(FLASH_SECTOR_SIZE*i);
    int *p, page, addr, *p2, page2, addr2;
    //the name of the contact is at page 10 of the sector, conventionally.
    //the length of the name is at page 11 of the sector, conventionally.

    addr = XIP_BASE + flash_target_offset_2 + (10 * FLASH_PAGE_SIZE) ;
    addr2 = XIP_BASE + flash_target_offset_2 + (11 * FLASH_PAGE_SIZE) ;
    p = (int *)addr;    // place an int pointer at our memory-mapped address
    p2 = (int *)addr2;  // place an int pointer at our memory-mapped address

    int lengthofname = *p2; 
    for(int j = 0;j<lengthofname;j++){
      Serial.print((char)*p);      
      targetarray[i][j] = (char)*p;
      *p++;
    }
    //34 because the display can display 34 characters in total
    for(int k = lengthofname;k<34;k++){
      targetarray[i][k] = ' ';
      *p++;
    }
  }
}

void collectNameLengths(int sectorposition, uint8_t * targetarray){
  for(int i = 0;i<sectorposition;i++){
    int flash_target_offset_2 = (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE*200)+(FLASH_SECTOR_SIZE*i);
    int *p2, page2, addr2;
    //the length of the name is at page 11 of the sector, conventionally.
    addr2 = XIP_BASE + flash_target_offset_2 + (11 * FLASH_PAGE_SIZE) ;
    p2 = (int *)addr2; // place an int pointer at our memory-mapped address
    int lengthofname = *p2;
    targetarray[i] = (char)*p2;
    *p2++;
  }
}

int selectContact(char * mynames[], int totalcontacts, char *textheader){
  int targetcontact = -1;
  delay(400); //to prevent a right arrow to be carried over from the previous "next"
  char localbuffer[10000];
  int localbufferposition = 0;
  int previouslocalbufferpostion = 0;
  int * localbufferpositionpointer = &localbufferposition;
  int * previouslocalbufferpostionpointer = &previouslocalbufferpostion;

  rendermainagain = true;     //to render it at least once..
  menustate = 0;
  prevmenustate = 0;
  menuhighlight = 0;

  while(localbuffer[*localbufferpositionpointer-1] != 18 && localbuffer[*localbufferpositionpointer-1] != 20){              //no right or left arrow..
      collectInput(localbuffer, localbufferpositionpointer, previouslocalbufferpostionpointer);
      evaluateBuffers(localbuffer, *localbufferpositionpointer, *previouslocalbufferpostionpointer, totalcontacts-1);      //check the last character in a specific buffer for a specific state and set the state accordingly   
      renderMenu(mynames, textheader, totalcontacts);                            
  }
  //characters 17, 18, 19, 20 are used conventionally for up, right, down, left arrows
  if(localbuffer[*localbufferpositionpointer-1] == 18){ //18 is right arrow. if right arrow, set the recipient
    targetcontact = menustate;
    //readRecipientName(targetcontact);   //should be replaced by readRecipientNameAndLength by now.
  }
  resetCharBuffer(localbuffer, localbufferpositionpointer, previouslocalbufferpostionpointer);
  return targetcontact;
}


//this is to get rid of global var selectedrecipientname and read the name and length into whatever is passed into the function.
//200 is the convention for the memory offset, that is, the location, where contacts are stored. The last 200 sectors of memory are used for contacts.
//recipientindex is the sector number of the target contact.
void readRecipientNameAndLength(int recipientindex, char* targetRecipient, int *targetRecipientLength){
    int flash_target_offset_2 = (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE*200)+(FLASH_SECTOR_SIZE*recipientindex);
    int *p, page, addr, *p2, page2, addr2;
    //page 10 and 11 of the sector are used conventionally for the name and length of the name.
    addr = XIP_BASE + flash_target_offset_2 + (10 * FLASH_PAGE_SIZE) ;
    addr2 = XIP_BASE + flash_target_offset_2 + (11 * FLASH_PAGE_SIZE) ;
    p = (int *)addr;    // place an int pointer at our memory-mapped address
    p2 = (int *)addr2;  // place an int pointer at our memory-mapped address

    int lengthofname = *p2;
    for(int j = 0;j<lengthofname;j++){
      targetRecipient[j] = (char)*p;
      *p++;
    }
    for(int k = lengthofname;k<16;k++){
      targetRecipient[k] = ' ';
      *p++;
    }
    targetRecipient[16]='\0';
    *targetRecipientLength = lengthofname +1;
}


void resetCharBuffer(char * targetbuffer, int * targetbufpos, int * prevtargetbufpos){
  int targetsize = sizeof(targetbuffer);
  for(int i = 0;i<targetsize;i++){
    targetbuffer[i]=0;
  }
  *targetbufpos = 0;
  *prevtargetbufpos = 0;
}

//renderMenu uses some global variables: menustate, prevmenustate, menuhighlight, menurowcount
//menurowcount is global, set once depending on font height, currently there are 6 menurows.
//but we also set menurowcount to 2? in prompt? no we don't..
//we manipulate global menuhighlight, menustate and prevmenustate in this method.
//this only sets menuhighlight, prevmenustate
//in theory, yes, you could pass menustate, prevmenustate, etc, everything, into here.. but that's a lot of work still.
void renderMenu(char * passedmenu[], char *headerstring, int passedmenusize){

  //this is to enable prompts with 2 options
  int localmenurowcount = menurowcount;
  if(menurowcount > passedmenusize){
    localmenurowcount = passedmenusize;
  }    
  u8g2.clearBuffer();    
  u8g2.setFontMode(1);  //activate transparent font mode
  u8g2.setDrawColor(1); // color 1 for the box
  u8g2.drawBox(0, 0, screenwidth, menuitemboxy);
  u8g2.setDrawColor(0);
  u8g2.drawStr(menuitemboxxtextoffset, menuitemboxytextoffset, headerstring);
  
  //headermargin additions
  u8g2.setDrawColor(0);
  u8g2.drawBox(0, menuitemboxy, screenwidth, headermargin);
                                  
  if(menustate == prevmenustate +1){                              //scrolling down
    //if(printactive){Serial.println("scrolling down");}
    if(menuhighlight == 0){                                       //at first item, highlight = highlight +1 
      menuhighlight = menuhighlight +1;
      prevmenustate = menustate; 
      for(int i = 0;i<localmenurowcount;i++){
        u8g2.setDrawColor(menuhighlight==i);                      //if true, color the box black, otherwise, color the box white
        u8g2.drawBox(0, headermargin+(i+1)*menuitemboxy, screenwidth, menuitemboxy);       
        u8g2.setDrawColor(2);
        u8g2.drawStr(menuitemboxxtextoffset, headermargin+(i+1)*menuitemboxy + menuitemboxytextoffset, passedmenu[menustate-menuhighlight+i]);
      }
    }
    else if (menuhighlight != localmenurowcount -1 && menuhighlight!=0){  //in between items
      menuhighlight = menuhighlight +1;
      prevmenustate = menustate;
      for(int i = 0;i<localmenurowcount;i++){
        u8g2.setDrawColor(menuhighlight==i);                      
        u8g2.drawBox(0, headermargin+ (i+1)*menuitemboxy, screenwidth, menuitemboxy);                       
        u8g2.setDrawColor(2);
        u8g2.drawStr(menuitemboxxtextoffset, headermargin+ (i+1)*menuitemboxy + menuitemboxytextoffset, passedmenu[menustate-menuhighlight+i]);
      }
    }
    else if (menuhighlight == localmenurowcount-1){               //at the bottom
      menuhighlight = menuhighlight;                                  
      prevmenustate = menustate;
      for(int i = 0; i <localmenurowcount;i++){
        u8g2.setDrawColor(menuhighlight==i);
        u8g2.drawBox(0, headermargin+ (i+1)*menuitemboxy, screenwidth, menuitemboxy);
        u8g2.setDrawColor(2);
        u8g2.drawStr(menuitemboxxtextoffset, headermargin+ (i+1)*menuitemboxy + menuitemboxytextoffset, passedmenu[(menustate-(localmenurowcount-1))+i]);
      }    
    }
    else{
      Serial.println("Found a bug");
    }
    u8g2.sendBuffer();            // transfer internal memory to the display
  }
   else if(menustate == prevmenustate -1){                       //scrolling up
      if(menuhighlight == 0){                                    //at first item, highlight stays the same   
        menuhighlight = 0;
        prevmenustate = menustate;                                
        for(int i = 0;i<localmenurowcount;i++){
          u8g2.setDrawColor(menuhighlight==i);
          u8g2.drawBox(0, headermargin+ (i+1)*menuitemboxy, screenwidth, menuitemboxy);
          u8g2.setDrawColor(2);
          u8g2.drawStr(menuitemboxxtextoffset, headermargin+ (i+1)*menuitemboxy + menuitemboxytextoffset, passedmenu[menustate-menuhighlight+i]);
        }
      }
      else if (menuhighlight != localmenurowcount -1 && menuhighlight!=0){  //in between, highlight = highlight -1
        menuhighlight = menuhighlight - 1;
        prevmenustate = menustate;
        for(int i = 0;i<localmenurowcount;i++){
          u8g2.setDrawColor(menuhighlight==i);                      
          u8g2.drawBox(0, headermargin+ (i+1)*menuitemboxy, screenwidth, menuitemboxy);
          u8g2.setDrawColor(2);
          u8g2.drawStr(menuitemboxxtextoffset, headermargin+ (i+1)*menuitemboxy + menuitemboxytextoffset, passedmenu[menustate-menuhighlight+i]);
        }
      }
      else if (menuhighlight == localmenurowcount-1){                      //at the bottom, highlight = highlight -1     
        menuhighlight = menuhighlight - 1;
        prevmenustate = menustate;
        for(int i = 0;i<localmenurowcount;i++){
          u8g2.setDrawColor(menuhighlight==i);
          u8g2.drawBox(0, headermargin+ (i+1)*menuitemboxy, screenwidth, menuitemboxy);
          u8g2.setDrawColor(2);
          u8g2.drawStr(menuitemboxxtextoffset, headermargin+ (i+1)*menuitemboxy + menuitemboxytextoffset, passedmenu[(menustate-menuhighlight+i)]);
        }       
      }
      else{             
        Serial.println("Found a bug");
      }
   u8g2.sendBuffer();             // transfer internal memory to the display    
   }
   
   else if(menustate == prevmenustate){                            //not scrolling
      if(rendermainagain){
        menuhighlight = 0;
        prevmenustate = menustate; 
        for(int i = 0;i<localmenurowcount;i++){
          u8g2.setDrawColor(menuhighlight==i); 
          u8g2.drawBox(0, headermargin+ (i+1)*menuitemboxy, screenwidth, menuitemboxy);
          u8g2.setDrawColor(2);
          u8g2.drawStr(menuitemboxxtextoffset, headermargin+ (i+1)*menuitemboxy + menuitemboxytextoffset, passedmenu[menustate-menuhighlight+i]);
        } 
      u8g2.sendBuffer();          // transfer internal memory to the display    
      rendermainagain = false;
      }
  }
  else{
    Serial.println("Found a bug");
  }
}

void resetMemory(){
    uint32_t ints5 = save_and_disable_interrupts();
    //flash_range_erase should write 0xFF (all ones)
    flash_range_erase((PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE*204), FLASH_SECTOR_SIZE*204);
    restore_interrupts (ints5); 
}

//200 is the convention for the memory offset, that is, the location, where contacts are stored. The last 200 sectors of memory are used for contacts.
//Sectors -2 and -3 are used conventionally for the users own private/public key.
int firstEmptySector(){

  int first_empty_sector = -1;  //-1 = no empty sectors
  for(int sector = 0; sector < 200; sector++)
  {
      if(isSectorEmpty(sector)){              
        first_empty_sector = sector;          
        break;
      }
  }
  return first_empty_sector;
}

//200 is the convention for the memory offset, that is, the location, where contacts are stored. The last 200 sectors of memory are used for contacts.
//Sectors -2 and -3 are used conventionally for the users own private/public key.
bool isSectorEmpty(int sector){
  boolean returnval = false;
  int flash_target_offset_2 = (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE*200)+(FLASH_SECTOR_SIZE*sector);
  int *p, page, addr, lastval, newval;
  int page_with_contents = -1;          
  //Loop over all pages in this sector and check if they are all empty.
  for(page = 0; page < FLASH_SECTOR_SIZE/FLASH_PAGE_SIZE; page++)
  {
      Serial.println("Checking page "+String(page));
      addr = XIP_BASE + flash_target_offset_2 + (page * FLASH_PAGE_SIZE);
      p = (int *)addr;              //place an int pointer at our memory-mapped address
      //0xFFFFFFFF cast as an int is -1 so this is how we detect an empty page
      if( *p != -1){
        page_with_contents = page;  //we found something. Page is not empty.
        break;
      }
  }
  if(page_with_contents > -1){
    //found at least one page with contents. So this complete sector is not empty. 
    returnval = false;
  }
  else{
    //did not find at least one page with contents. So this sector is empty.
    returnval = true;
  }
  return returnval;
}


void eraseSector(int sector){  
  //FLASH_PAGE_SIZE is the minimum size you can write. Is 256 bytes.
  //200 is the convention for the memory offset, that is, the location, where contacts are stored. The last 200 sectors of memory are used for contacts.
  int flash_target_offset = (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE*200) + FLASH_SECTOR_SIZE*sector + FLASH_PAGE_SIZE;
  uint32_t ints5 = save_and_disable_interrupts();
  flash_range_erase(flash_target_offset, FLASH_SECTOR_SIZE);
  restore_interrupts (ints5); 
}


void saveKey(uint8_t data[], int targetsize, int sector, int page, char nameinputbuffer[], int nameinputbufferpos){
  //https://www.makermatrix.com/blog/read-and-write-data-with-the-pi-pico-onboard-flash/
  //Note that when I called flash_range_program(), I cast the page buffer into (uint8_t *), as the function expects a pointer that increments on one byte boundaries
  //flash_range_program programs a multiple of 256 (FLASH_PAGE_SIZE) bytes.
  //200 is the convention for the memory offset, that is, the location, where contacts are stored. The last 200 sectors of memory are used for contacts.

  int flash_target_offset = (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE*200) + FLASH_SECTOR_SIZE*sector + FLASH_PAGE_SIZE*page;
  int flash_target_offset2 = (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE*200) + FLASH_SECTOR_SIZE*sector + FLASH_PAGE_SIZE*10;        //Page 10 is conventional offset for the name of the key.
  int flash_target_offset3 = (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE*200) + FLASH_SECTOR_SIZE*sector + FLASH_PAGE_SIZE*11;        //Page 11 is conventional offset for the length of the name of the key.
  
  uint8_t *p2, page2, addr2;
  addr2 = XIP_BASE + flash_target_offset;
  p2 = (uint8_t *)addr2;

  //write is 256 bytes minium.
  int buf[FLASH_PAGE_SIZE/sizeof(int)];  //One page buffer of ints, 64 ints in a buf. 4 bytes per int is 256 bytes total.

  for(int i = 0; i < targetsize; i++){            
    buf[i] = data[i];
  }

  int buf2[FLASH_PAGE_SIZE/sizeof(int)];  //One page buffer of ints, 64 ints in a buf. 4 bytes per int is 256 bytes total.

  for(int i = 0; i <= nameinputbufferpos; i++){
    buf2[i] = nameinputbuffer[i];
  }

  //length of name
  int buf3[FLASH_PAGE_SIZE/sizeof(int)];  //One page buffer of ints, 64 ints in a buf.4 bytes per int is 256 bytes total.
  *buf3 = nameinputbufferpos;
  
  //erase before writing the key
  uint32_t ints5 = save_and_disable_interrupts();
  flash_range_erase(flash_target_offset, FLASH_SECTOR_SIZE);
  restore_interrupts (ints5);

  //saving the key
  uint32_t ints2 = save_and_disable_interrupts();
  flash_range_program(flash_target_offset, (uint8_t *)buf, FLASH_PAGE_SIZE);
  restore_interrupts (ints2);

  //saving the name
  uint32_t ints3 = save_and_disable_interrupts();
  flash_range_program(flash_target_offset2, (uint8_t *)buf2, FLASH_PAGE_SIZE);
  restore_interrupts (ints3);

  //saving the length of the name
  uint32_t ints4 = save_and_disable_interrupts();
  flash_range_program(flash_target_offset3, (uint8_t *)buf3, FLASH_PAGE_SIZE);
  restore_interrupts (ints3);
}

void readKey(uint8_t *targetbuffer, int readamount, int sector, int page){
  //address from where we will read the key
  int flash_target_offset = (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE*200) + FLASH_SECTOR_SIZE*sector + FLASH_PAGE_SIZE*page;

  int *p2, page2, addr2;
  addr2 = XIP_BASE + flash_target_offset;
  //addr is an int. since this is a 32 bit architecture, increasing the address by 1 will read next first byte from 32 bits further up.
  //https://www.makermatrix.com/blog/read-and-write-data-with-the-pi-pico-onboard-flash/
  p2 = (int *)addr2;
  for(int i = 0;i<readamount;i++){
    targetbuffer[i] = *p2;
    *p2++;
  }
  //Serial.println("Found array in HEX readkey");
  //printNarray(targetbuffer, 32);
}

void generateKeypair(){
    static uint8_t mypub_k[32];
    static uint8_t mypriv_f[32];
    
    //Keep stirring random bytes from the ring oscilator (rosc) into the RNG until we have 32 good random numbers available.
    //Once done, we generate the keypair with Curve25519::dh1(mypub_k, mypriv_f);
    //We can get good entropy and good random numbers from our ring oscillator because we can afford to wait and don't need large amounts of random numbers.
    while(!RNG.available(32)){
      uint8_t rosc_single_byte2;
      uint8_t * rosc_byte_byte_pointer2 = &rosc_single_byte2;  
      rosc_single_byte(rosc_byte_byte_pointer2);   
      //1 bit entropy credit per single byte, very conservative.    
      RNG.stir(rosc_byte_byte_pointer2, 1, 1);
    }
    Curve25519::dh1(mypub_k, mypriv_f);

    saveKey(mypub_k, 32,-2,0, "mypubkey",8);      //sector -2, page 0, under name "mypubkey", which is a name of length 8. The convention is to save the personal keypair in this memory location.
    saveKey(mypriv_f, 32, -3, 0, "myprivkey",9);  //sector -3, page 0, under name "myprivkey", which is a name of length 9. The convention is to save the personal keypair in this memory location.
    //Serial.println("my pub key is");
    //printNarray(mypub_k, 32);
}

//read passedlength bytes into targetbuffer
bool readCamera(int passedlength, char *targetbuffer){
  bool camerainterrupted = false;
  delay(300); //to prevent interruptions
  //uart deinit/setup required?
  uart_deinit(UART0_ID);
  setup_uart();
  char c;
  for(int i = 0;i<passedlength;i++){
    if(uart_is_readable(UART0_ID)){
      c = uart_getc(UART0_ID);    
      targetbuffer[i]=c;
      //Serial.print(c);
    }
    else{
      //Serial.println("UART NOT READABLE");
      i--;
    }
    camerainterrupted = returnAnyButton();  //0 if no reading, 1 if reading
    if(camerainterrupted){
      break;
    }
  }
  uart_deinit(UART0_ID);
  delay(300);
  return camerainterrupted;
}

//mode and errorcorrection are options from https://github.com/ricmoo/QRCode
//together they determine the number of characters/bytes the QR code can contain
void displayBufferAsQR(char *buf2, int count, int mode, int errorcorrection, int pixelsize, int margin,int xpos, int ypos){
  QRCode qrcode;
  //here you need to correspond with the correct mode you are using below. H
  uint8_t qrcodeData[qrcode_getBufferSize(mode)];
  char memqr[count];
  for(int i = 0; i<count;i++){
    memqr[i] = buf2[i];
  }
  Serial.println("Initializing qrcode with text of strlen " + String(strlen(memqr)) + " " + String(strlen(buf2)));
  qrcode_initText(&qrcode, qrcodeData, mode, errorcorrection, memqr);
  Serial.println("Initializedqrcode with text");
  u8g2.clearBuffer();
  drawQR(qrcode,pixelsize,margin,xpos,ypos);
  u8g2.sendBuffer();
  awaitAnyButton();
}

//loop until any button is pressed
void awaitAnyButton(){
  boolean continueloop = true;
  while(continueloop){
    for (int row=0; row<5; row++){  
      pinMode(rowPins[row], OUTPUT);      //set the row pin as an output
      digitalWrite(rowPins[row], LOW);    //pull the row pins low
      for (int col=0; col<10; col++)
      {
        int reading = digitalRead(columnPins[col]);
        if (!reading){                    //active low
             continueloop = false;        //break
        }
      }
      pinMode(rowPins[row], INPUT);       //reset the row pin as an input
      digitalWrite(rowPins[row], HIGH);   //pull the row pin high
    }
  }
  rendermainagain = true;
}

//used for detecting if any button was pressed.
bool returnAnyButton() {
  bool buttonpressed = false;
  for (int row=0; row<5; row++){  
      pinMode(rowPins[row], OUTPUT);    //set the row pin as an output
      digitalWrite(rowPins[row], LOW);  //pull the row pins low
      for (int col=0; col<10; col++) {
        int reading = digitalRead(columnPins[col]);
        if (!reading) {                 //active low
             buttonpressed = true;
        }
      }
      pinMode(rowPins[row], INPUT);     //reset the row pin as an input
      digitalWrite(rowPins[row], HIGH); //pull the row pin high
  }
  return buttonpressed;
}

void displayMessage(char *buf2, int x, int y){
  u8g2.clearBuffer();
  u8g2.setFontMode(1);                  //activate transparent font mode
  u8g2.setDrawColor(1);                 //color 1 for the box
  u8g2.drawBox(0, 0, screenwidth, 64);
  u8g2.setDrawColor(0);
  u8g2.drawStr(x, y, buf2);
  u8g2.sendBuffer();
}

void drawQR(QRCode qrcode, int passedsize, int margin, int xpos, int ypos){
    u8g2.setDrawColor(1);                                                                               //for black margin    
    u8g2.drawBox(xpos,ypos,margin,qrcode.size*passedsize+margin);                                       //left margin
    u8g2.drawBox(xpos+margin,ypos,qrcode.size*passedsize+margin,margin);                                //upper margin
    u8g2.drawBox(xpos+qrcode.size*passedsize+margin,ypos+margin,margin,qrcode.size*passedsize+margin);  //right margin
    u8g2.drawBox(xpos,ypos+qrcode.size*passedsize+margin,qrcode.size*passedsize+margin,margin);         //bottom margin

    u8g2.setDrawColor(1);
    for (uint8_t y = 0; y < qrcode.size; y++) {
        for (uint8_t x = 0; x < qrcode.size; x++) {
            //black on white
            qrcode_getModule(&qrcode, x, y) ? u8g2.drawBox(0,0,0,0) : u8g2.drawBox(xpos+x*passedsize+margin,ypos+y*passedsize+margin,passedsize,passedsize);
        }
    }
}

void sendKeyPress(int row, int col, char* buf2, int* buf2pos, int* prevbuf2pos){
      if(row == 2 && col == 9){
          //found backspace
          buf2[*buf2pos]=' ';
          if(*buf2pos <= 0){     
            *buf2pos=0;
          }
          else{
            --*buf2pos;
            --*prevbuf2pos;
          }
        globalbackspace = true;
      }
      else{
        if(digitalRead(21)==0){
          buf2[*buf2pos]=keyboarduppercase[row][col];
          ++*buf2pos;
        }
        else{
          buf2[*buf2pos]=keyboardlowercase[row][col];
          ++*buf2pos;
        }
      }
}
