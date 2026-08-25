#include <Arduino.h>
#include <SPI.h>
#include <ESP32Servo.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>

// ==========================================
// 1. CẤU HÌNH & HẰNG SỐ (CONSTANTS & PINS)
// ==========================================
// Cấu hình biến toàn cục
const char* ssid = "Thang";         
const char* password = "15112004";        
const char* mqtt_server = "broker.emqx.io";    
const int mqtt_port = 1883;
const char* mqtt_topic_log = "quangthang/smartlock/log"; 
const char* mqtt_topic_cmd = "quangthang/smartlock/cmd"; 

#define TOUCH_PIN     34  
#define LED_PIN       16   
#define NUM_LEDS      3

const int SERVO_PIN = 15;
int SERVO_NEUTRAL = 1500; 
const int SERVO_OPEN = 1310;
const int SERVO_CLOSE = 1700;
const int SERVO_DELAY = 800;

const int BUZZ_PIN = 17;
const int BUZZ_CH = 6;
const int BUZZ_FREQ = 2000;
const int BUZZ_RES = 8;

const int MC38_PIN = 5; 

const int RFID_SS = 2;
const int RFID_RST = 4;
const byte SECURE_BLOCK = 4;
const byte ADMIN_UID[4] = {0xAC, 0x64, 0x91, 0x05};

const char *PREF_NS = "rfid_store";
const int MAX_CARDS = 60;

const unsigned long SLEEP_TIMEOUT = 15000UL; 
const unsigned long OTP_TIMEOUT = 600000UL; //

// ==========================================
// 2. BIẾN TOÀN CỤC & ĐỐI TƯỢNG (GLOBALS)
// ==========================================
WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastReconnectAttempt = 0;
bool currentWiFiState = false; 
unsigned long lastWiFiAttempt = 0;

const byte ROWS = 4, COLS = 4;
char keysArr[ROWS][COLS] = {
  {'1','2','3','A'}, {'4','5','6','B'},
  {'7','8','9','C'}, {'*','0','#','D'}
};
byte rowPins[ROWS] = {27,14,12,13}; 
byte colPins[COLS] = {32,33,25,26}; 
Keypad keypad = Keypad( makeKeymap(keysArr), rowPins, colPins, ROWS, COLS );

LiquidCrystal_I2C lcd(0x27, 20, 4);

MFRC522 rfid(RFID_SS, RFID_RST);
MFRC522::MIFARE_Key rfidKey;
byte myCustomKey[6] = {0x15, 0x11, 0x20, 0x04, 0x0A, 0x0B}; 
byte secretData[16] = {'Q','U','A','N','G','T','H','A','N','G','_','S','E','C','U','R'};

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
// Biến cho LED chớp Non-blocking
unsigned long lastLedTime = 0;
bool isRedOn = false;
int chaseStep = 0;

Servo doorServo;

Preferences prefs;

String inputBuf = "";
String storedPassword;
int wrongCount = 0;
int wrongFaceCount = 0;   
bool faceLocked = false;  
int wrongCardCount = 0;   
bool rfidLocked = false;
bool alarmActive = false;
bool alarmLcdPrinted = false; 
bool showingMain = false;
bool isLcdOn = true; 
unsigned long lastActivity = 0;
bool isHoldingHash = false;
bool isWaitingFaceAuth = false;
int faceAuthResult = 0; 
unsigned long faceAuthTimeout = 0;

//OTP
String otpCode = "";
unsigned long otpStartTime = 0;
bool isOtpActive = false;

// flag de check trang thai cua, tranh mo cua nhieu lan tu web
bool isDoorOperating = false; 

// ==========================================
// 3. KHAI BÁO HÀM (FORWARD DECLARATIONS)
// ==========================================
void setAllLeds(int r, int g, int b);
void blinkLeds(int r, int g, int b, int times);
void blinkAndBuzz(int r, int g, int b, int times, int buzzDuty = 180, unsigned long onTime = 200, unsigned long offTime = 150);
void handleLedChase();
void buzz(int duty, unsigned long ms);
String uidToHex(const MFRC522::Uid &u);
bool isAdmin(const MFRC522::Uid &u);

String loadPassword();
void savePassword(const String &pw);
int cardCount();
String cardAt(int i);
void setCard(int i, const String &uid);
void setCount(int n);
bool addCardToMem(const String &uidIn);
bool removeCard(const String &uidIn);
bool isAllowedInMem(const String &uidIn);

void handleWiFiAndMQTT();
void sendMQTTLog(String message);
void mqttCallback(char* topic, byte* payload, unsigned int length);

void performDoorCycle();
void openDoor(const String &who, bool admin = false);
void wrongNotify();
void startAlarm();
void stopAlarm();
void showMainPrompt();
void leaveMainUI();
void wakeUpLcdIfNeeded();
void adminMenu();

int waitForCardOrCancel(String &outUid, unsigned long timeoutMs = 15000);
bool writeSecureBlock();
bool verifySecureBlock();
bool resetSecureBlock();

