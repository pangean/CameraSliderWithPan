/*
SliderCam v0.2 - Rob Taylor (@robtaylorcase) June 2014 GPL 3.0

 IMPROVEMENTS AND CONSIDERATIONS TOWARDS V1.0:
 1) Efficiency of submenu button response code for first three menu headers
 2) Use of bulb shutter time as an extra menu option, passed to shutterDuration int
 3) shutter duration should be timed pause, not delay() - can't poll stop button!
 4) Use EEPROM library functions to save quantities, can thus simplify the motion control section and use Reset as "stop"
 5) Remove switch from "Go" submenu, replace with more appropriate logic statement
 6) Would it be better to time camera steps rather than total travel? "duration" being more like 15 sec or 2 min than 30 min or 4hrs?
 7) Any const ints that would be better as #define or ints better as boolean? Hardly running against the limits of SRAM space at 8kB, though.
 8) Tweening/easing for acceleration curves, particularly for video use
 9) Error check for zero step size, or simply add one to intervalDistance if value is zero before calculations- other end of Distance is still 1 step
 10) Would sub-16ms delay()s be better as delayMicroseconds()? How much do interrupts throw off timing?
 11) Use of sleep on A4988 to reduce power consumption in the field?
 12) Error check for currentDurationInt <= currentStepsInt*shutterDuration, allowing no time for movement or even negative pulseDelay!
 */

// include the library code:
#include <Wire.h>
#include <utility/Adafruit_MCP23017.h>
#include <Adafruit_RGBLCDShield.h>
#include <MsTimer2.h>  

// The shield uses the I2C SCL and SDA pins. On classic Arduinos
// this is Analog 4 and 5 so you can't use those for analogRead() anymore
// However, you can connect other I2C sensors to the I2C bus and share
// the I2C bus.
Adafruit_RGBLCDShield lcd = Adafruit_RGBLCDShield();
// These #defines make it easy to set the backlight color
#define RED 0x1
#define YELLOW 0x3
#define GREEN 0x2
#define TEAL 0x6
#define BLUE 0x4
#define VIOLET 0x5
#define WHITE 0x7


//define stepper driver pins
const int stp = 11;    //can't use pin 10 with the SS LCD as it's the backlight control.
//if it goes low, backlight turns off!
const int dir = 12;

//define trigger pin
const int trig = 13;

const int MAX_TRAVEL_DISTANCE = 1300; // in mm. needs to be customized for each rig

//define Microstep pins
const int MS1 = 5;
const int MS2 = 6;
const int MS3 = 7;

//BUTTONS
//define button values
const int btnUp = 0;
const int btnDn = 1;
const int btnL = 2;
const int btnR = 3;
const int btnSel = 4;
const int btnNone = 5;

const int MENU_ITEM_DISTANCE  = 0;
const int MENU_ITEM_DURATION  = 1;
const int MENU_ITEM_STEPS     = 2;
const int MENU_ITEM_BULB      = 3;
const int MENU_ITEM_DIRECTION = 4;
const int MENU_ITEM_GO        = 5;

const int NUM_DISTANCE_DIGITS = 4;
const int NUM_DURATION_DIGITS = 6;
const int NUM_STEPS_DIGITS    = 4;
const int NUM_BULB_DIGITS     = 5;


//define button-reading variables
int prevButton = 1;
const int DEBOUNCE_TIME = 10;

int getButtonfromRawData() {
  uint8_t buttons = lcd.readButtons();
  int adcIn;
  adcIn = analogRead(0); //read value from pin A0

    //Serial.println(adcIn);

    Serial.println(buttons);
/*threshold values confirmed by experimentation with button calibration sketch returning the following ADC read values:
     right: 2
     up: 8
     down: 4
     left: 16
     select: 1
     */
  if (buttons & 0) return btnNone;
  if (buttons & BUTTON_RIGHT) return btnR;
  if (buttons & BUTTON_UP) return btnUp;
  if (buttons & BUTTON_DOWN) return btnDn;
  if (buttons & BUTTON_LEFT) return btnL;
  if (buttons & BUTTON_SELECT) return btnSel;
  return btnNone; //if it can't detect anything, return no button pressed */
}

