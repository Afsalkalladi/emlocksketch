#include <SPI.h>
#include <MFRC522.h>

// Pin definitions
#define SS_PIN 21   // SDA
#define RST_PIN 22  // RST

MFRC522 mfrc522(SS_PIN, RST_PIN);

void setup() {
  Serial.begin(115200);
  while (!Serial);   // ESP32 safe

  SPI.begin(18, 19, 23, SS_PIN); // SCK, MISO, MOSI, SS
  mfrc522.PCD_Init();

  Serial.println("ESP32 RFID Reader Ready");
  Serial.println("Scan your RFID card...");
}

void loop() {
  // Look for new card
  if (!mfrc522.PICC_IsNewCardPresent()) {
    return;
  }

  // Select one of the cards
  if (!mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  Serial.print("UID:");
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    Serial.print(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(mfrc522.uid.uidByte[i], HEX);
  }
  Serial.println();

  // Stop reading
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}
