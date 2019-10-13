#include <EEPROM.h>

#define TOTAL_DEVICES 4 // Number of keys/phones
#define SETUP_MAGICNUM 0x45 // Magic number, which signals that the Bluno has been initialised with "Key Holder" as name

typedef enum {START, WAITING_AT, IN_AT, DEVICE_CONNECTED, DEVICE_CONNECTED_READY, WAITING_CONNECT, AT_PENDING_EXIT} state_t; // List of states the device can be in

state_t state = START; // Default state is called START
unsigned long lastTime; // Time when last serial message was received, it is used to make sure that each phone can connect for max 10 seconds

volatile boolean didUserPressFactoryResetButton = false;

void setup() {
  // Code here runs once only
  Serial.begin(115200); // Initialise connection with the Serial, for debug and for AT commands
  pinMode(2, INPUT); // Set pins 2-5 and A0-A3 (all the GPIO pins) to input pins, and pin 13 (onboard LED) to output for debugging
  pinMode(3, INPUT);
  pinMode(4, INPUT);
  pinMode(5, INPUT);
  pinMode(A0, INPUT);
  pinMode(A1, INPUT);
  pinMode(A2, INPUT);
  pinMode(A3, INPUT);
  pinMode(13, OUTPUT);
  digitalWrite(13, LOW);
  attachInterrupt(digitalPinToInterrupt(2), triggerFacReset, FALLING); // Set an interrupt on digital pin 2, to detect when it is pushed, and trigger a factory reset
  Serial.println("AT+EXIT"); // Exit AT mode, in case we are in the wrong mode due to pressing RESET onboard PTM
}

void triggerFacReset() { // Interrupt, triggered when the user pushed PTM on D2
  didUserPressFactoryResetButton = true; // Set a variable, so we can act on it next iteration of main loop
}

String readChars(unsigned int n, String expected, unsigned long timeout) { // This function is a utility function to read from serial, with optional expected value or timeout
  /* If any error happens, returns "", else the read String */
  String out = "";
  int tmp;
  if (expected.length()) n = expected.length();
  if (n == 0)
    return "";
  unsigned long start = millis(); // Get ready in case of timeout
  while ((!Serial.available()) && (timeout == 0 || millis() < start + timeout)) delay(5); // While no data available, and has not timed out, pause for 5 milliseconds
  while (out.length() < n  && (timeout == 0 || millis() < start + timeout)) { // While data is available, we have not consumed enough data, and not timed out
    tmp = Serial.read(); // Read 1 byte
    if (tmp < 0) { // No data is actually available, so we must wait
      delay(5);
      continue; // Restart loop
    }
    if (tmp > 255) return ""; // ASCII character set ends at 255, above there, it's an error
    if (expected.length() && expected.charAt(out.length()) != tmp) return ""; // If the character does not match what we were expecting, exit
    out += (char)tmp; // Save the character we got
  }
  return out; // Return the data we got, or "" on timeout
}

void clearBuffer() { // Clean the read buffer, so that future calls to readChars() do not get old data
  while (Serial.available())
    Serial.read(); // Cleanup read buffer
}

void restart() {
  // This function never returns
  Serial.println("AT+EXIT");
  delay(100);
  Serial.println("Restarting, goodbye!");
  delay(200);
  Serial.print("+++"); // Enter AT mode
  delay(800);
  Serial.println("AT+RESTART"); // Request the Bluetooth controller to restart the AP (application processor)
  Serial.println("AT+EXIT"); // Exit AT mode
  delay(10000); // Wait for us to die
  Serial.println("We weren't killed by the BLE chip, something bad happened");
  while (1) { // error condition, flash forever
    digitalWrite(13, HIGH);
    delay(100);
    digitalWrite(13, LOW);
    delay(100);
  }
}