int readLcdButtons() {

  if (getButtonfromRawData() == prevButton) {
    return prevButton;
  } else {
    // Button has changed so wait for a fixed amount of time, read again and return that value
    delay(DEBOUNCE_TIME);
    return (prevButton = getButtonfromRawData());
  }
}

// state values for key-click state machine
// basically a key-click is registered after user releases the button
const int up_state = 0;
const int timer_done_state = 1;
const int wait_for_up_state = 2;

int getKeyClick() {

  int debounce_fsm_state_var;
  int btnVal, newbtnVal;
  
  debounce_fsm_state_var = up_state;
  while (true) {
    switch (debounce_fsm_state_var) {
      case up_state:
        btnVal = readLcdButtons();
        if (btnVal != btnNone) {
          delay(DEBOUNCE_TIME);
          debounce_fsm_state_var = timer_done_state;
        }
        continue;
      case timer_done_state:
        newbtnVal = readLcdButtons();
        if (newbtnVal == btnVal) {
          debounce_fsm_state_var = wait_for_up_state;
        } else {
          debounce_fsm_state_var = up_state;
        }
        continue;
      case wait_for_up_state:
        newbtnVal = readLcdButtons();
        if (newbtnVal == btnNone) {
          return btnVal;
        } else if (newbtnVal != btnVal) {
          debounce_fsm_state_var = up_state;
        }
        continue;
    }
  }
}

//MENU GUI
//define top-level menu item strings for numerical navigation
char* menuItemsTop[] = {
  " 01 Distance >", "< 02 Travel Time >", "< 03 Steps > ","< 04 Bulb > ", "< 05 Direction >", "<06 Select to Go"
};

int currentMenuItem = 0;       //x-axis position of menu selection
int currentCursorPos = 0;      //current lcd cursor position
int currentDistance[4] = {
  0, 1, 0, 0
};
int currentDuration[6] = {
  0, 0, 0, 0, 1, 0
};
int currentSteps[4] = {
  0, 0, 0, 0
};

int currentBulb[5] = {
  0, 0, 0, 0, 0
};

int LCDLine2Value[6] = {0, 0, 0, 0, 0, 0};

//MENU FUNCTIONALITY
int  currentChar = 0;        //global declarations of array-parsing variables
long currentDistanceInt = 0100;
long currentDurationInt = 000010;
long currentStepsInt = 0000; // Zero steps means video mode
long currentBulbInt = 00000; // Zero use camera shutter speed.
int travelDir = 0;

int adjustDigit(int x, int dir) {     //digit adjust function
  if (dir == 0 && x > 0) x--;         //subtract from digit on btnDn
  if (dir == 1 && x < 9) x++;         // add to digit on btnUp
  lcd.setCursor(currentCursorPos, 1);
  lcd.print(x);
  currentChar = x;
  return currentChar;                 //return new digit
}

int getIntFromDigits(int *digits, int num_digits) {
  int i;
  int value = 0;
  int power = 1;
  for (i = num_digits - 1; i >= 0; i--) {
    value += power * digits[i];
    power *= 10;
  }
  return value;
}

//MOTION CONTROL
double totalMotorSteps = 0;
double pulseDelay = 0;
int intervalDistance = 0;           //number of motor steps contained within a camera step
volatile int currentStep = 0;        //number of motor steps thus far
int motion = 0;             // motion = 1 if stop hasn't been pressed
int shutterDuration = 2;   //length of time for the camera to stop at shot steps in seconds

int dotSteps = 16; // number of dots on LCD
int dotDistance;

void debugPrint(char *str, long val) {
  lcd.clear();
  lcd.setCursor(0, 1);
  lcd.print(str);
  lcd.print(val);
  delay(1000);
}

const int DESIRED_PULSE_DELAY = 1; // we can (to some extent) adjust microsteps to get Pulse Delay to a desirable value