void processPassword();
void triggerFaceAuth();
void keypadEvent(KeypadEvent key);

// ==========================================
// 4. CHƯƠNG TRÌNH CHÍNH (SETUP & LOOP)
// ==========================================
void setup() {
  Serial.begin(9600);
  delay(200);
  
  pinMode(MC38_PIN, INPUT_PULLUP);
  
  // Nút nhấn cơ kết hợp trở kéo 27k Ohm bên ngoài
  pinMode(TOUCH_PIN, INPUT);
  
  strip.begin();
  strip.setBrightness(50); 
  setAllLeds(255, 255, 0);

  WiFi.mode(WIFI_STA);
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback); 

  keypad.setHoldTime(3000); 
  keypad.addEventListener(keypadEvent); 

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();

  ledcSetup(BUZZ_CH, BUZZ_FREQ, BUZZ_RES);
  ledcAttachPin(BUZZ_PIN, BUZZ_CH);
  ledcWrite(BUZZ_CH, 0);

  doorServo.attach(SERVO_PIN, 500, 2400);
  doorServo.writeMicroseconds(SERVO_NEUTRAL);
  delay(200);

  SPI.begin(18, 19, 23, RFID_SS);
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);
  delay(100);

  for (byte i = 0; i < 6; i++) rfidKey.keyByte[i] = myCustomKey[i];

  prefs.begin(PREF_NS, false);
  storedPassword = loadPassword();

  WiFi.begin(ssid, password);

  showMainPrompt();
  
  lastActivity = millis(); 
}

