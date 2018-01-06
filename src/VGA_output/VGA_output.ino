/*
 VGA video generation
 
 Author:   Nick Gammon
 Date:     20th April 2012
 Version:  1.2
 
 Version 1.0: initial release
 Version 1.1: code cleanups
 Version 1.2: more cleanups, added clear screen (0x0C), added scrolling
 

 Connections:
 
 D1 : Pixel output (470 ohms in series to each one of R, G, B)   --> Pins 1, 2, 3 on DB15 socket
 D3 : Horizontal Sync (68 ohms in series) --> Pin 13 on DB15 socket
 D10 : Vertical Sync (68 ohms in series) --> Pin 14 on DB15 socket
 
 Gnd : --> Pins 5, 6, 7, 8, 10 on DB15 socket

 PERMISSION TO DISTRIBUTE
 
 Permission is hereby granted, free of charge, to any person obtaining a copy of this software 
 and associated documentation files (the "Software"), to deal in the Software without restriction, 
 including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, 
 and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, 
 subject to the following conditions:
 
 The above copyright notice and this permission notice shall be included in 
 all copies or substantial portions of the Software.
 
 
 LIMITATION OF LIABILITY
 
 The software is provided "as is", without warranty of any kind, express or implied, 
 including but not limited to the warranties of merchantability, fitness for a particular 
 purpose and noninfringement. In no event shall the authors or copyright holders be liable 
 for any claim, damages or other liability, whether in an action of contract, 
 tort or otherwise, arising from, out of or in connection with the software 
 or the use or other dealings in the software. 

*/

#include "TimerHelpers.h"
#include <avr/pgmspace.h>
#include "screenFont.h"
#include <avr/sleep.h>
#include <Wire.h>

#define BETA_ARDUINO ARDUINO < 100

const byte pixelPin = 1;     // <------- Pixel data
const byte hSyncPin = 3;     // <------- HSYNC
const byte MSPIM_SCK = 4;    // <-- we aren't using it directly
const byte vSyncPin = 10;    // <------- VSYNC

const int horizontalBytes = 44;  // 160 pixels wide
const int verticalPixels = 480;  // 480 pixels high

const byte i2cAddress = 42;

// Timer 1 - Vertical sync

// output    OC1B   pin 16  (D10) <------- VSYNC

//   Period: 16.64 mS (60 Hz)
//      1/60 * 1e6 = 16666.66 uS
//   Period: 20 mS (50 Hz)
//      1/50 * 1e6 = 20000 uS

//   Pulse for 64 uS  (2 x HSync width of 32 uS)
//    Sync pulse: 2 lines
//    Back porch: 33 lines
//    Active video: 480 lines
//    Front porch: 10 lines
//       Total: 525 lines

// Timer 2 - Horizontal sync

// output    OC2B   pin 5  (D3)   <------- HSYNC

//   Period: 32 uS (31.25 kHz)
//      (1/60) / 525 * 1e6 = 31.74 uS
//   Pulse for 4 uS (96 times 39.68 nS)
//    Sync pulse: 96 pixels
//    Back porch: 48 pixels
//    Active video: 640 pixels
//    Front porch: 16 pixels
//       Total: 800 pixels

// Pixel time =  ((1/60) / 525 * 1e9) / 800 = 39.68  nS
//  frequency =  1 / (((1/60) / 525 * 1e6) / 800) = 25.2 MHz

// However in practice, it is the SPI speed, namely a period of 125 nS
//     (that is 2 x system clock speed)
//   giving an 8 MHz pixel frequency. Thus the characters are about 3 times too wide.
// Thus we fit 160 of "our" pixels on the screen in what usually takes 3 x 160 = 480

const byte screenFontHeight = 8;
const byte screenFontWidth = 8;

const int verticalLines = verticalPixels / screenFontHeight / 2;  // double-height characters
const int horizontalPixels = horizontalBytes * screenFontWidth;

const byte verticalBackPorchLines = 35;  // includes sync pulse?
const byte verticalFrontPorchLines = 525 - verticalBackPorchLines;