void loop() {
  // This function is called in a loop, forever as long as the chip is powered
  if (didUserPressFactoryResetButton) {
    digitalWrite(13, HIGH); // Light up for debug purposes
    Serial.println("AT+EXIT"); // Exit AT mode in case we are in it
    delay(500);
    Serial.println("Wiping EEPROM"); // Send debug message
    for (int i = 0 ; i < EEPROM.length(); i++) { // set variable `i` to 0, while it is less than the size of the EEPROM (on-board storage), increment it
      EEPROM.write(i, 0); // Clear that byte of EEPROM
    }
    Serial.println("EEPROM erased."); // Send debug message
    restart();
  }
  state_t laststate = state; // Save the current state so we can know if it has changed
  // The next section calls into the function based on which state we are in
  switch (state) {
    case START:
      start();
      break;
    case WAITING_AT:
      waiting_at();
      break;
    case IN_AT:
      in_at();
      break;
    case DEVICE_CONNECTED:
      device_connected();
      break;
    case DEVICE_CONNECTED_READY:
      device_connected_ready();
      break;
    case WAITING_CONNECT:
      waiting_connect();
      break;
    case AT_PENDING_EXIT:
      at_pending_exit();
      break;
  }
//  delay(500);
  if (state == laststate) { // If the state didn't change, wait 1/5th of a second to give the chip some rest, otherwise 1/10th
    delay(10);
  }
}

void start() {
  clearBuffer(); // Clean backlog
  delay(100); // Required, as the Bluetooth controller ignores +++ if any data was sent recently
  Serial.print("+++");
  delay(700); // Again, it's ignored otherwise
  state = WAITING_AT;
}

void waiting_at() {
  String text = readChars(0, "Enter AT Mode", 1000);
  if (text.length()) {
    state = IN_AT;
  } else {
    state = START; // It didn't enter AT mode, something weird happened.
    Serial.print("got text ");
    Serial.println(text);
  }
}

void in_at() {
  // This function is to initialise the settings of BLE chip
  clearBuffer();
  Serial.println("AT+SETTING=DEFAULT"); // Reset settings of chip
  readChars(0, "OK", 100); // Read the message from BLE chip
  Serial.println("AT+NAME=Key Holder"); // Set name of chip
  readChars(0, "OK", 100);
  Serial.println("AT+ROLE=ROLE_CENTRAL"); // Set chip to central role (android only supports being a peripheral, so this must be central)
  readChars(0, "OK", 100);
  Serial.println("AT+CMODE=ANYONE"); // Allow any phone to connect to device without pairing
  readChars(0, "OK", 100);
  if (EEPROM.read(0) != SETUP_MAGICNUM) { // If magic number is not set in EEPROM
    Serial.println("AT+NAME=Key Holder"); // Program device name
    readChars(0, "OK", 100);
    EEPROM.write(0, SETUP_MAGICNUM); // Store the fact that we have programmed the device name, as the name is not stored or used unless RESTART is sent
    delay(1000); // Wait a while, to give time for settings to be saved
    return;
  }
  state = WAITING_CONNECT; // We are ready for a device to connect
}

void at_pending_exit() {
  // It is trying to exit AT mode but not certainly done yet
  String x = readChars(0, "OK", 100); // Wait for OK message to signal exit successful
  if (!x.length()) {
    state = START; // OK was not read, return to entrypoint
    return;
  }
  state = DEVICE_CONNECTED; // exited AT mode, now device can be communicated with
  delay(100); // Wait in case more data was sent
  clearBuffer(); // Make sure no cached data is used
  lastTime = millis(); // Get time of "last communication", in this case it is not a serial message but when the device was connected
}

void waiting_connect() {
  delay(200); // We are waiting 300ms to make sure it's not spamming or heating up
  clearBuffer();
  Serial.println("AT+RSSI=?"); // Check the RSSI, its the signal strength. Always prefixed with a "-". 000 means not connected, any other 3 digit value means connected
  String rssi = readChars(4, "", 100); // Read 4 unknown chars with 100ms timeout
  if (rssi.charAt(0) != '-') { // If the RSSI starts with wrong character, we know it is some kind of error. Log it and restart
    Serial.print("rssi corrupt");
    Serial.println(rssi);
    state = START;
    return;
  }
  if (rssi == "-000") {
    return; // Still waiting for connection
  }
  clearBuffer(); // Clear buffer again
  Serial.println("AT+EXIT"); // Exit AT mode
  state = AT_PENDING_EXIT; // Wait for exit to complete
}

