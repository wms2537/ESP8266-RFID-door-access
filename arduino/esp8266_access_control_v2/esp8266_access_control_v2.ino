/*
   --------------------------------------------------------------------------------------------------------------------
   Example sketch/program showing An ESP8266 Door Access Control featuring RFID, SPIFFS, Relay
   --------------------------------------------------------------------------------------------------------------------
   This is a MFRC522 library example; for further details and other examples see: https://github.com/miguelbalboa/rfid

   This example showing a complete Door Access Control System

  Simple Work Flow (not limited to) :
                                     +---------+
  +----------------------------------->READ TAGS+^------------------------------------------+
  |                              +--------------------+                                     |
  |                              |                    |                                     |
  |                              |                    |                                     |
  |                         +----v-----+        +-----v----+                                |
  |                         |MASTER TAG|        |OTHER TAGS|                                |
  |                         +--+-------+        ++-------------+                            |
  |                            |                 |             |                            |
  |                            |                 |             |                            |
  |                      +-----v---+        +----v----+   +----v------+                     |
  |         +------------+READ TAGS+---+    |KNOWN TAG|   |UNKNOWN TAG|                     |
  |         |            +-+-------+   |    +-----------+ +------------------+              |
  |         |              |           |                |                    |              |
  |    +----v-----+   +----v----+   +--v--------+     +-v----------+  +------v----+         |
  |    |MASTER TAG|   |KNOWN TAG|   |UNKNOWN TAG|     |GRANT ACCESS|  |DENY ACCESS|         |
  |    +----------+   +---+-----+   +-----+-----+     +-----+------+  +-----+-----+         |
  |                       |               |                 |               |               |
  |       +----+     +----v------+     +--v---+             |               +--------------->
  +-------+EXIT|     |DELETE FROM|     |ADD TO|             |                               |
          +----+     |  SPIFFS   |     |SPIFFS|             |                               |
                     +-----------+     +------+             +-------------------------------+


   Use a Master Card which is act as Programmer then you can able to choose card holders who will granted access or not

 * **Stores Information on SPIFFS**

   Information stored on non volatile ESP8266's SPIFFS memory to preserve Users' tag and Master Card. No Information lost
   if power lost. SPIFFS has unlimited Read cycle but roughly 10,000 limited Write cycle.
   Data is stored in JSON format
   Master card will be deleted if wipe button is pressed on boot for 8 seconds
   Other records(not including master card) will be deleted if wipe button is pressed for 8 seconds

 * **Security**
   To keep it simple we are going to use Tag's Unique IDs. Modified from original example, this code hashes the ID and stores it

   @license Released into the public domain.

   Typical pin layout used:
   ---------------------------------
               MFRC522      ESP
               Reader/PCD   8266
   Signal      Pin          Pin
   ---------------------------------
   RST/Reset   RST          0
   SPI SS      SDA(SS)      15
   SPI MOSI    MOSI         13
   SPI MISO    MISO         12
   SPI SCK     SCK          14
*/
#include <FS.h> //this needs to be first, or it all crashes and burns...
#include <SPI.h>        // RC522 Module uses SPI protocol
#include <MFRC522.h>  // Library for Mifare RC522 Devices
#include <ArduinoJson.h>          // https://github.com/bblanchon/ArduinoJson
#include <Hash.h>
/*
   Using a 74hc138 3-to-8 line demultiplexer due to limited io pins
*/

#define DEMUX_A0 2
#define DEMUX_A1 4
#define DEMUX_A2 5

#define wipeB A0     // Button pin for WipeMode

bool programMode = false;  // initialize programming mode to false

uint8_t successRead;    // Variable integer to keep if we have Successful Read from Reader

String cardHash;   // Stores scanned ID read from RFID Module
DynamicJsonDocument recordsDoc(1024);


// Create MFRC522 instance.
#define SS_PIN 15
#define RST_PIN 0
MFRC522 mfrc522(SS_PIN, RST_PIN);