void loop() {
  handleWiFiAndMQTT();
  if (isOtpActive && (millis() - otpStartTime > OTP_TIMEOUT)) {
    isOtpActive = false;
    otpCode = "";
  }
  // --- LOGIC ĐỌC NÚT NHẤN CƠ (EXIT BUTTON) ---
  if (digitalRead(TOUCH_PIN) == HIGH) {
    delay(50); // Chống dội phím (Debounce) cơ bản
    if (digitalRead(TOUCH_PIN) == HIGH) { // Xác nhận lại lần nữa tránh nhiễu
      if (alarmActive) stopAlarm();
      
      openDoor("", false); 
      if (!alarmActive) setAllLeds(255, 255, 0); 
      
      // Chờ người dùng thả nút nhấn ra để không bị mở liên tục
      // Cài đặt timeout tối đa 3 giây đề phòng rủi ro nút cơ kẹt cứng
      unsigned long btnWait = millis();
      while(digitalRead(TOUCH_PIN) == HIGH) {
        handleWiFiAndMQTT(); 
        delay(50); 
        if (millis() - btnWait > 3000) break;
      }
    }
  }
  // -------------------------------------------
  
  char k = keypad.getKey(); 
  if (k) {
    lastActivity = millis(); 
    wakeUpLcdIfNeeded(); 
    
    if (k == '*') {
      if (inputBuf.length()) inputBuf.remove(inputBuf.length()-1);
    } else if (k != '#') { 
      if (inputBuf.length() < 16) inputBuf += k;
    }

    if (alarmActive) {
      lcd.setCursor(0, 2);
      lcd.print("                    "); 
      lcd.setCursor(0, 2);
      String disp = "";
      for (size_t i=0; i<inputBuf.length(); i++) disp += '*';
      lcd.print(disp);
    } else {
      showMainPrompt(); 
    }
  }

  if (alarmActive) {
    wakeUpLcdIfNeeded(); 
    if (millis() - lastLedTime > 150) {
      lastLedTime = millis();
      isRedOn = !isRedOn;
      if (isRedOn) setAllLeds(255, 0, 0); else setAllLeds(0, 0, 0);
    }

    if (!alarmLcdPrinted) {
      leaveMainUI();
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("!! SECURITY ALARM !!");
      lcd.setCursor(0, 1);
      lcd.print("Admin/Pass to unlock"); 
      alarmLcdPrinted = true;
    }
    unsigned long r = millis() % 2000;
    if (r < 300) {
      ledcWrite(BUZZ_CH, 255); 
    } else {
      ledcWrite(BUZZ_CH, 0);   
    }

    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      if (isAdmin(rfid.uid)) {
        if (verifySecureBlock()) { 
          stopAlarm(); 
          rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
          setAllLeds(0, 255, 0); 
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Alarm stopped");
          buzz(160, 120); 
          delay(1500);
          setAllLeds(255, 255, 0); 
          
          wrongCount = 0; 
          inputBuf = "";
          showMainPrompt();
          return;
        } else {
          String uidHex = uidToHex(rfid.uid);
          sendMQTTLog("CLONED_WARNING: ADMIN_" + uidHex);
        }
      }
      rfid.PICC_HaltA(); rfid.PCD_StopCrypto1(); 
    }
    return; 
  }
  if (isWaitingFaceAuth) {
    wakeUpLcdIfNeeded(); 
    handleLedChase();
    if (faceAuthResult == 1) {
      isWaitingFaceAuth = false;
      wrongFaceCount = 0; 
      sendMQTTLog("GRANTED: FACE_ID_SUCCESS");
      openDoor("", false);
    } 
    else if (faceAuthResult == -1) {
      isWaitingFaceAuth = false;
      sendMQTTLog("DENIED: UNKNOWN_FACE");
      
      wrongFaceCount++;
      if (wrongFaceCount >= 5) {
        faceLocked = true;
        sendMQTTLog("FACE_LOCKED"); 
        lcd.clear(); lcd.setCursor(0,0); lcd.print("TOO MANY FAILES");
        lcd.setCursor(0,1); lcd.print("FACE IS LOCKED!");
        buzz(180, 500); delay(2000);
        setAllLeds(255, 255, 0);
        showMainPrompt();
      } else {
        wrongNotify();
      }
    } 
    else if (millis() > faceAuthTimeout) {
      isWaitingFaceAuth = false;
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("AI Timeout!");
      buzz(100, 500);
      delay(2000);
      setAllLeds(255, 255, 0);
      showMainPrompt();
    }
    return; 
  }

  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    lastActivity = millis(); 
    wakeUpLcdIfNeeded();

    String uidHex = uidToHex(rfid.uid);
    uidHex.toUpperCase();
    
    if (isAdmin(rfid.uid)) {
      if (verifySecureBlock()) {
        rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();

        rfidLocked = false; 
        wrongCardCount = 0;  
        wrongCount = 0;

        faceLocked = false;     
        wrongFaceCount = 0;
        
        sendMQTTLog("GRANTED_ADMIN: " + uidHex); 
        openDoor("", true);                      
        adminMenu();
        storedPassword = loadPassword();
      } else {
        rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
        wrongCardCount++; 
        sendMQTTLog("CLONED_WARNING: ADMIN_" + uidHex); 
        
        if (wrongCardCount >= 5) {
          rfidLocked = true;
          sendMQTTLog("RFID_LOCKED");
          lcd.clear(); lcd.setCursor(0,0); lcd.print("CARDS LOCKED!"); 
          lcd.setCursor(0,1); lcd.print("Enter Password");
          setAllLeds(255,0,0);
          buzz(180, 500);
          delay(2000);
          setAllLeds(255,255,0);
          showMainPrompt();
        } else {
          setAllLeds(255,0,0);
          lcd.clear(); lcd.setCursor(0,0); lcd.print("SECURITY ALERT!");
          lcd.setCursor(0,1); lcd.print("Fake Admin Card!");
          buzz(180, 500); delay(2000);
          setAllLeds(255, 255, 0);
          showMainPrompt();
        }
      }
    }
    else {
      if (rfidLocked) {
        rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
        lcd.clear(); lcd.setCursor(0,0); lcd.print("CARDS LOCKED!");
        lcd.setCursor(0,1); lcd.print("Enter Password");
        setAllLeds(255,0,0);
        buzz(180, 200); delay(100); buzz(180, 200);
        delay(1500);
        setAllLeds(255, 255, 0);
        showMainPrompt();
        return; 
      }

      bool inMem = isAllowedInMem(uidHex);
      bool isSecure = false;
      if (inMem) {
        isSecure = verifySecureBlock(); 
      }
      
      rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
      
      if (inMem && isSecure) {
        wrongCardCount = 0; 
        wrongFaceCount = 0; 
        faceLocked = false; 
        sendMQTTLog("GRANTED: " + uidHex);
        openDoor(uidHex, false);           
      }
      else {
        wrongCardCount++; 
        
        if (inMem && !isSecure) {
          sendMQTTLog("CLONED_WARNING: USER_" + uidHex); 
        } else {
          sendMQTTLog("DENIED_UNKNOWN_CARD: " + uidHex); 
        }
        
        if (wrongCardCount >= 5) {
          rfidLocked = true;
          sendMQTTLog("RFID_LOCKED");
          lcd.clear(); lcd.setCursor(0,0); lcd.print("CARDS LOCKED!");
          lcd.setCursor(0,1); lcd.print("Enter Password");
          setAllLeds(255,0,0);
          buzz(180,500);
          delay(2000);
          setAllLeds(255,255,0);
          showMainPrompt();
        } else {
          if (inMem && !isSecure) {
            setAllLeds(255,0,0);
             lcd.clear(); lcd.setCursor(0,0); lcd.print("SECURITY ALERT!");
             lcd.setCursor(0,1); lcd.print("Fake Card");
             buzz(180, 500);
             delay(2000);
             setAllLeds(255, 255, 0);
             showMainPrompt();
          } else {
             wrongNotify(); 
          }
        }
      }
    }
    delay(200);
  }
 
  if (isLcdOn) {
    if (millis() - lastActivity >= SLEEP_TIMEOUT) {
      lcd.noBacklight(); 
      lcd.clear();       
      isLcdOn = false;
      showingMain = false; 
    }
  }
  delay(10);
}

// ==========================================
// 5. CÁC HÀM TIỆN ÍCH & PHẦN CỨNG (UTILITIES)
// ==========================================
void setAllLeds(int r, int g, int b) {
  for(int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(r, g, b));
  strip.show(); 
}