volatile int vLine;
volatile int messageLine;
volatile byte backPorchLinesToGo;

enum SEND_COMMANDS { CLRSCR = 1, CLREOL, GOTOXY, ESC = 27 };
enum STATES { NORMAL, GOT_ESCAPE, GOT_GOTOXY, GOT_X };

char message [verticalLines]  [horizontalBytes];
byte column, line;
STATES state = NORMAL;
byte x, y;  // for gotoxy


// ISR: Vsync pulse
ISR (TIMER1_OVF_vect)
  {
  vLine = 0; 
  messageLine = 0;
  backPorchLinesToGo = verticalBackPorchLines;
  } // end of TIMER1_OVF_vect
  
// ISR: Hsync pulse ... this interrupt merely wakes us up
ISR (TIMER2_OVF_vect)
  {
  } // end of TIMER2_OVF_vect
    
// called by interrupt service routine when incoming data arrives

/*
Expected formats are:
   * ordinary text:           gets displayed
   * carriage-return (0x0D):  returns cursor to start of current line
   * newline (0x0A):          drops down a line and also goes to the start of the line
   * clear screen (0x0C):     clear screen, return cursor to 1,1
   * ESC (0x1B) followed by:
      * 1 : clear screen, return cursor to 1,1
      * 2 : clear to end of current line
      * 3 : go to x,y ... next two bytes are X and then Y: one-relative

  All writing wraps, eg. text wraps at end of line, then end of screen back to line 1, column 1.
  A gotoxy out of range is ignored.
*/

void receiveEvent (int howMany)
 {
  while (Wire.available () > 0)
  {
    byte c;
#if BETA_ARDUINO    
    c = Wire.receive ();
#else
    c = Wire.read ();
#endif 
    
    // first check state ... see if we are expecting a command or an x/y position
    switch (state)
      {
      // normal is, well, normal unless we get an ESC character
      case NORMAL:
          switch (c)
            {
            case ESC: 
              state = GOT_ESCAPE; 
              break;
 
            // otherwise just display the character
            default:
               message [line] [column] = c;
               if (++column >= horizontalBytes)
                 {
                 column = 0;
                 line++;
                 } // end wrapped line
                 
              if (line < verticalLines)
                  break;         
            // if wrapped past end of buffer, fall through to do a newline which will scroll up
                    
            // newline starts a new line, and drops down to do a carriage-return as well
            case '\n': 
              // end end? scroll
              if (++line >= verticalLines)
                {
                // move line 2 to line 1 and so on ...
                memmove (& message [0] [0], & message [1] [0], sizeof message - horizontalBytes);
                // clear last line
                memset (&message [verticalLines - 1] [0], ' ', horizontalBytes);    
                // put cursor on last line
                line = verticalLines - 1;    // back to last line          
                }
            // fall through ...
            
            // carriage-return returns to start of line
            case '\r': 
              column = 0; 
              break;
            
            // clear screen
            case '\f': 
              memset (message, ' ', sizeof message);
              line = column = 0;
              break;

             }  // end of switch on incoming character
          
          break;  // end of NORMAL
          
        // got ESC previously
        case GOT_ESCAPE:
          switch (c)
            {
            // clear screen ... just do it
            case CLRSCR:
              memset (message, ' ', sizeof message);
              line = column = 0;
              state = NORMAL;
              break;
              
            // clear to end of line
            case CLREOL:
              memset (&message [line] [column], ' ', horizontalBytes - column);
              state = NORMAL;
              break;

            // gotoxy expects two more bytes (x and y)
            case GOTOXY:
              state = GOT_GOTOXY;
              break;

            // unexpected ... not recognized command
            default:
              state = NORMAL;
              break;
            } // end of switch on command type
          break;  // end of GOT_ESCAPE
        
        // we got x, now we want y
        case GOT_GOTOXY:
          x = c - 1;  // make zero-relative
          state = GOT_X;
          break;
          
        // we now have x and y, we can move the cursor
        case GOT_X:
          y = c - 1;  // make zero-relative
          
          // if possible that is
          if (x < horizontalBytes && y < verticalLines)
            {
            column = x;
            line = y; 
            }
          state = NORMAL;
          break;
          
        // unexpected ... not recognized state
        default:
          state = NORMAL;
          break;
      } // end of switch on state
      
  }  // end of while available
}  // end of receiveEvent

