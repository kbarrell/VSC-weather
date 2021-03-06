#include <Arduino.h>

/*******************************************************************************
 *   CURRENT STATE - REPLICATING Weather1.0 functionality with later MCCI library
 * Copyright (c) 2015 Thomas Telkamp and Matthijs Kooijman
 * Copyright (c) 2018 Terry Moore, MCCI
 * Copyright (c) 2018 Thomas Laurenson
 * Copyright (c) 2020 Kevin Barrell
 *
 * Permission is hereby granted, free of charge, to anyone
 * obtaining a copy of this document and accompanying files,
 * to do whatever they want with them without any restriction,
 * including, but not limited to, copying, modification and redistribution.
 * NO WARRANTY OF ANY KIND IS PROVIDED.
 * 
 * Sketch Summary:
 * This sketch expands on a multi-sensor weather station design originally published at http://cactus.io/projects/weather/arduino-weather-station
 * Rather than the Ethernet-based backhaul of the original, this current integration adopts LoRaWAN radio technology to connect the 
 * weather station to the web, via The Things Network https://www.thethingsnetwork.org.   
 * While generally following the hardware approach of the original cactus design, this sketch integrates the examples of sensor reading provided 
 * there with LoRa-based data transmission to the TTN, using the MCCI LMIC library, and specifically the ttn-abp-dragino-lorashield-au915 example 
 * from Thomas Laurenson; https://www.thomaslaurenson.com/blog/2018/07/21/dragino-lorashield-on-AU915-using-arduino-lmic-library/
 *
 * Target device: Dragino LoRa Shield (AU915) with Arduino Mega 2560
 * Target frequency: AU915 sub-band 2 (916.8 to 918.2 uplink)
 * Authentication mode: Activation by Personalisation (ABP)
 *
 * This example requires the following modification before upload:
 * 1) Enter a valid Network Session Key (NWKSKEY)
 *    For example: 0x07f319fc
 * 2) Enter a valid Application Session Key (APPSKEY)
 *    For example: { 0xd9, 0x54, 0xce, 0xbe, 0x9b, 0x5b, 0x76, 0x2d, 0x56, 0x26, 0xc9, 0x4d, 0x82, 0x22, 0xf3, 0xad };
 * 3) Enter a valid Device Address (DEVADDR)
 *    For example: { 0xe4, 0x07, 0xe3, 0x3b, 0xef, 0xf3, 0x80, 0x6c, 0x7c, 0x6e, 0x42, 0x43, 0x56, 0x7c, 0x22, 0x37 };
 * 
 * The NWKSKEY, APPSKEY and DEVADDR values should be obtained from your
 * LoRaWAN server (e.g., TTN or any private LoRa provider).
 *
 *******************************************************************************/

#include <lmic.h>			//  MCCI.  
#include <hal/hal.h>
#include <SPI.h>
#include <cactus_io_BME280_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#include "TimerOne.h"     // Timer Interrupt set to 2.5 sec for read sensors
#include <math.h>
#include <Wire.h>         // For accessing RTC
#include <SD2405RTC.h>    // For Gravity RTC breakout board.   Set RTC to UTC time
#include <TimeLib.h>      // For epoch time en/decode
#include <Timezone.h>	  // For AU Eastern STD/DST so that daily readings are 24hr to 9am (local)

// Sensor-related definitions
// Set hardware pin assignments & pre-set constants
#define TX_Pin 4 				   // used to indicate web data tx
#define ONE_WIRE_BUS_PIN 29 	  //Data bus pin for DS18B20's

#define WindSensor_Pin (18)       //The pin location of the anemometer sensor
#define WindVane_Pin  (A13)       // The pin connecting to the wind vane sensor
#define VaneOffset  0		   // The anemometer offset from magnetic north
#define Bucket_Size  0.2 	   // mm bucket capacity to trigger tip count
#define RG11_Pin  19        		 // Interrupt pin for rain sensor
#define BounceInterval  15		// Number of ms to allow for debouncing
#define SampleInt_Pin   3		// Interrupt pin for RTC-generated sampling clock (when used)

// Set timer related settings for sensor sampling & calculation
#define Timing_Clock  500000    //  0.5sec in millis
#define Sample_Interval   5		//  = number of Timing_Clock cycles  i.e. 2.5sec interval
#define Report_Interval   120    //  = number of sample intervals contributing to each upload report (each 5 min)
#define Speed_Conversion  1.4481   // convert rotations to km/h.  = 2.25/(Sample_Interval x Timing_Clock)* 1.609 
									// refer Davis anemometer technical spec