void blinkLeds(int r, int g, int b, int times) {
  for (int i = 0; i < times; i++) {
    setAllLeds(r, g, b); delay(500);
    setAllLeds(0, 0, 0); delay(200);
  }
}

void blinkAndBuzz(int r, int g, int b, int times, int buzzDuty, unsigned long onTime, unsigned long offTime) {
  for (int i = 0; i < times; i++) {
    setAllLeds(r, g, b);
    ledcWrite(BUZZ_CH, buzzDuty); 
    delay(onTime); 

    setAllLeds(0, 0, 0);
    ledcWrite(BUZZ_CH, 0);
    delay(offTime); 
  }
}

void handleLedChase() {
  if (millis() - lastLedTime > 200) {
    lastLedTime = millis();
    strip.clear();
    int reversedIndex = (NUM_LEDS - 1) - chaseStep;
   strip.setPixelColor(reversedIndex, strip.Color(255, 255, 0));
    strip.show();
    chaseStep++;
    if (chaseStep >= NUM_LEDS) chaseStep = 0;
  }
}

void buzz(int duty, unsigned long ms) { 
  ledcWrite(BUZZ_CH, duty);
  delay(ms);
  ledcWrite(BUZZ_CH, 0);
}

String uidToHex(const MFRC522::Uid &u) { 
  String s = "";
  for (byte i=0;i<u.size;i++){
    if (u.uidByte[i] < 0x10) s += "0";
    s += String(u.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

bool isAdmin(const MFRC522::Uid &u) { 
  if (u.size != 4) return false;
  for (byte i=0;i<4;i++) if (u.uidByte[i] != ADMIN_UID[i]) return false;
  return true;
}

void showMainPrompt() { 
  showingMain = true;
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Scan card/Enter pass");
  lcd.setCursor(0,1);
  lcd.print("*:DEL #: ENTER");
  
  lcd.setCursor(0,2);
  String disp = "";
  for (size_t i=0; i<inputBuf.length(); i++) disp += '*';
  lcd.print(disp);

  lcd.setCursor(0,3);
  if (WiFi.status() == WL_CONNECTED) {
    lcd.print("WIFI: CONNECTED   ");
  } else {
    lcd.print("WIFI: DISCONNECTED");
  }
}

void leaveMainUI() { showingMain = false; } 

void wakeUpLcdIfNeeded() { 
  if (!isLcdOn) {
    lcd.backlight();
    isLcdOn = true;
    showMainPrompt();
  }
}

// ==========================================
// 6. CÁC HÀM QUẢN LÝ BỘ NHỚ (PREFERENCES)
// ==========================================
String loadPassword() { 
  String pw = prefs.getString("pw", "");
  if (pw == "") { pw = "1234"; prefs.putString("pw", pw); }
  return pw;
}

void savePassword(const String &pw){ prefs.putString("pw", pw); } 

int cardCount(){ return prefs.getInt("n", 0); } 

String cardAt(int i){ return prefs.getString(("uid"+String(i)).c_str(), ""); } 

void setCard(int i, const String &uid){ prefs.putString(("uid"+String(i)).c_str(), uid); } 

void setCount(int n){ prefs.putInt("n", n); } 

bool addCardToMem(const String &uidIn){  
  String uid = uidIn; uid.toUpperCase();
  int n = cardCount();
  for (int i=0; i<n; i++) if (cardAt(i) == uid) return false;
  if (n >= MAX_CARDS) return false;
  setCard(n, uid); setCount(n+1); return true;
}

bool removeCard(const String &uidIn){ 
  String uid = uidIn; uid.toUpperCase();
  int n = cardCount();
  int found = -1;
  for (int i=0; i<n; i++) if (cardAt(i) == uid) { found = i; break; }
  if (found == -1) return false;
  for (int i=found; i<n-1; i++) setCard(i, cardAt(i+1));
  prefs.remove(("uid"+String(n-1)).c_str());
  setCount(n-1);
  return true;
}

bool isAllowedInMem(const String &uidIn){ 
  String uid = uidIn; uid.toUpperCase();
  int n = cardCount();
  for (int i=0; i<n; i++) if (cardAt(i) == uid) return true;
  return false;
}

// ==========================================
// 7. CÁC HÀM MẠNG & MQTT (WIFI / MQTT)
// ==========================================
void handleWiFiAndMQTT() {  
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  
  if (isConnected != currentWiFiState) {
    currentWiFiState = isConnected;
    if (showingMain && !alarmActive && !isWaitingFaceAuth && isLcdOn) {
      lcd.setCursor(0,3);
      if (currentWiFiState) {
        lcd.print("WIFI: CONNECTED   ");
      } else {
        lcd.print("WIFI: DISCONNECTED");
      }
    }
  }

  if (!isConnected) {
    unsigned long now = millis();
    if (now - lastWiFiAttempt > 5000) { 
      WiFi.disconnect();
      WiFi.begin(ssid, password);
      lastWiFiAttempt = now;
    }
  } 
  else {
    if (!mqttClient.connected()) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt > 5000) {
        lastReconnectAttempt = now;
        String clientId = "ESP32_Lock_" + String(random(0xffff), HEX);
        if (mqttClient.connect(clientId.c_str())) {
          lastReconnectAttempt = 0;
          mqttClient.subscribe(mqtt_topic_cmd);
        }
      }
    } else {
      mqttClient.loop(); 
    }
  }
}

void sendMQTTLog(String message) { 
  if (mqttClient.connected()) {
    mqttClient.publish(mqtt_topic_log, message.c_str());
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) { 
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  if (String(topic) == String(mqtt_topic_cmd)) {

    if (message.startsWith("WEB_DELETE_CARD: ")) {
      String uidToDelete = message.substring(17); 
      uidToDelete.trim();
      removeCard(uidToDelete);
    } 
    else if (message == "FACE_SUCCESS") {
      faceAuthResult = 1;
    } 
    else if (message == "FACE_DENIED") {
      faceAuthResult = -1;
    }
    else if (message == "WEB_UNLOCK") {
      if (!isDoorOperating) {
      openDoor("", false); 
    }}
    else if (message == "WEB_STOP_ALARM") {
      if (alarmActive) {
        Serial.println("Nhận lệnh tắt báo động từ Web!");
        stopAlarm(); 
        lcd.clear(); lcd.setCursor(0, 0); lcd.print("Web Stopped!");
        buzz(160, 120); delay(1500);
        wrongCount = 0; inputBuf = ""; showMainPrompt(); 
      }
    }
    else if (message.startsWith("WEB_SET_OTP: ")) {
      otpCode = message.substring(13);
      otpCode.trim();
      isOtpActive = true;
      otpStartTime = millis();
    }
    else if (message.startsWith("WEB_CHANGE_PASS: ")) {
      String newDoorPass = message.substring(17); 
      newDoorPass.trim(); 
      
      savePassword(newDoorPass);       
      storedPassword = newDoorPass;    
  
      lcd.clear(); 
      lcd.setCursor(0, 0); 
      lcd.print("Web changed pass");
      buzz(160, 150); delay(150); buzz(160, 150);
      delay(1000);
      
      if (!alarmActive) {
        showMainPrompt();
      }
    }
  }
}

// ==========================================
// 8. CÁC HÀM XỬ LÝ BLOCK BẢO MẬT RFID
// ==========================================
bool writeSecureBlock() { 
  return true; //////////////////////////////
  MFRC522::StatusCode status;
  MFRC522::MIFARE_Key defaultKey;
  for (byte i = 0; i < 6; i++) defaultKey.keyByte[i] = 0xFF; 

  status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, 7, &defaultKey, &(rfid.uid));

  if (status != MFRC522::STATUS_OK) {
    rfid.PCD_StopCrypto1(); 
    rfid.PICC_HaltA();      
    
    delay(50); 
    
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
       return false;
    }

    status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, 7, &rfidKey, &(rfid.uid));
  }

  if (status != MFRC522::STATUS_OK) {
    return false;
  }

  byte sectorTrailerData[16] = {
    myCustomKey[0], myCustomKey[1], myCustomKey[2], myCustomKey[3], myCustomKey[4], myCustomKey[5],
    0xFF, 0x07, 0x80, 0x69,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
  };

  status = rfid.MIFARE_Write(7, sectorTrailerData, 16);
  if (status != MFRC522::STATUS_OK) return false;

  status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, SECURE_BLOCK, &rfidKey, &(rfid.uid));
  if (status != MFRC522::STATUS_OK) return false;

  status = rfid.MIFARE_Write(SECURE_BLOCK, secretData, 16);
  if (status != MFRC522::STATUS_OK) return false;

  return true; 
}

