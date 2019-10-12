#include <EEPROM.h>

#define TOTAL_DEVICES 4
#define SETUP_MAGICNUM 0x45

typedef enum {START, WAITING_AT, IN_AT, DEVICE_CONNECTED, DEVICE_CONNECTED_READY, WAITING_CONNECT, AT_PENDING_EXIT} state_t;

byte cur_device = 0;
state_t state = START;
state_t oldstate = AT_PENDING_EXIT;
unsigned long lastTime;

volatile boolean didUserPressFactoryResetButton = false;

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  pinMode(2, INPUT);
  pinMode(3, INPUT);
  pinMode(4, INPUT);
  pinMode(5, INPUT);
  pinMode(A0, INPUT);
  pinMode(A1, INPUT);
  pinMode(A2, INPUT);
  pinMode(A3, INPUT);
  pinMode(13, OUTPUT);
  digitalWrite(13, LOW);
  attachInterrupt(digitalPinToInterrupt(2), triggerFacReset, FALLING);
  lastTime = millis();
  Serial.println("AT+EXIT");
}

void triggerFacReset() {
  didUserPressFactoryResetButton = true;
}

String readChars(unsigned int n, String expected, unsigned long timeout) {
  /* If any error happens, returns "", else the read String */
  String out = "";
  int tmp;
  if (expected.length()) n = expected.length();
  if (n == 0)
    return "";
  unsigned long start = millis();
  while ((!Serial.available()) && (timeout == 0 || millis() < start + timeout)) delay(5);
  while (out.length() < n  && (timeout == 0 || millis() < start + timeout)) {
    tmp = Serial.read();
    if (tmp < 0) {
      delay(5);
      continue;
    }
    if (tmp > 255) return "";
    if (expected.length() && expected.charAt(out.length()) != tmp) return "";
    out += (char)tmp;
  }
  return out;
}

void clearBuffer() {
  while (Serial.available())
    Serial.read(); // Cleanup read buffer
}

void loop() {
  if (didUserPressFactoryResetButton) {
    digitalWrite(13, HIGH);
    Serial.println("AT+EXIT");
    delay(500);
    Serial.println("Wiping EEPROM");
    for (int i = 0 ; i < EEPROM.length(); i++) {
      EEPROM.write(i, 0); // Clear the EEPROM when pin 2 is high. Note to self, remember to attach pull-down to prevent random resets
    }
    Serial.println("EEPROM erased.");
    delay(200);
    Serial.print("+++");
    delay(800);
    Serial.println("AT+RESTART");
    Serial.println("AT+EXIT");
    delay(1000);
    Serial.println("We weren't killed by the BLE chip, something bad happened");
    while (1) { // error condition, flash
      digitalWrite(13, HIGH);
      delay(100);
      digitalWrite(13, LOW);
      delay(100);
    }
  }
  state_t laststate = state;
  switch (state) {
    case START:
      EEPROM.write(1, 1);
      start();
      break;
    case WAITING_AT:
      EEPROM.write(1, 2);
      waiting_at();
      break;
    case IN_AT:
      EEPROM.write(1, 3);
      in_at();
      break;
    case DEVICE_CONNECTED:
      EEPROM.write(1, 4);
      device_connected();
      break;
    case DEVICE_CONNECTED_READY:
      EEPROM.write(1, 5);
      device_connected_ready();
      break;
    case WAITING_CONNECT:
      EEPROM.write(1, 6);
      waiting_connect();
      break;
    case AT_PENDING_EXIT:
      EEPROM.write(1, 7);
      at_pending_exit();
      break;
  }
  oldstate = laststate;
  if (state == laststate) {
    delay(200); // Delay when state didn't change
  }
}

void start() {
  clearBuffer();
  delay(2000);
  Serial.print("+++");
  delay(700);
  state = WAITING_AT;
}

void waiting_at() {
  String ctext = readChars(0, "Enter AT Mode", 1000);
  if (ctext.length()) {
    state = IN_AT;
  } else {
    state = START;
    Serial.print("got text ");
    Serial.println(ctext);
  }
}

void in_at() {
  clearBuffer();
  Serial.println("AT+SETTING=DEFAULT");
  readChars(0, "OK", 100);
  Serial.println("AT+NAME=Key Holder");
  readChars(0, "OK", 100);
  Serial.println("AT+ROLE=ROLE_CENTRAL");
  readChars(0, "OK", 100);
  Serial.println("AT+CMODE=ANYONE");
  readChars(0, "OK", 100);
  if (EEPROM.read(0) == 0) {
    delay(1000);
    Serial.println("AT+NAME=Key Holder");
    EEPROM.write(0, SETUP_MAGICNUM); // Store the fact that we have programmed the device name, as the name is not stored or used unless RESTART is sent
    delay(1000);
    Serial.println("AT+RESTART");
    Serial.println("AT+EXIT");
    delay(1000);
    Serial.println("We were not restarted by the BLE controller. Something is wrong");
  }    
  state = WAITING_CONNECT;
}