void device_connected() {
  // This function is fairly complex, and performs bitwise arithmetic. Its purpose is to check the ID the phone sends, and tell whether it matches or not, and whether there is room for pairing.
  // IDs are 4 bytes (4 characters), which is the length of an unsigned long. We read 4 bytes from Serial and check if they are stored or not, and store them, etc
  unsigned long uid = 0; // initialise to 0 so that all bits are 0
  unsigned long tmp; // Temporary value used to ease arithmetic
  unsigned long uids[TOTAL_DEVICES]; // array to store the devices saved in EEPROM
  byte i; // Temporary variable to represent a counter
  byte j; // Temporary variable to represent a counter
  if (Serial.available() > 3) { // If there are at least 4 characters available to read from Serial...
    for (i = 0; i < 4; i++) { // For i = 0 to 3
      // Read the device UID from Serial
      tmp = Serial.read(); // set tmp to the serial value
      if (tmp > 255 || tmp < 0) { // If it's out of bounds...
        Serial.write(0xEE); // ... send an error
        return;
      }
      uid |= (unsigned long)tmp << (i * 8); // convert tmp to an unsigned long (a larger datatype) and do a bitwise left shift by `i` bytes (i.e. store `tmp` into `uid` at byte `i`)
    }
    if (uid == 0) {
      Serial.write(0xCC); // 0 is not a valid ID, it is used to say there is no uid stored
      return;
    }
    byte found = TOTAL_DEVICES; // TOTAL_DEVICES is a magicnumber here to represent unset value
    byte firstempty = TOTAL_DEVICES; // see above ^^^^^^
    for (i = 0; i < TOTAL_DEVICES; i++) { // For each key
      // Read stored UIDs from EEPROM
      uids[i] = 0; // Clear the uid value, as it isn't initialised to 0
      for (j = 0; j < 4; j++) { // For each byte of the ID
        // Read UID with offset (i+0xA)
        uids[i] |= (unsigned long)EEPROM.read(i * 4 + j + 0xA) << (j * 8); // Similar to the earlier long line
      }
      if (uids[i] == uid) found = i; // If we have the same ID in the EEPROM[i] as was sent on serial, we have a match
      if (uids[i] == 0 && firstempty >= TOTAL_DEVICES) firstempty = i; // If the EEPROM[i] is empty, and we haven't yet set a `firstempty`, set it to `i`
    }
    clearBuffer(); // Clear the buffer so that we don't get unwanted messages later on, must be here as the phone might be really fast and reply before we have finished telling it the pair status
    if (found >= TOTAL_DEVICES && firstempty >= TOTAL_DEVICES) {
      Serial.write(0xED); // All full, no room to pair
      restart(); // disconnect device
    }
    Serial.write(found < TOTAL_DEVICES ? 0xFD : 0xCE); // Tell the phone whether it was found as existing device, or paired as new device
    found = found < TOTAL_DEVICES ? found : firstempty; // Set `found` to the position of the phone, even if it is a new pair
    Serial.write(found); // Tell the phone what its ID is
    for (i = 0; i < 4; i++) { // For each byte of the phone's ID...
      EEPROM.write(found * 4 + i + 0xA, (uid >> i * 8) & 0xFF); // Save the byte, the reverse operation of above
    }
    state = DEVICE_CONNECTED_READY;
    lastTime = millis(); // Set the last communication time
  } else if (millis() > lastTime + 10000) {
    restart(); // Timeout
  } else {
    Serial.write(0xE0); // hEllO; tell the phone we are ready to recv ID
  }
}

void device_connected_ready() {
  if (Serial.available()) { // If we were sent a command...
    int com = Serial.read(); // Read the command
    clearBuffer(); // Clear all pending commands, as we have a state machine and must be interacted with one-by-one
    byte cmd; // Command is stored here if it isn't corrupt
    if (com > 255 || com < 0) { // If command is out of range...
      Serial.write(0xEE); // ...send an error...
      return; // ...and wait for next command
    } else {
      cmd = com;
    }
    switch (cmd) { // If the command is:
      case 'p': // ping:
        Serial.write(0xAC); // reply with AC, or ACK, or accept
        break;
      case 'c': // check:
        {
          int flag = 0; // initialise flag as 0 ready for bitwise arithmetic
          if (digitalRead(A0) == HIGH) // If A0 is high...
            flag |= 1; // bit 0 of flag goes high
          if (digitalRead(A1) == HIGH) // Repeat for A1...
            flag |= 2; // ...with bit 1
          if (digitalRead(A2) == HIGH) // For A2...
            flag |= 4; // ...with bit 2
          if (digitalRead(A3) == HIGH) // And for A3
            flag |= 8; // ...with bit 3
          Serial.write(flag); // Send the bitwise flag over serial to the phone
          break;
        }
      default:
        Serial.write(0xEE); // If command is not recognised, send an error message
    }
    lastTime = millis(); // Store when we were last communicated with
  } else if (millis() > lastTime + 10000) { // If there was a timeout, restart the AP, as above
    restart();
  }
}