bool verifySecureBlock() { 
  return true; /////////////////////////////////////////////////////////
  MFRC522::StatusCode status;
  byte buffer[18];
  byte size = sizeof(buffer);
  
  for (int attempt = 0; attempt < 5; attempt++) {
    status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, SECURE_BLOCK, &rfidKey, &(rfid.uid));
    
    if (status == MFRC522::STATUS_OK) {
      status = rfid.MIFARE_Read(SECURE_BLOCK, buffer, &size);
      if (status == MFRC522::STATUS_OK) {
        bool match = true;
        for (byte i = 0; i < 16; i++) {
          if (buffer[i] != secretData[i]) {
            match = false;
            break;
          }
        }
        if (match) return true; 
      }
    }
    
    rfid.PCD_StopCrypto1(); 
    delay(20); 
  }
  
  return false; 
}

bool resetSecureBlock() { 
  return true; ///////////////////////
  MFRC522::StatusCode status;

  status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, 7, &rfidKey, &(rfid.uid));
  if (status != MFRC522::STATUS_OK) return false;

  byte factoryTrailer[16] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0x07, 0x80, 0x69,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
  };

  status = rfid.MIFARE_Write(7, factoryTrailer, 16);
  if (status != MFRC522::STATUS_OK) return false;

  MFRC522::MIFARE_Key defaultKey;
  for (byte i = 0; i < 6; i++) defaultKey.keyByte[i] = 0xFF;

  status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, SECURE_BLOCK, &defaultKey, &(rfid.uid));
  if (status == MFRC522::STATUS_OK) {
    byte emptyBlock[16] = {0}; 
    rfid.MIFARE_Write(SECURE_BLOCK, emptyBlock, 16); 
  }

  return true;
}

