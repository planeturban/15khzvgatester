/*
 VGA colour video generation
 
 Author:   Nick Gammon
 Date:     22nd April 2012
 Version:  1.0
 
 Version 1.0: initial release

 Connections:
 
 D3 : Horizontal Sync (68 ohms in series) --> Pin 13 on DB15 socket
 D4 : Red pixel output (470 ohms in series) --> Pin 1 on DB15 socket
 D5 : Green pixel output (470 ohms in series) --> Pin 2 on DB15 socket
 D6 : Blue pixel output (470 ohms in series) --> Pin 3 on DB15 socket
 D10 : Vertical Sync (68 ohms in series) --> Pin 14 on DB15 socket
 
 Gnd : --> Pins 5, 6, 7, 8, 10 on DB15 socket


 Note: As written, this sketch has 34 bytes of free SRAM memory.
 
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
#include <avr/sleep.h>

const byte hSyncPin = 3;     // <------- HSYNC

const byte redPin = 4;       // <------- Red pixel data
const byte greenPin = 5;     // <------- Green pixel data
const byte bluePin = 6;      // <------- Blue pixel data

const byte vSyncPin = 10;    // <------- VSYNC

const int horizontalBytes = 120;  // 480 pixels wide
const int verticalPixels = 240;  // 480 pixels high

// Timer 1 - Vertical sync

// output    OC1B   pin 16  (D10) <------- VSYNC

//   Period: 16.64 mS (60 Hz)
//      1/60 * 1e6 = 16666.66 uS
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

// However in practice, it we can only pump out pixels at 375 nS each because it
//  takes 6 clock cycles to read one in from RAM and send it out the port.

const int verticalLines = verticalPixels / 16;  
const int horizontalPixels = horizontalBytes * 8;

const byte verticalBackPorchLines = 35;  // includes sync pulse?
const int verticalFrontPorchLines = 525 - verticalBackPorchLines;

volatile int vLine;
volatile int messageLine;
volatile byte backPorchLinesToGo;

#define nop asm volatile ("nop\n\t")

// bitmap - gets sent to PORTD
// For D4/D5/D6 bits need to be shifted left 4 bits
//  ie. 00BGR0000

char message [verticalLines]  [horizontalBytes];

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


void setup()
  {
  
  // initial bitmap ... change to suit
  for (int y = 0; y < verticalLines; y++)
    for (int x = 0; x < horizontalBytes; x++)
      message [y] [x] = (x + y) << 4;
   
  // disable Timer 0
  TIMSK0 = 0;  // no interrupts on Timer 0
  OCR0A = 0;   // and turn it off
  OCR0B = 0;
  
  // Timer 1 - vertical sync pulses
  pinMode (vSyncPin, OUTPUT); 
  Timer1::setMode (15, Timer1::PRESCALE_1024, Timer1::CLEAR_B_ON_COMPARE);
  OCR1A = 259;  // 16666 / 64 uS = 260 (less one)
  OCR1B = 0;    // 64 / 64 uS = 1 (less one)
  TIFR1 = bit (TOV1);   // clear overflow flag
  TIMSK1 = bit (TOIE1);  // interrupt on overflow on timer 1

  // Timer 2 - horizontal sync pulses
  pinMode (hSyncPin, OUTPUT); 
  Timer2::setMode (7, Timer2::PRESCALE_8, Timer2::CLEAR_B_ON_COMPARE);
  OCR2A = 127;   // 32 / 0.5 uS = 64 (less one)
  OCR2B = 7;    // 4 / 0.5 uS = 8 (less one)
  TIFR2 = bit (TOV2);   // clear overflow flag
  TIMSK2 = bit (TOIE2);  // interrupt on overflow on timer 2
 
  // prepare to sleep between horizontal sync pulses  
  set_sleep_mode (SLEEP_MODE_IDLE);  
  
  // pins for outputting the colour information
  pinMode (redPin, OUTPUT);
  pinMode (greenPin, OUTPUT);
  pinMode (bluePin, OUTPUT);
  
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
  register char * messagePtr =  & (message [messageLine] [0] );

  delayMicroseconds (1);
  
  // how many pixels to send
  register byte i = horizontalBytes;

  // blit pixel data to screen    
  while (i--)
    PORTD = * messagePtr++;

  // stretch final pixel
  nop; nop; nop;

  PORTD = 0;  // back to black
  // finished this line 
  vLine++;

  // every 16 pixels it is time to move to a new line in our text
  if ((vLine & 0xF) == 0)
    messageLine++;
    
  }  // end of doOneScanLine

void loop() 
  {
  // sleep to ensure we start up in a predictable way
  sleep_mode ();
  doOneScanLine ();
 }  // end of loop