// For the Pololu A4988 Stepper Motor Driver

const int micro_stepping_value[5] = {0b000, 0b100, 0b010, 0b110, 0b111};
long pulse_delay_values[5];
long motor_step_values[5];


//NOTE: returns the INDEX of the best pulse_delay_value
int calcPulseDelay() {
  int index;
  long num_steps_per_tick = 1;
  long debug_val;
  for (index = 0; index < 5; index++) {
    motor_step_values[index] = currentDistanceInt * 5 * num_steps_per_tick;
    pulse_delay_values[index] = (1000L * currentDurationInt) / motor_step_values[index];
    num_steps_per_tick *= 2;
  }

  // find the one that is just greater than or equal to desired value of pulse delay;
  for (index = 4; index >= 0 ; index--) {
    if (pulse_delay_values[index] >= DESIRED_PULSE_DELAY) return index;
  }
  return 4;
}

void setMicroStep(int i) {
  digitalWrite(MS1, micro_stepping_value[i] & 0b1);
  digitalWrite(MS2, micro_stepping_value[i] & 0b10);
  digitalWrite(MS3, micro_stepping_value[i] & 0b100);
}


void stepperDriveUsingTimer() {
  digitalWrite(stp, HIGH);
  digitalWrite(stp, LOW);
  currentStep++;
}


void motionControl() {

  int index;

  // If the number of steps is zero implies video mode (i.e. does not pause for shooting)
  currentStep = 0;

  //totalMotorSteps = currentDistanceInt * 5; //calculate total steps (0.2mm = 20-tooth gear on 2mm pitch belt; 40mm per rev, 200 steps per rev, ergo 1/5th mm per step)
  //pulseDelay = (1000L * currentDurationInt) / totalMotorSteps; //how long to pause in ms between STP pulses to the motor driver -- does not include shutter stop time

  index = calcPulseDelay();
  pulseDelay = pulse_delay_values[index];
  totalMotorSteps = motor_step_values[index];
  setMicroStep(index);
  
  // debugPrint("in ", index); 
  // debugPrint("pd ", pulseDelay);
  // debugPrint("tm ", totalMotorSteps);
  
  if (pulseDelay < 1) pulseDelay = 1; // Can't set timer to zero ms
   
  if (currentStepsInt > 0)  {
    intervalDistance = totalMotorSteps / currentStepsInt;
  }

  // Needed to print progress dot on LCD screen
  dotDistance = totalMotorSteps / dotSteps;

  //once per overall run
  if (travelDir == 0) digitalWrite(dir, LOW);
  else if (travelDir == 1) digitalWrite(dir, HIGH);
  //Serial.begin(9600);
  //Serial.println(pulseDelay);
  lcd.setCursor(0, 1);

  MsTimer2::set(pulseDelay, stepperDriveUsingTimer);
  MsTimer2::start();

  int prevStep = 0;

  //step loop
  do {

    //Note: Stepper is driven via Timer interrupts and CurrentStep is incremented in the Timer Interrupt routine. 
    //      This loop just checks for when to stop (user, task done, stop for photo etc.).

    // Check for user stop -- notice doesn't check for key click just whether select is down
    if (readLcdButtons() == btnSel) break; // exits loop, stops stepper and exits

    //Note: Check for Limit Switches can be added here

    //at end of each step
    if (currentStepsInt > 0 && currentStep % intervalDistance == 0) {    //if current number of motor steps is divisible by the number of motor steps in a camera step, fire the camera
      MsTimer2::stop();
      delay(20);
      digitalWrite(trig, HIGH); //trigger camera shutter
      delay(80);
      digitalWrite(trig, LOW);    //reset trigger pin
      delay((shutterDuration * 1000) - 80); //delay needs changing to timer so stop button can be polled
      MsTimer2::start();
    }

    if (currentStep != prevStep) { // If current step changed since the last time we checked
      prevStep = currentStep;
      //print progress dot on LCD Screen
      if (currentStep % dotDistance == 0) {
        lcd.print(".");
      }
    }
  }
  while (currentStep < totalMotorSteps);
  MsTimer2::stop();
} //end motion control