// ==========================================
// 9. CÁC HÀM LOGIC HOẠT ĐỘNG (CORE LOGIC)
// ==========================================
void performDoorCycle() { 
  isDoorOperating = true;
  wakeUpLcdIfNeeded();
  leaveMainUI();
  lcd.clear();
  
  doorServo.writeMicroseconds(SERVO_OPEN);
  delay(SERVO_DELAY);
  doorServo.writeMicroseconds(SERVO_NEUTRAL);
  
  unsigned long startTime = millis();
  lcd.setCursor(0, 0);
  lcd.print("Opening door     ");

  int VAL_NEAR = LOW;  // Nam châm gần (Cửa ĐÓNG)
  int VAL_FAR  = HIGH; // Nam châm tách (Cửa MỞ)

  int step = 0; 
  delay(1000); 

  int lastRawState = digitalRead(MC38_PIN);
  unsigned long lastDebounceTime = millis();
  int stableState = lastRawState;

  while (true) {
    handleWiFiAndMQTT(); 
    
    // --- Lọc nhiễu 300ms ---
    int rawState = digitalRead(MC38_PIN);
    if (rawState != lastRawState) {
      lastDebounceTime = millis(); 
      lastRawState = rawState;
    }
    if ((millis() - lastDebounceTime) > 300) {
      stableState = rawState;
    }
    // -----------------------

    if (step == 0) { 
      if (stableState == VAL_FAR) { 
        step = 1; 
        lcd.setCursor(0, 0);
        lcd.print("Door is opened       ");
        startTime = millis(); 
      } 
      else if (millis() - startTime > 15000UL) { 
        lcd.setCursor(0, 0);
        lcd.print("Timeout! Auto Lock ");
        delay(1500);
        break; 
      }
    }
    else if (step == 1) { 
      handleLedChase(); 
      
      if (stableState == VAL_NEAR) { 
        lcd.setCursor(0, 0);
        lcd.print("Door is closed    ");
        delay(2000); 
        break; 
      } 
      else if (millis() - startTime > 15000UL) { 
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Please close door!");
        blinkAndBuzz(255, 255, 0, 3, 200, 300, 200); 
        
        startTime = millis(); 
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Door is opened       ");
      }
    }
    delay(20); 
  }
  
  setAllLeds(255, 255, 0);
  
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Closing door...");
  doorServo.writeMicroseconds(SERVO_CLOSE);
  delay(SERVO_DELAY);
  doorServo.writeMicroseconds(SERVO_NEUTRAL);
  
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);
  
  lastActivity = millis(); 
  showMainPrompt();
  isDoorOperating = false;
}

void openDoor(const String &who, bool admin) { 
  wakeUpLcdIfNeeded();
  leaveMainUI();
  lcd.clear();
  lcd.setCursor(0,0);
  
  if (admin) { 
    lcd.print("Welcome THANG HUYNH"); 
    setAllLeds(0, 255, 0);
    buzz(200,200); 
    delay(1000);
  } else { 
    lcd.print("Welcome"); 
    if (who != "") {
      lcd.setCursor(0,1);
      lcd.print(who); 
    }
    setAllLeds(0, 255, 0);
    buzz(120,150); 
    delay(1000); 
    performDoorCycle();
  }
}

void wrongNotify() { 
  wakeUpLcdIfNeeded();
  leaveMainUI();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("LOCK !!");
  blinkAndBuzz(255, 0, 0, 2, 180, 200, 150);
  setAllLeds(255, 255, 0);
  lastActivity = millis(); 
  showMainPrompt();
}

void startAlarm() {  
  wakeUpLcdIfNeeded();
  alarmActive = true; 
  alarmLcdPrinted = false; 
  sendMQTTLog("PASS_LOCKED");
}

void stopAlarm() { 
  alarmActive = false; 
  alarmLcdPrinted = false;
  lastActivity = millis(); 
}