///////////////////////////////////////// Setup ///////////////////////////////////
void setup() {
  //Arduino Pin Configuration
  pinMode(DEMUX_A0, OUTPUT);
  pinMode(DEMUX_A1, OUTPUT);
  pinMode(DEMUX_A2, OUTPUT);

  //Be careful how relay circuit behave on while resetting or power-cycling your Arduino
  toggleRedLed();

  //Protocol Configuration
  Serial.begin(115200);  // Initialize serial communications with PC
  SPI.begin();           // MFRC522 Hardware uses SPI protocol
  mfrc522.PCD_Init();    // Initialize MFRC522 Hardware

  //If you set Antenna Gain to Max it will increase reading distance
  //mfrc522.PCD_SetAntennaGain(mfrc522.RxGain_max);

  Serial.println(F("ESP8266 Access Control Example v1.0"));   // For debugging purposes
  ShowReaderDetails();  // Show details of PCD - MFRC522 Card Reader details

  if (SPIFFS.begin()) {  // Load records from SPIFFS
    Serial.println("mounted file system");
    if (SPIFFS.exists("/records.json")) {
      //file exists, reading and loading
      Serial.println("reading records file");
      File recordsFile = SPIFFS.open("/records.json", "r");
      if (recordsFile) {
        Serial.println("opened records file");
        DeserializationError error = deserializeJson(recordsDoc, recordsFile);

        if (recordsDoc.isNull()) {  // Init with empty array if json does not exist to avoid null
          deserializeJson(recordsDoc, "[]");
        }
        serializeJson(recordsDoc, Serial);
        if (!error) {
          Serial.println("\nparsed json");
        } else {
          Serial.println("failed to load json config");
        }
      }
    } else {  // Create file if it does not exist
      File configFile = SPIFFS.open("/records.json", "w");
      if (!configFile) {
        Serial.println("failed to open config file for writing");
      } else {
        deserializeJson(recordsDoc, "[]");
        serializeJson(recordsDoc, configFile);
        configFile.close();
        return;
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }

  //Wipe Code - If the Button (wipeB) Pressed while setup run (powered on) it wipes master card
  if (analogRead(wipeB) >  1000) {  // when button pressed pin should get low, button connected to ground
    toggleBuzzer();
    delay(1000);
    toggleBlueLed();
    Serial.println(F("Wipe Button Pressed"));
    Serial.println(F("Master Card will be Erased! in 8 seconds"));
    bool buttonState = monitorWipeButton(8000); // Give user enough time to cancel operation
    if (buttonState == true && analogRead(wipeB) >  1000) {    // If button still be pressed, wipe records
      if (!recordsDoc.isNull()) {
        JsonArray allRecords = recordsDoc.as<JsonArray>();
        for (JsonArray::iterator it = allRecords.begin(); it != allRecords.end(); ++it) {
          if ((*it)["role"] == "master") {
            allRecords.remove(it);
          }
        }
        recordsDoc.garbageCollect();
        File configFile = SPIFFS.open("/records.json", "w");
        if (!configFile) {
          Serial.println("failed to open config file for writing");
        } else {
          serializeJson(recordsDoc, configFile);
          configFile.close();
        }
      }
      Serial.println(F("Master Card Erased from device"));
      Serial.println(F("Please reset to re-program Master Card"));
      while (1) {
        yield();
      };
    }
    Serial.println(F("Master Card Erase Cancelled"));
  }
  // Check if master card defined, if not let user choose a master card
  if (!isMasterDefined()) {
    Serial.println(F("No Master Card Defined"));
    Serial.println(F("Scan A PICC to Define as Master Card"));
    do {
      successRead = getID();            // sets successRead to 1 when we get read from reader otherwise 0
      toggleRedLed();    // Visualize Master Card need to be defined
      delay(200);
      toggleBlueLed();
      delay(200);
      yield();
    }
    while (!successRead);                  // Program will not go further while you not get a successful read
    writeID(cardHash, true);
    Serial.println(F("Master Card Defined"));
  }
  Serial.println(F("-------------------"));
  Serial.println(F("Master Card's UID"));
  if (!recordsDoc.isNull()) {
    for (JsonObject record : recordsDoc.as<JsonArray>()) {
      if (record["role"] == "master") {
        Serial.println(record["uid"].as<String>());
      }
    }
  }
  Serial.println("");
  Serial.println(F("-------------------"));
  Serial.println(F("Everything is ready"));
  Serial.println(F("Waiting PICCs to be scanned"));
  toggleBuzzer();
  delay(200);
  toggleGreenLed();
  delay(200);
  toggleBuzzer();
  delay(200);
  toggleGreenLed();
  delay(200);
  toggleBuzzer();
  delay(200);
  toggleGreenLed();
  delay(200);
  // Everything ready lets give user some feedback
}


///////////////////////////////////////// Main Loop ///////////////////////////////////
void loop () {
  do {
    successRead = getID();  // sets successRead to 1 when we get read from reader otherwise 0
    // When device is in use if wipe button pressed for 10 seconds initialize records wiping
    if (analogRead(wipeB) >  1000) { // Check if button is pressed
      // Visualize normal operation is iterrupted by pressing wipe button Red is like more Warning to user
      toggleBuzzer();
      delay(1000);
      toggleBlueLed();
      // Give some feedback
      Serial.println(F("Wipe Button Pressed"));
      Serial.println(F("You have 8 seconds to Cancel"));
      Serial.println(F("This will be remove all records and cannot be undone"));
      bool buttonState = monitorWipeButton(8000); // Give user enough time to cancel operation
      if (buttonState == true && analogRead(wipeB) >  1000) {    // If button still be pressed, wipe records
        Serial.println(F("Starting Wiping records"));
        if (!recordsDoc.isNull()) {
          JsonArray allRecords = recordsDoc.as<JsonArray>();
          for (JsonArray::iterator it = allRecords.begin(); it != allRecords.end(); ++it) {
            if ((*it)["role"] != "master") {
              allRecords.remove(it);
            }
          }
          recordsDoc.garbageCollect();
          File configFile = SPIFFS.open("/records.json", "w");
          if (!configFile) {
            Serial.println("failed to open config file for writing");
          } else {
            serializeJson(recordsDoc, configFile);
            configFile.close();
          }
        }
      }
      else {
        Serial.println(F("Wiping Cancelled")); // Show some feedback that the wipe button did not pressed for 15 seconds
        toggleRedLed();
      }
    }
    if (programMode) {
      toggleBlueLed();
      delay(200);
      toggleGreenLed();
      delay(200);
    }
    else {
      normalModeOn();     // Normal mode, blue Power LED is on, all others are off
    }
    yield();
  }
  while (!successRead);  //the program will not go further while you are not getting a successful read
  switch (findID(cardHash)) {
    case 1:
      if (programMode) { //When in program mode check First If master card scanned again to exit program mode
        Serial.println(F("Master Card Scanned"));
        Serial.println(F("Exiting Program Mode"));
        Serial.println(F("-----------------------------"));
        programMode = false;
      } else { // If scanned card's ID matches Master Card's ID - enter program mode
        programMode = true;
        Serial.println(F("Hello Master - Entered Program Mode"));
        JsonArray allRecords = recordsDoc.as<JsonArray>();
        Serial.print(F("I have "));     // stores the number of ID's in records
        Serial.print(allRecords.size());
        Serial.print(F(" record(s) registered"));
        Serial.println("");
        Serial.println(F("Scan a PICC to ADD or REMOVE"));
        Serial.println(F("Scan Master Card again to Exit Program Mode"));
        Serial.println(F("-----------------------------"));
      }
      break;
    case 2:
      if (programMode) {  // If scanned card is known delete it
        Serial.println(F("I know this PICC, removing..."));
        deleteID(cardHash);
        Serial.println("-----------------------------");
        Serial.println(F("Scan a PICC to ADD or REMOVE"));
      } else {
        Serial.println(F("Welcome, You shall pass"));
        granted(5000);         // Open the door lock
      }
      break;
    case 0:
      if (programMode) {  // If scanned card is not known add it
        Serial.println(F("I do not know this PICC, adding..."));
        writeID(cardHash, false);
        Serial.println(F("-----------------------------"));
        Serial.println(F("Scan a PICC to ADD or REMOVE"));
      } else {      // If not, show that the ID was not valid
        Serial.println(F("You shall not pass"));
        denied();
      }
      break;
    default:
      break;
  }
}

/////////////////////////////////////////  Access Granted    ///////////////////////////////////
void granted ( uint16_t setDelay) {
  toggleBuzzer();
  delay(500);
  toggleOutput();
  delay(setDelay);          // Hold door lock open for given seconds
  toggleRedLed();
}

///////////////////////////////////////// Access Denied  ///////////////////////////////////
void denied() {
  toggleBuzzer();
  delay(200);
  toggleRedLed();
  delay(200);
  toggleBuzzer();
  delay(200);
  toggleRedLed();
}


///////////////////////////////////////// Get PICC's UID ///////////////////////////////////
uint8_t getID() {
  // Getting ready for Reading PICCs
  if ( ! mfrc522.PICC_IsNewCardPresent()) { //If a new PICC placed to RFID reader continue
    return 0;
  }
  if ( ! mfrc522.PICC_ReadCardSerial()) {   //Since a PICC placed get Serial and continue
    return 0;
  }
  // There are Mifare PICCs which have 4 byte or 7 byte UID care if you use 7 byte PICC
  // I think we should assume every PICC as they have 4 byte UID
  // Until we support 7 byte PICCs
  Serial.println(F("Scanned PICC's UID:"));
  String hexstring = "";
  for ( uint8_t i = 0; i < 4; i++) {  //
    byte rb = mfrc522.uid.uidByte[i];
    if (rb < 0x10) {
      hexstring += '0';
    }

    hexstring += String(rb, HEX);
  }
  cardHash = sha1(hexstring);
  Serial.println(cardHash);
  mfrc522.PICC_HaltA(); // Stop reading
  return 1;
}

void ShowReaderDetails() {
  // Get the MFRC522 software version
  byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  Serial.print(F("MFRC522 Software Version: 0x"));
  Serial.print(v, HEX);
  if (v == 0x91)
    Serial.print(F(" = v1.0"));
  else if (v == 0x92)
    Serial.print(F(" = v2.0"));
  else
    Serial.print(F(" (unknown),probably a chinese clone?"));
  Serial.println("");
  // When 0x00 or 0xFF is returned, communication probably failed
  if ((v == 0x00) || (v == 0xFF)) {
    Serial.println(F("WARNING: Communication failure, is the MFRC522 properly connected?"));
    Serial.println(F("SYSTEM HALTED: Check connections."));
    // Visualize system is halted
    toggleRedLed();
    while (true) {
      toggleBuzzer();
      delay(200);
      toggleRedLed();
      delay(200);
      yield();
    }; // do not go further
  }
}

//////////////////////////////////////// Normal Mode Led  ///////////////////////////////////
void normalModeOn () {
  toggleRedLed();
}

///////////////////////////////////////// Add ID to Records   ///////////////////////////////////
void writeID(String hashedUID, bool isMaster) {
  if (!findID(hashedUID)) {     // Before we add to records, check to see if we have seen this card before!
    JsonArray allRecords = recordsDoc.as<JsonArray>();
    JsonObject nested = allRecords.createNestedObject();
    nested["uid"] = hashedUID;
    nested["role"] = isMaster ? "master" : "user";
    serializeJson(allRecords, Serial);
    File configFile = SPIFFS.open("/records.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    } else {
      serializeJson(recordsDoc, configFile);
      configFile.close();
      successWrite();
      return;
    }
  }
  failedWrite();
  Serial.println(F("Failed! There is something wrong with ID or bad memory"));
}

///////////////////////////////////////// Remove ID from Records   ///////////////////////////////////
void deleteID(String hashedUID) {
  if (!recordsDoc.isNull()) {
    JsonArray allRecords = recordsDoc.as<JsonArray>();
    for (JsonArray::iterator it = allRecords.begin(); it != allRecords.end(); ++it) {
      if ((*it)["uid"].as<String>() == hashedUID) {
        allRecords.remove(it);
        recordsDoc.garbageCollect();
        File configFile = SPIFFS.open("/records.json", "w");
        if (!configFile) {
          Serial.println("failed to open config file for writing");
        } else {
          serializeJson(recordsDoc, configFile);
          configFile.close();
          return;
        }
      }
    }
  }
  failedWrite();      // If not
}

///////////////////////////////////////// Find ID From Records   ///////////////////////////////////
uint8_t findID(String hashedUID) {
  if (!recordsDoc.isNull()) {
    for (JsonObject record : recordsDoc.as<JsonArray>()) {
      if (record["uid"] == hashedUID) {
        return record["role"] == "master" ? 1 : 2;
      }
    }
  }
  return 0;
}

///////////////////////////////////////// Write Success to Records   ///////////////////////////////////
// Beep 3 times with green led to indicate a successful write
void successWrite() {
  toggleBuzzer();
  delay(200);
  toggleGreenLed();
  delay(200);
  toggleBuzzer();
  delay(200);
  toggleGreenLed();
  delay(200);
  toggleBuzzer();
  delay(200);
  toggleGreenLed();
}

///////////////////////////////////////// Write Failed to Records   ///////////////////////////////////
// Long beep 3 times with red led to indicate a failed write
void failedWrite() {
  toggleBuzzer();
  delay(200);
  toggleRedLed();
  delay(200);
  toggleBuzzer();
  delay(200);
  toggleRedLed();
  delay(200);
  toggleBuzzer();
  delay(200);
  toggleRedLed();
}

///////////////////////////////////////// Success Remove UID From Records  ///////////////////////////////////
// Beep 3 times with blue led to indicate a success delete
void successDelete() {
  toggleBuzzer();
  delay(200);
  toggleBlueLed();
  delay(200);
  toggleBuzzer();
  delay(200);
  toggleBlueLed();
  delay(200);
  toggleBuzzer();
  delay(200);
  toggleBlueLed();
}

////////////////////// DEMUX IO Operations /////////////////////////////////////////////////////
void toggleRedLed() {
  digitalWrite(DEMUX_A2, 0);
  digitalWrite(DEMUX_A1, 0);
  digitalWrite(DEMUX_A0, 0);
}

void toggleGreenLed() {
  digitalWrite(DEMUX_A2, 0);
  digitalWrite(DEMUX_A1, 0);
  digitalWrite(DEMUX_A0, 1);
}

void toggleBlueLed() {
  digitalWrite(DEMUX_A2, 0);
  digitalWrite(DEMUX_A1, 1);
  digitalWrite(DEMUX_A0, 0);
}

void toggleBuzzer() {
  digitalWrite(DEMUX_A2, 0);
  digitalWrite(DEMUX_A1, 1);
  digitalWrite(DEMUX_A0, 1);
}

void toggleOutput() {
  digitalWrite(DEMUX_A2, 1);
  digitalWrite(DEMUX_A1, 0);
  digitalWrite(DEMUX_A0, 0);
}

////////////////////// Check readCard IF is masterCard   ///////////////////////////////////
// Check to see if the ID passed is the master programing card
bool isMaster(String hashedUID) {
  if (!recordsDoc.isNull()) {
    for (JsonObject record : recordsDoc.as<JsonArray>()) {
      if (record["uid"] == hashedUID && record["role"] == "master") {
        return true;
      }
    }
  }
  return false;
}

bool isMasterDefined() {
  if (!recordsDoc.isNull()) {
    for (JsonObject record : recordsDoc.as<JsonArray>()) {
      if (record["role"] == "master") {
        return true;
      }
    }
  }
  return false;
}

bool monitorWipeButton(uint32_t interval) {
  uint32_t now = (uint32_t)millis();
  while ((uint32_t)millis() - now < interval)  {
    // check on every half a second
    if (((uint32_t)millis() % 500) == 0) {
      if (analogRead(wipeB) <  1000)
        return false;
    }
    yield();
  }
  return true;
}