void setup()
  {
  pinMode(A0, OUTPUT);
  digitalWrite(A0, HIGH);
  // initial message ... change to suit
  for (int i = 0; i < verticalLines; i++)
    sprintf (message [i], "Glad retrojul!");
//    sprintf (message [i], "Line %03i - hello!", i);
   
  // disable Timer 0
  TIMSK0 = 0;  // no interrupts on Timer 0
  OCR0A = 0;   // and turn it off
  OCR0B = 0;
  
  // Timer 1 - vertical sync pulses
  pinMode (vSyncPin, OUTPUT); 
  Timer1::setMode (15, Timer1::PRESCALE_1024, Timer1::CLEAR_B_ON_COMPARE);
  OCR1A = 259;  // 16666 / 64 uS = 260 (less one)
  OCR1B = 0;    // 64 / 64 uS = 1 (less one)
  TIFR1 = _BV (TOV1);   // clear overflow flag
  TIMSK1 = _BV (TOIE1);  // interrupt on overflow on timer 1

  // Timer 2 - horizontal sync pulses
  pinMode (hSyncPin, OUTPUT); 
  Timer2::setMode (7, Timer2::PRESCALE_8, Timer2::CLEAR_B_ON_COMPARE);
  OCR2A = 127;   // 32 / 0.5 uS = 64 (less one)
//  OCR2A = 64;   // 32 / 0.5 uS = 64 (less one)

  OCR2B = 7;    // 4 / 0.5 uS = 8 (less one)
  TIFR2 = _BV (TOV2);   // clear overflow flag
  TIMSK2 = _BV (TOIE2);  // interrupt on overflow on timer 2
 
  // Set up USART in SPI mode (MSPIM)
  
  // baud rate must be zero before enabling the transmitter
  UBRR0 = 0;  // USART Baud Rate Register
//  pinMode (MSPIM_SCK, OUTPUT);   // set XCK pin as output to enable master mode
  UCSR0B = 0; 
  UCSR0C = _BV (UMSEL00) | _BV (UMSEL01) | _BV (UCPHA0) | _BV (UCPOL0);  // Master SPI mode
  
  // prepare to sleep between horizontal sync pulses  
  set_sleep_mode (SLEEP_MODE_IDLE);  

  // for incoming data to display from I2C
//  Wire.begin (i2cAddress);
//  Wire.onReceive (receiveEvent);
 digitalWrite(A0, LOW); 
}  // end of setup

// draw a single scan line
void doOneScanLine ()
  {
    
  // after vsync we do the back porch
  if (backPorchLinesToGo)
    {
    backPorchLinesToGo--;
    return;   
    }  // end still doing back porch
    
  // if all lines done, do the front porch
  if (vLine >= verticalPixels)
    return;
    
  // pre-load pointer for speed
  const register byte * linePtr = &screen_font [ (vLine >> 1) & 0x07 ] [0];
  register char * messagePtr =  & (message [messageLine] [0] );

  // how many pixels to send
  register byte i = horizontalBytes;

  // turn transmitter on 
  UCSR0B = _BV (TXEN0);  // transmit enable (starts transmitting white)

  // blit pixel data to screen    
  while (i--)
    UDR0 = pgm_read_byte (linePtr + (* messagePtr++));

  // wait till done    
  while (!(UCSR0A & _BV(TXC0))) 
    {}
  
  // disable transmit
  UCSR0B = 0;   // drop back to black

  // finished this line 
  vLine++;

  // every 16 pixels it is time to move to a new line in our text
  //  (because we double up the characters vertically)
  if ((vLine & 0xF) == 0)
    messageLine++;
    
  }  // end of doOneScanLine

void loop() 
  {
  // sleep to ensure we start up in a predictable way
  sleep_mode ();
  doOneScanLine ();
 }  // end of loop