//#define TIMER_FROM_RTC 1		// Uncomment this line if timing clock for sampler drawn from RTC frequency interrupt
									
volatile bool isSampleRequired;    		// set true every Sample_Interval.   Get wind speed
volatile unsigned int timerCount;  		// used to determine when Sample_Interval is reached
volatile unsigned int sampleCount;		// used to determin when Report_Interval is reached
volatile unsigned long rotations;  		// cup rotation counter for wind speed calcs
volatile unsigned long contactBounceTime;  // Timer to avoid contact bounce in wind speed sensor
volatile float windSpeed, windGust;        // speed in km per hour

volatile unsigned long tipCount;  	 	// rain bucket tipcounter used in interrupt routine
volatile unsigned long contactTime; 	// timer to manage contact bounce in interrupt routine
volatile unsigned long obsRainfallCount; // total count of rainfall tips recorded in observatoin period (5min)
volatile float obsReportRainfallRate;    	// total amount of rainfall in the reporting period  (5 min)
volatile unsigned long dailyRainfallCount;	//  total count of rainfall tips in 24 hrs to 9am (local time)
const float reportIntervalSec = Report_Interval * Sample_Interval * float(Timing_Clock) / 1000000;

// Define structures for handling reporting via TTN
typedef struct obsSet {
	uint16_t 	windGustX10; // observed windgust speed (km/h) X10  ~range 0 -> 1200
	uint16_t	windGustDir; // observed wind direction of Gust (compass degrees)  0 -> 359
	uint16_t	tempX10;	// observed temp (??C) +100 x 10   ~range -200->600
	uint16_t	humidX10;	// observed relative humidty (%) x 10   range 0->1000
	uint16_t 	pressX10;	// observed barometric pressure at station level (hPa)  x 10  ~range 8700 -> 11000 
	uint16_t	rainflX10;	// observed accumulated rainfall (mm) x10   ~range 0->1200
	uint16_t	windspX10;	// observed windspeed (km/h) x10 ~range 0->1200
	uint16_t	windDir;	// observed wind direction (compass degrees)  range 0->359
	uint16_t	dailyRainX10; //  accumulated rainfall (mm) X10 for period to 9am daily
	uint16_t	casetempX10;		// station case temperature (for alarming)
 } obsSet;
		
union obsPayload
{
	obsSet	obsReport;
	uint8_t	readAccess[sizeof(obsSet)];
}sensorObs[2];

// AU Eastern Time Zone (Sydney, Melbourne)   Use next 3 lines for one time setup to be written to EEPROM
//TimeChangeRule auEDST = {"AEDT", First, Sun, Oct, 2, 660};    //Daylight time = UTC + 11:00 hours
//TimeChangeRule auESTD = {"AEST", First, Sun, Apr, 2, 600};     //Standard time = UTC + 10:00 hours
//Timezone auEastern(auEDST, auESTD);

// If TimeChangeRules are already stored in EEPROM, comment out the three
// lines above and uncomment the line below.
Timezone auEastern(100);       // assumes rules stored at EEPROM address 100 & that RTC set to UTC
TimeChangeRule *tcr;		// pointer to the timechange rule
time_t utc, localTime;
boolean	dailyTotalsDue;		// flags that totals for 24hr to 9am local are to be reported & reset

int  currentObs, reportObs;   //References which obsPayload [0 or 1]is being filled vs. reported 
int vaneValue;         	 	//  raw analog value from wind vane
int vaneDirection;          //  translated 0-360 direction
int calDirection, calGustDirn;     	//  converted value with offset applied
const int count = 4;				// average the last 4 wind directions
int boxcar[count];					// stack of wind direction values for averaging calculation
const bool BaseRange = true;
const bool ExtdRange = false;


// LoRaWAN NwkSKey, network session key
static const PROGMEM u1_t NWKSKEY[16] = { 0x1A, 0x71, 0xFD, 0x1C, 0xFC, 0x99, 0x53, 0x84, 0xE2, 0xCD, 0x7B, 0xEE, 0xBB, 0x7F, 0xE3, 0xF9 };

// LoRaWAN AppSKey, application session key
static const PROGMEM u1_t  APPSKEY[16] = { 0x14, 0xEE, 0x5D, 0xE6, 0x45, 0xDE, 0x42, 0xA1, 0xA7, 0xAA, 0xF9, 0xAF, 0x36, 0x94, 0x90, 0x6E };