void adminMenu() { 
  setAllLeds(0, 255, 0);
  wakeUpLcdIfNeeded();
  leaveMainUI();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("ADMIN MODE");
  lcd.setCursor(0,1);
  lcd.print("1:CHG 2:DEL 3:ADD");
  lcd.setCursor(0,2);
  lcd.print("C:Exit");
  
  unsigned long start = millis();
  while (millis() - start < 15000UL) { 
    handleWiFiAndMQTT();
    char k = keypad.getKey();
    if (!k) { delay(30); continue; }
    
    lastActivity = millis(); 
    wakeUpLcdIfNeeded();
    
    if (k == 'C' || k == 'c' || k == 'D') {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Exit Admin");
      delay(300); showMainPrompt(); setAllLeds(255, 255, 0); return;
    }
    
    if (k == '1') {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("CHANGE PASS");
      lcd.setCursor(0,1); lcd.print("New pass:");
      lcd.setCursor(0,3); lcd.print("*:Del #:Ent C:Exit");
      
      String newPw = "";
      unsigned long t0 = millis();
      while (millis() - t0 < 15000UL) {
        handleWiFiAndMQTT();
        char kk = keypad.getKey();
        if (kk) {
          t0 = millis(); 
          lastActivity = millis(); 
          wakeUpLcdIfNeeded();
          
          if (kk == 'C' || kk == 'c' || kk == 'D') {
            lcd.clear(); lcd.setCursor(0,0); lcd.print("Canceled"); 
            delay(600); showMainPrompt(); setAllLeds(255, 255, 0); return;
          }
          
          if (kk == '#') { 
            if (newPw.length() > 0) {
              savePassword(newPw); storedPassword = newPw;
              sendMQTTLog("ADMIN_CHANGED_PASSWORD");
              lcd.clear(); lcd.setCursor(0,0); lcd.print("SAVED PWD!");setAllLeds(0,255,0);buzz(180,200);
               delay(800); setAllLeds(255, 255, 0); showMainPrompt(); return;
            } else {
              lcd.clear(); lcd.setCursor(0,0); lcd.print("Pass empty!");  blinkAndBuzz(255, 0, 0, 2, 180, 200, 150); setAllLeds(255, 255, 0);
              delay(800); showMainPrompt(); return;
            }
          } else if (kk == '*') { 
            if (newPw.length()) newPw.remove(newPw.length()-1);
          } else { 
            if (newPw.length() < 16) newPw += kk;
          }
          
          lcd.setCursor(0,2);
          String ds = ""; for (size_t i=0; i<newPw.length(); i++) ds += '*';
          lcd.print("                "); lcd.setCursor(0,2); lcd.print(ds);
        }
        delay(30);
      }
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Timeout! Cancel"); delay(800); showMainPrompt(); setAllLeds(255, 255, 0); return;
    }
    
    if (k == '2') {
      lcd.clear(); 
      lcd.setCursor(0,0); lcd.print("DEL TAG: Scan");
      lcd.setCursor(0,1); lcd.print("C: Cancel");
      
      String uid;
      int status = waitForCardOrCancel(uid, 15000UL); 
      
      if (status == 1) { 
        uid.toUpperCase();

        if (!isAllowedInMem(uid)) {
          lcd.clear(); 
          lcd.setCursor(0,0); 
          lcd.print("Not found"); 
         blinkAndBuzz(255, 0, 0, 2, 180, 200, 150);
        setAllLeds(255, 255, 0);
          rfid.PICC_HaltA(); rfid.PCD_StopCrypto1(); 
          delay(900); showMainPrompt(); return;
        }

        if (resetSecureBlock()) {
          if (removeCard(uid)) {
            sendMQTTLog("ADMIN_DELETED_CARD: " + uid);
            lcd.clear(); lcd.setCursor(0,0); lcd.print("DELETED"); blinkAndBuzz(0, 255, 0, 1, 160, 120, 150); setAllLeds (0,255,0);
          }
        } else {
          lcd.clear(); lcd.setCursor(0,0); lcd.print("Reset Key Loi!");
          lcd.setCursor(0,1); lcd.print("The khong khop Key"); buzz(60,200);
          blinkLeds(255, 0, 0, 1); 
          setAllLeds(255, 0, 0);
        }
        
        rfid.PICC_HaltA(); rfid.PCD_StopCrypto1(); 
        delay(900); showMainPrompt(); setAllLeds(255, 255, 0); return;
      } 
      else if (status == -1) { 
        lcd.clear(); lcd.setCursor(0,0); lcd.print("Canceled"); delay(600); showMainPrompt(); setAllLeds(255, 255, 0); return;
      } 
      else { 
        lcd.clear(); lcd.setCursor(0,0); lcd.print("No card"); delay(700); showMainPrompt(); setAllLeds(255, 255, 0);  return;
      }
    }
    
    if (k == '3') {
      lcd.clear(); 
      lcd.setCursor(0,0); lcd.print("ADD TAG: Scan");
      lcd.setCursor(0,1); lcd.print("C: Cancel");
      
      String uid;
      int status = waitForCardOrCancel(uid, 15000UL); 
      
      if (status == 1) { 
        uid.toUpperCase();

        if (isAllowedInMem(uid)) {
          lcd.clear();
          lcd.setCursor(0,0); lcd.print("Already exists");  blinkAndBuzz(255, 0, 0, 2, 180, 200, 150);  setAllLeds(255, 255, 0);
          rfid.PICC_HaltA(); rfid.PCD_StopCrypto1(); 
          delay(1500);  showMainPrompt(); ; return;
        }

        if (writeSecureBlock()) {
          if (addCardToMem(uid)) {
            lcd.clear(); 
            lcd.setCursor(0, 0); 
            lcd.print("Look at Camera!"); 

            
            for (int i = 3; i > 0; i--) {
              lcd.setCursor(0, 1); 
              lcd.print("Capturing in "); lcd.print(i); lcd.print("s ");

              strip.clear(); 
       
              for(int j = 0; j < i; j++) {
                strip.setPixelColor(j, strip.Color(255, 255, 0)); 
              }
              strip.show();

              delay(1000);     
            }
            
            sendMQTTLog("ADMIN_ADDED_CARD: " + uid);
            lcd.clear(); lcd.setCursor(0, 0);   lcd.print("Added:");   lcd.setCursor(0, 1); lcd.print(uid); blinkAndBuzz(0, 255, 0, 1, 160, 120, 150); setAllLeds (0,255,0);
          } else { 
            lcd.clear(); lcd.setCursor(0, 0);  lcd.print("Exists/Full"); 
            buzz(60, 200); 
          }
        } else {
          lcd.clear(); lcd.setCursor(0,0); lcd.print("Loi Ghi Sector");
          lcd.setCursor(0,1); lcd.print("The da duoc khoa"); buzz(60,200);
        }
        rfid.PICC_HaltA(); rfid.PCD_StopCrypto1(); 
        delay(900); showMainPrompt();  setAllLeds(255, 255, 0); return;
      } 
      else if (status == -1) { 
        lcd.clear(); lcd.setCursor(0,0); lcd.print("Canceled"); delay(600); showMainPrompt();setAllLeds(255, 255, 0); return;
      } 
      else { 
        lcd.clear(); lcd.setCursor(0,0); lcd.print("No card"); delay(700); showMainPrompt();setAllLeds(255, 255, 0); return;
      }
    }
  }
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Exit admin"); delay(600); showMainPrompt(); setAllLeds(255, 255, 0); return;
}