void processTopLevelMenu() {
   
  
 
  int btnVal;
  while (true) {
    btnVal = getKeyClick();
    switch (btnVal) {
      case  btnL:
        if (currentMenuItem == MENU_ITEM_DISTANCE) break;      //can't go left from here
        else currentMenuItem--;
        break;
      case  btnR:
        if (currentMenuItem == MENU_ITEM_GO) break;      //can't go right from here
        else  currentMenuItem++;
        break;
      case  btnSel:
        processSecondaryLevelMenu();
        break;
    } //end of switch
    refreshLCD();
  }
}

void processSecondaryLevelMenu() {
  currentCursorPos = 0;
  refreshLCD();
  lcd.blink();

  switch (currentMenuItem) {
    case MENU_ITEM_DISTANCE: //01 DISTANCE
      getDistance();
      break;
    case MENU_ITEM_DURATION:
      getDuration();
      break;
    case MENU_ITEM_STEPS:
      getSteps();
      break;
    case MENU_ITEM_BULB:
      getBulb();
      break;
    case MENU_ITEM_DIRECTION:
      getDirection();
      break;
    case MENU_ITEM_GO:
      getGo();
      break;
  }
  refreshLCD();
  lcd.noBlink();
}

void writeToLCDLine2(int* from, int num_digits) {
  int i;
  for (i = 0; i < num_digits; i++) {
    LCDLine2Value[i] = from[i];
  }
}

void readFromLCDLine2(int* to, int num_digits) {
  int i;
  for (i = 0; i < num_digits; i++) {
    to[i] = LCDLine2Value[i];
  }
}

void getUserInputValue(int numDigits) {
  int btnVal;
  lcd.setCursor(currentCursorPos, 1);
  while (true) { // Exits only when select is pressed
    btnVal = getKeyClick();
    switch (btnVal) {
      case btnUp:
        currentChar = LCDLine2Value[currentCursorPos];
        adjustDigit(currentChar, 1);
        LCDLine2Value[currentCursorPos] = currentChar;
        break;
      case btnDn:
        currentChar = LCDLine2Value[currentCursorPos];
        adjustDigit(currentChar, 0);
        LCDLine2Value[currentCursorPos] = currentChar;
        break;
      case btnL:
        if (currentCursorPos == 0) break;      //can't go left from here
        else currentCursorPos--;
        break;
      case btnR:
        if (currentCursorPos == numDigits - 1) break;      //can't go right from here
        else currentCursorPos++;
        break;
      case btnSel:
        return;
    }    //end switch
    lcd.setCursor(currentCursorPos, 1);
  }
}

void getDistance() { // MENU_ITEM_DISTANCE
  getUserInputValue(NUM_DISTANCE_DIGITS); //Digits 0 throught 3
  readFromLCDLine2(currentDistance, NUM_DISTANCE_DIGITS);
  currentDistanceInt =  getIntFromDigits(currentDistance, NUM_DISTANCE_DIGITS);
  if (currentDistanceInt > MAX_TRAVEL_DISTANCE) currentDistanceInt = MAX_TRAVEL_DISTANCE;
}

void getDuration() { // MENU_ITEM_DURATION
  getUserInputValue(NUM_DURATION_DIGITS); // Digits 0 through 5
  readFromLCDLine2(currentDuration, NUM_DURATION_DIGITS);
  currentDurationInt =  getIntFromDigits(currentDuration, NUM_DURATION_DIGITS);
}

void getSteps() { // MENU_ITEM_STEPS
  getUserInputValue(NUM_STEPS_DIGITS); // Digits 0 through 3
  readFromLCDLine2(currentSteps, NUM_STEPS_DIGITS);
  currentStepsInt =  getIntFromDigits(currentSteps, NUM_STEPS_DIGITS);
}