//  Create BME280 object
BME280_I2C bme;     // I2C using address 0x77

// Setup a oneWire instance to communicate with OneWire devices
OneWire oneWire(ONE_WIRE_BUS_PIN);
DallasTemperature DSsensors(&oneWire);    // Pass the OneWire reference to Dallas Temperature lib

// Assign the addresses of the DS18B20 sensors (determined by reading them previously)
DeviceAddress airTempAddr = { 0x28, 0x1A, 0x30, 0x94, 0x3A, 0x19, 0x01, 0x55 };
DeviceAddress caseTempAddr = { 0x28, 0xAA, 0x68, 0x93, 0x41, 0x14, 0x01, 0xD8 };


// LoRaWAN end-device address (DevAddr)
// See http://thethingsnetwork.org/wiki/AddressSpace
// The library converts the address to network byte order as needed.
static const u4_t DEVADDR = 0x26002FB5; // <-- Change this address for every node!

// These callbacks are only used in over-the-air activation, so they are
// left empty here (we cannot leave them out completely unless
// DISABLE_JOIN is set in config.h, otherwise the linker will complain).
void os_getArtEui (u1_t* buf) { }
void os_getDevEui (u1_t* buf) { }
void os_getDevKey (u1_t* buf) { }


static osjob_t sendjob;

// Schedule TX every this many seconds (might become longer due to duty
// cycle limitations).
const unsigned TX_INTERVAL = 300 ;		// 5 min reporting cycle
const int EOD_HOUR = 9;			// Daily totals are reset at 9am (local);

// Pin mapping
// TL Modifications:
// Specifically for Arduino Uno/Mega + Dragino LoRa Shield US900
const lmic_pinmap lmic_pins = {
    .nss = 10,
    .rxtx = LMIC_UNUSED_PIN,
    .rst = 9,
    .dio = {2, 6, 7},
};

void onEvent (ev_t ev) {
    Serial.print(os_getTime());
    Serial.print(": ");
    switch(ev) {
        case EV_SCAN_TIMEOUT:
            Serial.println(F("EV_SCAN_TIMEOUT"));
            break;
        case EV_BEACON_FOUND:
            Serial.println(F("EV_BEACON_FOUND"));
            break;
        case EV_BEACON_MISSED:
            Serial.println(F("EV_BEACON_MISSED"));
            break;
        case EV_BEACON_TRACKED:
            Serial.println(F("EV_BEACON_TRACKED"));
            break;
        case EV_JOINING:
            Serial.println(F("EV_JOINING"));
            break;
        case EV_JOINED:
            Serial.println(F("EV_JOINED"));
            break;
        /*
        || This event is defined but not used in the code. No
        || point in wasting codespace on it.
        ||
        || case EV_RFU1:
        ||     Serial.println(F("EV_RFU1"));
        ||     break;
        */
        case EV_JOIN_FAILED:
            Serial.println(F("EV_JOIN_FAILED"));
            break;
        case EV_REJOIN_FAILED:
            Serial.println(F("EV_REJOIN_FAILED"));
            break;
        case EV_TXCOMPLETE:
            Serial.println(F("EV_TXCOMPLETE (includes waiting for RX windows)\n"));
            if (LMIC.txrxFlags & TXRX_ACK)
              Serial.println(F("Received ack"));
            if (LMIC.dataLen) {
              Serial.println(F("Received "));
              Serial.println(LMIC.dataLen);
              Serial.println(F(" bytes of payload"));
            }
			digitalWrite(TX_Pin, LOW);		// Tx/Rx LED off
            // Schedule next transmission - move next line to schedule in loop(), to stay in sync with sensors
//            os_setTimedCallback(&sendjob, os_getTime()+sec2osticks(TX_INTERVAL), do_send);
			break;
        case EV_LOST_TSYNC:
            Serial.println(F("EV_LOST_TSYNC"));
            break;
        case EV_RESET:
            Serial.println(F("EV_RESET"));
            break;
        case EV_RXCOMPLETE:
            // data received in ping slot
            Serial.println(F("EV_RXCOMPLETE"));
            break;
        case EV_LINK_DEAD:
            Serial.println(F("EV_LINK_DEAD"));
            break;
        case EV_LINK_ALIVE:
            Serial.println(F("EV_LINK_ALIVE"));
            break;
        /*
        || This event is defined but not used in the code. No
        || point in wasting codespace on it.
        ||
        || case EV_SCAN_FOUND:
        ||    Serial.println(F("EV_SCAN_FOUND"));
        ||    break;
        */
        case EV_TXSTART:
            Serial.println(F("EV_TXSTART"));
			digitalWrite(TX_Pin, HIGH);		//  Tx/Rx LED ON for external visual
            break;
			
        default:
            Serial.print(F("Unknown event: "));
            Serial.println((unsigned) ev);
            break;
    }
}