int waitForCardOrCancel(String &outUid, unsigned long timeoutMs) { 
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    handleWiFiAndMQTT(); 
    char k = keypad.getKey();
    if (k) {
      lastActivity = millis(); 
      wakeUpLcdIfNeeded();
      if (k == 'C' || k == 'c' || k == 'D') return -1; 
    }

    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      lastActivity = millis();
      wakeUpLcdIfNeeded();
      outUid = uidToHex(rfid.uid);
      return 1; 
    }
    delay(30);
  }
  return 0; 
}

void processPassword() { 
  if (inputBuf.length() == 0) return;
  
  bool passOk = (inputBuf == storedPassword);
  bool otpOk = (isOtpActive && inputBuf == otpCode);

  if (passOk || otpOk) {
    if (otpOk) {
      // Nếu mở bằng OTP -> Vô hiệu hóa OTP ngay lập tức
      isOtpActive = false;
      otpCode = "";
    } else {
      sendMQTTLog("GRANTED: PASSWORD");
    }

    wrongCount = 0;
    wrongCardCount = 0;  
    wrongFaceCount = 0; 
    rfidLocked = false;  
    faceLocked = false;
    
    if (alarmActive) {
      stopAlarm(); 
    }
    openDoor("", false);      
  } else {
    if (!alarmActive) { 
      wrongCount++;
      if (wrongCount >= 5) { 
        startAlarm();
        wrongCount = 0;
      } else {
        wrongNotify();
      }
    } else {
      buzz(180, 200);
    }
  }
  inputBuf = "";
  if (!alarmActive) {
    showMainPrompt(); 
  } else {
    lcd.setCursor(0, 2);
    lcd.print("                    ");
  }
}

void triggerFaceAuth() { 
  wakeUpLcdIfNeeded();
  leaveMainUI();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Authenticating...");
  lcd.setCursor(0, 1);
  lcd.print("Please wait...");
  
  buzz(100, 150); delay(100); buzz(100, 150);

  sendMQTTLog("REQUEST_FACE_AUTH: HOLD"); 
  
  isWaitingFaceAuth = true;
  faceAuthResult = 0;
  faceAuthTimeout = millis() + 15000; 
}

void keypadEvent(KeypadEvent key) {   
  lastActivity = millis();
  wakeUpLcdIfNeeded();
  
  if (key == '#') {
    switch (keypad.getState()) {
      case HOLD:
        isHoldingHash = true;
        if (!alarmActive && !rfidLocked && !faceLocked) { 
          triggerFaceAuth();
        } 
        else if (!alarmActive && (rfidLocked || faceLocked)) {
          leaveMainUI();
          lcd.clear();
          lcd.setCursor(0,0);
          lcd.print("FACE LOCKED!");blinkLeds(255, 0, 0, 3);
          lcd.setCursor(0,1);
          lcd.print("Use Card or Pass");
          buzz(180, 200); delay(100); buzz(180, 200);
          delay(1500);
          showMainPrompt();
        }
        break;
      case RELEASED:
        if (!isHoldingHash && !isWaitingFaceAuth) {
          processPassword(); 
        }
        isHoldingHash = false;
        break;
      case IDLE:
      case PRESSED:
        break;
    }
  }
}