void getBulb() { // MENU_ITEM_BULB
  getUserInputValue(NUM_BULB_DIGITS); // Digits 0 through 5
  readFromLCDLine2(currentSteps, NUM_BULB_DIGITS);
  currentBulbInt =  getIntFromDigits(currentSteps, NUM_BULB_DIGITS);
}

void getDirection() { // MENU_ITEM_DIRECTION
  int btnVal;
  while (true) {
    btnVal = getKeyClick();
    switch (btnVal) { // left right button clicks are ignored
      case btnUp:
        travelDir = 1;
        break;
      case btnDn:
        travelDir = 0;
        break;
      case btnSel:
        return;
    }  //end switch
    refreshLCD();
  }
}

void getGo() { // MENU_ITEM_GO
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("On:Select to Stp");
  motion = 1;
  motionControl();
  delay(500);
}

void refreshLCD() { // Only called from the Top Level Menu. Secondary Menu only changes blinking and individual digits
  //PRINT NEW SCREEN VALUES
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print(menuItemsTop[currentMenuItem]);    //print top level menu item

  lcd.setCursor(0, 1);
  switch (currentMenuItem) {
    case MENU_ITEM_DISTANCE:
      writeToLCDLine2(currentDistance, NUM_DISTANCE_DIGITS);
      for (int i = 0; i < NUM_DISTANCE_DIGITS; i++) {
        lcd.setCursor(i, 1);
        lcd.print(LCDLine2Value[i]);
      }
      lcd.setCursor(NUM_DISTANCE_DIGITS, 1);
      lcd.print("mm(max 1300)");    //insert max carriage travel on slider used
      break;
    case MENU_ITEM_DURATION:
      writeToLCDLine2(currentDuration, NUM_DURATION_DIGITS);
      for (int i = 0; i < NUM_DURATION_DIGITS; i++) {
        lcd.setCursor(i, 1);
        lcd.print(LCDLine2Value[i]);
      }
      lcd.setCursor(NUM_DURATION_DIGITS, 1);
      lcd.print("s(3600/hr)");
      break;
    case MENU_ITEM_STEPS:
      writeToLCDLine2(currentSteps, NUM_STEPS_DIGITS);
      for (int i = 0; i < NUM_STEPS_DIGITS; i++) {
        lcd.setCursor(i, 1);
        lcd.print(LCDLine2Value[i]);
      }
      break;
    case MENU_ITEM_BULB:
      writeToLCDLine2(currentSteps, NUM_BULB_DIGITS);
      for (int i = 0; i < NUM_BULB_DIGITS; i++) {
        lcd.setCursor(i, 1);
        lcd.print(LCDLine2Value[i]);
      }
      lcd.setCursor(NUM_BULB_DIGITS, 1);
      lcd.print("s(3600/hr)");
      break;
    case MENU_ITEM_DIRECTION:
      if (travelDir == 0) lcd.print("From Motor");
      else lcd.print("To Motor");
      break;
    case MENU_ITEM_GO:
      lcd.print("Stopped");
      break;
  }  //end switch

  lcd.setCursor(currentCursorPos, 1);
}

void setup() {

  // Debugging output
  Serial.begin(9600);
  lcd.begin(16, 2);               // initialise LCD lib full-screen
  lcd.setCursor(0, 0);            // set cursor position
  lcd.setBacklight(WHITE);
  pinMode(A0, INPUT);

  // Microstep Pins
  pinMode(MS1, OUTPUT);
  pinMode(MS2, OUTPUT);
  pinMode(MS3, OUTPUT);

  pinMode(stp, OUTPUT);           //initialise stepper pins
  pinMode(dir, OUTPUT);

  pinMode(trig, OUTPUT);           //initialise trigger pin
  digitalWrite(trig, LOW);         //ensure trigger is turned off

  lcd.print("Welcome to");  //welcome screen
  lcd.setCursor(0, 1);
  lcd.print("SliderCam v1.0!");
  delay(1000);
  currentMenuItem = MENU_ITEM_DISTANCE;
  refreshLCD();
}

//MAIN LOOP
void loop() {
  processTopLevelMenu();
} //END OF PROGRAM