void do_send(osjob_t* j){

    // Check if there is not a current TX/RX job running
    if (LMIC.opmode & OP_TXRXPEND) {
        Serial.println(F("OP_TXRXPEND, not sending"));
    } else {
        // Prepare upstream data transmission at the next possible time.
        LMIC_setTxData2(1, sensorObs[reportObs].readAccess, sizeof(obsSet), 0);     // Use the last completed set of obs
        Serial.println(F("Packet queued"));
        Serial.print(F("Sending packet on frequency: "));
        Serial.println(LMIC.freq);
    }
    // Next TX is scheduled after TX_COMPLETE event.
}


// Interrupt handler routine for timer interrupt
void isr_timer() {
	
	timerCount++;

	if(timerCount == Sample_Interval) {
		// convert to km/h using the formula V=P(2.25/T)*1.609 where T = sample interval
		// i.e. V = P(2.25/2.5)*1.609 = P * Speed_Conversion factor  (=1.4481  for 2.5s interval)
		windSpeed = rotations * Speed_Conversion; 
		rotations = 0;   
		isSampleRequired = true;
		timerCount = 0;						// Restart the interval count
	}
}

// Interrupt handler routine to increment the rotation count for wind speed
void isr_rotation ()   {

  if ((millis() - contactBounceTime) > BounceInterval ) {  // debounce the switch contact.
    rotations++;
    contactBounceTime = millis();
  }
}

// Interrrupt handler routine that is triggered when the rg-11 detects rain   
void isr_rg ()   { 

   if ((millis() - contactTime) > BounceInterval ) {  // debounce of sensor signal
      tipCount++;
      contactTime = millis();
   } 
} 

//  Calculate average of 'count' readings using boxcar method
int average(int value)
{
  static int i;
  static long sum=0;

  sum -= boxcar[i];  //remove oldest value from sum
  boxcar[i] = value;  // add new value to array
  sum += boxcar[i]; // add new value to sum

  i++;
  if (i == count) i=0;
  return sum/count;
}

// Get Wind Direction
void getWindDirection(bool baseRange) {
	static int recentAvgDirn = 0;		// average of last 3 adjusted measurements
	int altReading, deltaAsRead, deltaExtd;		// candidate alternative to raw direction measurement
	
	if (baseRange) {		// take a reading in standard 0-360 deg. range
		vaneValue = analogRead(WindVane_Pin);
		vaneDirection = map(vaneValue, 0, 1023, 0, 359);
		calDirection = vaneDirection + VaneOffset;
		if(calDirection > 360)
			calDirection = calDirection - 360;
		return;			// returns value via calDirection
	}   
	
	// Here (baseRange is FALSE) we find if +/- 360 gives a reading closer to the most recent wind direction
	// Does not take a new direction reading - uses the last one 
	// This averaging is only invoked for directions included in the 5 min reports
	if (calDirection > 270) altReading	= calDirection - 360;
	else if (calDirection < 90) altReading = calDirection + 360;
	else altReading = calDirection;
	
	deltaAsRead = abs(calDirection - recentAvgDirn);
	deltaExtd = abs(altReading - recentAvgDirn);
	calDirection = (deltaAsRead < deltaExtd) ? calDirection : altReading;
	
	// Update the average etc
	recentAvgDirn = average(calDirection);
}

// Field format utility for printing
void print2digits(int number)  {
	if (number >= 0 && number <10) {
		Serial.write('0');
	}
	Serial.print(number);
}

// Check if a report is the last of a daily sequence.  Relies on window open +/- 1hr 
//  either side of EOD_HOUR
boolean resetDaily(time_t localTime, int windowOpensHr, int windowClosesHr) {
	int checkHour = hour(localTime);
	if ((checkHour < windowOpensHr) || (checkHour >= windowClosesHr)) {
		dailyTotalsDue = true;
		return false; 		// Outside the reset time window
	}
	if (dailyTotalsDue) {
		if (checkHour == EOD_HOUR) {
			dailyTotalsDue = false;
			return true;
		}
		if ((minute(localTime) + TX_INTERVAL/60) < 60 ) {
			dailyTotalsDue = true;
			return false;    // Next report will still be ahead of EOD_HOUR
		}
		else {
			dailyTotalsDue = false;
			return true;	//  Next report period should start from zero daily totals
		}
	}
	else return false;
}
				