void at_pending_exit() {
  String x = readChars(0, "OK", 0);
  if (!x.length())
    state = START;
  else {
    state = DEVICE_CONNECTED;
  }
  state = DEVICE_CONNECTED;
  delay(100);
  clearBuffer();
  lastTime = millis();
}

void waiting_connect() {
  delay(400);
  clearBuffer();
  Serial.println("AT+RSSI=?");
  String rssi = readChars(4, "", 100);
  if (rssi.charAt(0) != '-') {
    Serial.println("rssi corrupt" + rssi.charAt(0));
    state = START;
    return;
  }
  if (rssi == "-000") {
    return; // Still waiting for connection
  }
  clearBuffer();
  Serial.println("AT+EXIT");
  state = AT_PENDING_EXIT;
}

void device_connected() {
  unsigned long uid = 0;
  unsigned long tmp;
  unsigned long uids[TOTAL_DEVICES];
  byte j;
  if (Serial.available() > 3) {
    lastTime = millis();
    for (byte i = 0; i < 4; i++) {
      // Read the device UID from Serial
      tmp = Serial.read();
      if (tmp > 255 || tmp < 0) {
        Serial.write(0xEE);
        return;
      }
      uid |= ((unsigned long) tmp) << (i * 8UL);
    }
    clearBuffer();
    byte found = TOTAL_DEVICES;
    byte firstempty = TOTAL_DEVICES;
    bool kill = false;
    for (byte i = 0; i < TOTAL_DEVICES; i++) {
      // Read stored UIDs from EEPROM
      uids[i] = 0;
      for (j = 0; j < 4; j++) {
        // Read UID with offset (i+0xA)
        uids[i] |= ((unsigned long) EEPROM.read(i * 4 + j + 0xA)) << (j * 8);
      }
      if (uids[i] == uid) found = i;
      if (uids[i] == 0 && firstempty >= TOTAL_DEVICES) firstempty = i;
    }
    if (found >= TOTAL_DEVICES && firstempty >= TOTAL_DEVICES) {
      Serial.write(0xED); // All full, no room to pair
      goto restart; // disconnect device
    }
    Serial.write(found < TOTAL_DEVICES ? 0xFD : 0xCE);
    found = found < TOTAL_DEVICES ? found : firstempty;
    Serial.write(found);
    for (j = 0; j < 4; j++) {
      EEPROM.write(found * 4 + j + 0xA, (uid >> j * 8) & 0xFF); // Save the entry
    }
    state = DEVICE_CONNECTED_READY;
  } else if (millis() > lastTime + 10000) {
    goto restart;
  } else {
    Serial.write(0xE0); // hEllO
  }
  return;

  restart:
  delay(200);
  Serial.print("+++");
  delay(700);
  String ctext = readChars(0, "Enter AT Mode", 1000);
  if (ctext.length()) {
    lastTime = millis();
    state = IN_AT;
  } else {
    state = START;
    Serial.print("got text ");
    Serial.println(ctext);
  }
  Serial.println("AT+RESTART");
  delay(10000);
  Serial.println("Didn't get restarted!");
  state = START;
  return;
}

void device_connected_ready() {
  if (Serial.available()) {
    lastTime = millis();
    int com = Serial.read();
    clearBuffer();
    byte cmd;
    if (com > 255 || com < 0) {
      Serial.write(0xEE);
      return;
    } else {
      cmd = com;
    }
    switch (cmd) {
      case 'p':
        Serial.write(0xAC);
        break;
      case 'c':
        {
          int flag = 0;
          if (digitalRead(A0) == HIGH)
            flag |= 1;
          if (digitalRead(A1) == HIGH)
            flag |= 2;
          if (digitalRead(A2) == HIGH)
            flag |= 4;
          if (digitalRead(A3) == HIGH)
            flag |= 8;
          Serial.write(flag);
          break;
        }
      default:
        Serial.write(0xEE);
    }
  } else if (millis() > lastTime + 10000) {
    delay(200);
    Serial.print("+++");
    delay(700);
    String ctext = readChars(0, "Enter AT Mode", 1000);
    if (ctext.length()) {
      lastTime = millis();
      state = IN_AT;
    } else {
      state = START;
      Serial.print("got text ");
      Serial.println(ctext);
    }
    Serial.println("AT+NAME=Key Holder");
    delay(500);
    Serial.println("AT+RESTART");
    delay(10000);
    Serial.println("Didn't get restarted!");
    state = START;
  }
}