// Print utility for packed structure
void printIt(uint8_t *charArray, int length) {
  int i;
	char charMember;
	Serial.print("buff length:"); Serial.println(length);
	for (i=0; i<length; i++) {
		charMember = charArray[i];
		Serial.println(charMember, BIN);
	}
	Serial.println("===EndOfBuffer========");
}

void setup() {
	
	while (!Serial); // wait for Serial to be initialized
	Serial.begin(115200);
	delay(500);     
	
	setSyncProvider(RTC.get);
	setSyncInterval(500);     // resync system time to RTC every 500 sec


	// prepare obsPayload selection indices
	currentObs = 0;
	reportObs = 1;
	dailyTotalsDue = true;
  
	// initialise anemometer values
	rotations = 0;
	isSampleRequired = false;
	windGust = 0;
	calGustDirn = 0;
  
	// setup RG11 rain totals & conversion factor
	obsRainfallCount = 0;
	obsReportRainfallRate = 0.0;
	dailyRainfallCount = 0;
  
	// setup timer values
	timerCount = 0;
	sampleCount = 0;
	
  
	// Initialise the Temperature measurement library & set sensor resolution to 12 (10) bits
	DSsensors.setResolution(airTempAddr, 12);
	DSsensors.setResolution(caseTempAddr, 10);
 
	if (!bme.begin())  {    
      Serial.println("Could not find BME280 sensor -  check wiring");
     while (1);
	}

	
	#ifdef VCC_ENABLE
	// For Pinoccio Scout boards
		pinMode(VCC_ENABLE, OUTPUT);
		digitalWrite(VCC_ENABLE, HIGH);
		delay(1000);
	#endif
	
  // Setup pins & interrupts	
	pinMode(TX_Pin, OUTPUT);
	pinMode(WindSensor_Pin, INPUT);
	pinMode(RG11_Pin, INPUT);

	attachInterrupt(digitalPinToInterrupt(WindSensor_Pin), isr_rotation, FALLING);
	attachInterrupt(digitalPinToInterrupt(RG11_Pin),isr_rg, FALLING);
	
	//Setup the timer for 0.5s
	#ifdef TIMER_FROM_RTC
		// For RTC-generated clock timer
		pinMode(SampleInt_Pin, INPUT_PULLUP);
		attachInterrupt(digitalPinToInterrupt(SampleInt_Pin), isr_timer, FALLING); 
	#else
		// timer drawn from internal MCU Timer1 interrupt
		Timer1.initialize(Timing_Clock);     
		Timer1.attachInterrupt(isr_timer);
	#endif
   
    // LMIC init
    os_init();
		
    // Reset the MAC state. Session and pending data transfers will be discarded.
    LMIC_reset();

    // Set static session parameters. Instead of dynamically establishing a session
    // by joining the network, precomputed session parameters are be provided.
    #ifdef PROGMEM
    // On AVR, these values are stored in flash and only copied to RAM
    // once. Copy them to a temporary buffer here, LMIC_setSession will
    // copy them into a buffer of its own again.
		uint8_t appskey[sizeof(APPSKEY)];
		uint8_t nwkskey[sizeof(NWKSKEY)];
		memcpy_P(appskey, APPSKEY, sizeof(APPSKEY));
		memcpy_P(nwkskey, NWKSKEY, sizeof(NWKSKEY));
		LMIC_setSession (0x13, DEVADDR, nwkskey, appskey);
    #else
		// If not running an AVR with PROGMEM, just use the arrays directly
		LMIC_setSession (0x13, DEVADDR, NWKSKEY, APPSKEY);
    #endif

    #if defined(CFG_eu868)
    // Set up the channels used by the Things Network, which corresponds
    // to the defaults of most gateways. Without this, only three base
    // channels from the LoRaWAN specification are used, which certainly
    // works, so it is good for debugging, but can overload those
    // frequencies, so be sure to configure the full frequency range of
    // your network here (unless your network autoconfigures them).
    // Setting up channels should happen after LMIC_setSession, as that
    // configures the minimal channel set.
    LMIC_setupChannel(0, 868100000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(1, 868300000, DR_RANGE_MAP(DR_SF12, DR_SF7B), BAND_CENTI);      // g-band
    LMIC_setupChannel(2, 868500000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(3, 867100000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(4, 867300000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(5, 867500000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(6, 867700000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(7, 867900000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(8, 868800000, DR_RANGE_MAP(DR_FSK,  DR_FSK),  BAND_MILLI);      // g2-band
    // TTN defines an additional channel at 869.525Mhz using SF9 for class B
    // devices' ping slots. LMIC does not have an easy way to define set this
    // frequency and support for class B is spotty and untested, so this
    // frequency is not configured here.
    #elif defined(CFG_us915)
    // NA-US channels 0-71 are configured automatically
    // but only one group of 8 should (a subband) should be active
    // TTN recommends the second sub band, 1 in a zero based count.
    // https://github.com/TheThingsNetwork/gateway-conf/blob/master/US-global_conf.json
    LMIC_selectSubBand(1);
    // Specify to operate on AU915 sub-band 2
    #elif defined(CFG_au915)
    Serial.println(F("Loading AU915 Configuration..."));
    // Set to AU915 sub-band 2
    LMIC_selectSubBand(1); 
    #endif

    // Disable link check validation
    LMIC_setLinkCheckMode(0);

    // TTN uses SF9 for its RX2 window.
    //LMIC.dn2Dr = DR_SF9;
	LMIC.dn2Dr = DR_SF7CR;    //** now uses SF7 

    // Set data rate and transmit power for uplink (note: txpow seems to be ignored by the library)
    LMIC_setDrTxpow(DR_SF7,14);

    // Start job
    do_send(&sendjob);
		
	sei();   // Enable Interrupts
	
}

void loop() {

	if(isSampleRequired) {
		sampleCount++;
		DSsensors.requestTemperatures();    // Read temperatures from all DS18B20 devices
		bme.readSensor();					// Read humidity & barometric pressure
	
		getWindDirection(BaseRange);			//  Read dirn in range 0 - 360 deg.
		
		if (windSpeed > windGust) {      // Check last sample of windspeed for new Gust record
			windGust = windSpeed;
			calGustDirn = calDirection;
		}
			

	//  Does this sample complete a reporting cycle?   If so, prepare payload.
		if (sampleCount == Report_Interval) {
			obsRainfallCount = tipCount - dailyRainfallCount;
			dailyRainfallCount = tipCount;
			getWindDirection(ExtdRange);	// Update direction to reflect recent average in {-90 to 450 deg}
			
			obsReportRainfallRate = obsRainfallCount * Bucket_Size * 3600 / reportIntervalSec;   //  mm/hr
			sensorObs[currentObs].obsReport.windGustX10 = windGust * 10.0;
			sensorObs[currentObs].obsReport.windGustDir = calGustDirn;
			sensorObs[currentObs].obsReport.tempX10 = (DSsensors.getTempC(airTempAddr)+ 100.0)* 10.0;
			sensorObs[currentObs].obsReport.humidX10 = bme.getHumidity()*10.0;
			sensorObs[currentObs].obsReport.pressX10 = bme.getPressure_MB()*10.0;
			sensorObs[currentObs].obsReport.rainflX10 = obsReportRainfallRate * 10.0;
			sensorObs[currentObs].obsReport.windspX10 = windSpeed * 10.0;
			sensorObs[currentObs].obsReport.windDir =  calDirection +90;   // NB: Offset caters for extended range -90 to 450
			sensorObs[currentObs].obsReport.dailyRainX10 = dailyRainfallCount * Bucket_Size * 10.0;
			sensorObs[currentObs].obsReport.casetempX10 = (DSsensors.getTempC(caseTempAddr)+ 100.0) * 10.0;
			

        //  Schedule Callback to transmit the report
			os_setTimedCallback(&sendjob, os_getTime()+sec2osticks(TX_INTERVAL/10), do_send);
		
			sampleCount = 0;
			currentObs = 1- currentObs;		//
			reportObs = 1 - currentObs;   	// switch reporting to last collected observation
			windGust = 0;					// Gust reading is reset for every reporting period
		
			
		// Check if this report completes a daily cycle
			utc = now();
			localTime = auEastern.toLocal(utc, &tcr);
			if (resetDaily(localTime, EOD_HOUR - 1, EOD_HOUR + 1) ){
				tipCount = 0;
				dailyRainfallCount = 0;     // Next report cycle starts daily total from 0mm
				obsRainfallCount = 0;
			}
		}
			
		isSampleRequired = false;
	}
	
    os_runloop_once();
    
}