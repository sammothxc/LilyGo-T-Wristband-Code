#include <pcf8563.h>
#include <TFT_eSPI.h> // Graphics and font library for ST7735 driver chip
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include "sensor.h"
#include "esp_adc_cal.h"
#include "ttgo.h"
#include "charge.h"
#include <SparkFunLSM9DS1.h>

//  git clone -b development https://github.com/tzapu/WiFiManager.git
//#include <WiFiManager.h>         //https://github.com/tzapu/WiFiManager         //COMPILE ERROR

 #define FACTORY_HW_TEST     //! Test RTC and WiFi scan when enabled
 #define ARDUINO_OTA_UPDATE      //! Enable this line OTA update IF YOU EXPERIENCE ERRORS ON STARTUP, COMMENT THIS OUT

#ifndef ST7735_SLPIN
#define ST7735_SLPIN    0x10
#define ST7735_DISPOFF  0x28
#endif

#define TP_PIN_PIN          33
#define I2C_SDA_PIN         21
#define I2C_SCL_PIN         22
#define RTC_INT_PIN         34
#define BATT_ADC_PIN        35
#define TP_PWR_PIN          25
#define LED_PIN             4
#define CHARGE_PIN          32
#define INT1_PIN_THS        38
#define INT2_PIN_DRDY       39
#define INTM_PIN_THS        37
#define RDYM_PIN            36


TFT_eSPI tft = TFT_eSPI();  // Invoke library, pins defined in User_Setup.h
PCF8563_Class rtc;
//WiFiManager wifiManager;                                                      //COMPILE ERROR
LSM9DS1 imu; // Create an LSM9DS1 object to use from here on.


char buff[256];
bool rtcIrq = false;
bool initial = 1;
bool otaStart = false;

uint8_t func_select = 0;
uint8_t omm = 99;
uint8_t xcolon = 0;
uint32_t targetTime = 0;       // for next 1 second timeout
uint32_t colour = 0;
int vref = 1100;

bool pressed = false;
uint32_t pressedTime = 0;
bool charge_indication = false;
bool charge_show = false;
uint8_t hh, mm, ss ;

#ifdef ARDUINO_OTA_UPDATE
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#endif


void configureLSM9DS1Interrupts()
{
    /////////////////////////////////////////////
    // Configure INT1 - Gyro & Accel Threshold //
    /////////////////////////////////////////////
    // For more information on setting gyro interrupt, threshold,
    // and configuring the intterup, see the datasheet.
    // We'll configure INT_GEN_CFG_G, INT_GEN_THS_??_G,
    // INT_GEN_DUR_G, and INT1_CTRL.
    // 1. Configure the gyro interrupt generator:
    //  - ZHIE_G: Z-axis high event (more can be or'd together)
    //  - false: and/or (false = OR) (not applicable)
    //  - false: latch interrupt (false = not latched)
    imu.configGyroInt(ZHIE_G, false, false);
    // 2. Configure the gyro threshold
    //   - 500: Threshold (raw value from gyro)
    //   - Z_AXIS: Z-axis threshold
    //   - 10: duration (based on ODR)
    //   - true: wait (wait duration before interrupt goes low)
    imu.configGyroThs(500, Z_AXIS, 10, true);
    // 3. Configure accelerometer interrupt generator:
    //   - XHIE_XL: x-axis high event
    //     More axis events can be or'd together
    //   - false: OR interrupts (N/A, since we only have 1)
    imu.configAccelInt(XHIE_XL, false);
    // 4. Configure accelerometer threshold:
    //   - 20: Threshold (raw value from accel)
    //     Multiply this value by 128 to get threshold value.
    //     (20 = 2600 raw accel value)
    //   - X_AXIS: Write to X-axis threshold
    //   - 10: duration (based on ODR)
    //   - false: wait (wait [duration] before interrupt goes low)
    imu.configAccelThs(20, X_AXIS, 1, false);
    // 5. Configure INT1 - assign it to gyro interrupt
    //   - XG_INT1: Says we're configuring INT1
    //   - INT1_IG_G | INT1_IG_XL: Sets interrupt source to
    //     both gyro interrupt and accel
    //   - INT_ACTIVE_LOW: Sets interrupt to active low.
    //         (Can otherwise be set to INT_ACTIVE_HIGH.)
    //   - INT_PUSH_PULL: Sets interrupt to a push-pull.
    //         (Can otherwise be set to INT_OPEN_DRAIN.)
    imu.configInt(XG_INT1, INT1_IG_G | INT_IG_XL, INT_ACTIVE_LOW, INT_PUSH_PULL);

    ////////////////////////////////////////////////
    // Configure INT2 - Gyro and Accel Data Ready //
    ////////////////////////////////////////////////
    // Configure interrupt 2 to fire whenever new accelerometer
    // or gyroscope data is available.
    // Note XG_INT2 means configuring interrupt 2.
    // INT_DRDY_XL is OR'd with INT_DRDY_G
    imu.configInt(XG_INT2, INT_DRDY_XL | INT_DRDY_G, INT_ACTIVE_LOW, INT_PUSH_PULL);

    //////////////////////////////////////
    // Configure Magnetometer Interrupt //
    //////////////////////////////////////
    // 1. Configure magnetometer interrupt:
    //   - XIEN: axis to be monitored. Can be an or'd combination
    //     of XIEN, YIEN, or ZIEN.
    //   - INT_ACTIVE_LOW: Interrupt goes low when active.
    //   - true: Latch interrupt
    imu.configMagInt(XIEN, INT_ACTIVE_LOW, true);
    // 2. Configure magnetometer threshold.
    //   There's only one threshold value for all 3 mag axes.
    //   This is the raw mag value that must be exceeded to
    //   generate an interrupt.
    imu.configMagThs(10000);

}

uint16_t setupIMU()
{
    // Set up our Arduino pins connected to interrupts.
    // We configured all of these interrupts in the LSM9DS1
    // to be active-low.
    pinMode(INT2_PIN_DRDY, INPUT);
    pinMode(INT1_PIN_THS, INPUT);
    pinMode(INTM_PIN_THS, INPUT);

    // The magnetometer DRDY pin (RDY) is not configurable.
    // It is active high and always turned on.
    pinMode(RDYM_PIN, INPUT);

    // gyro.latchInterrupt controls the latching of the
    // gyro and accelerometer interrupts (INT1 and INT2).
    // false = no latching
    imu.settings.gyro.latchInterrupt = false;
    // Set gyroscope scale to +/-245 dps:
    imu.settings.gyro.scale = 245;
    // Set gyroscope (and accel) sample rate to 14.9 Hz
    imu.settings.gyro.sampleRate = 1;
    // Set accelerometer scale to +/-2g
    imu.settings.accel.scale = 2;
    // Set magnetometer scale to +/- 4g
    imu.settings.mag.scale = 4;
    // Set magnetometer sample rate to 0.625 Hz
    imu.settings.mag.sampleRate = 0;

    // Call imu.begin() to initialize the sensor and instill
    // it with our new settings.
    bool r =  imu.begin(LSM9DS1_AG_ADDR(1), LSM9DS1_M_ADDR(1), Wire); // set addresses and wire port
    if (r) {
        imu.sleepGyro(false);
        return true;
    }
    return false;
}



/*                                                                        //COMPILE ERROR
void configModeCallback (WiFiManager *myWiFiManager)
{
    Serial.println("Entered config mode");
    Serial.println(WiFi.softAPIP());
    //if you used auto generated SSID, print it
    Serial.println(myWiFiManager->getConfigPortalSSID());

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("Connect hotspot name ",  20, tft.height() / 2 - 20);
    tft.drawString("configure wrist",  35, tft.height() / 2  + 20);
    tft.setTextColor(TFT_GREEN);
    tft.drawString("\"T-Wristband\"",  40, tft.height() / 2 );

}
*/

void scanI2Cdevice(void)
{
    uint8_t err, addr;
    int nDevices = 0;
    for (addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        err = Wire.endTransmission();
        if (err == 0) {
            Serial.print("I2C device found at address 0x");
            if (addr < 16)
                Serial.print("0");
            Serial.print(addr, HEX);
            Serial.println(" !");
            nDevices++;
        } else if (err == 4) {
            Serial.print("Unknow error at address 0x");
            if (addr < 16)
                Serial.print("0");
            Serial.println(addr, HEX);
        }
    }
    if (nDevices == 0)
        Serial.println("No I2C devices found\n");
    else
        Serial.println("Done\n");
}


void wifi_scan()
{
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);

    tft.drawString("Scan Network", tft.width() / 2, tft.height() / 2);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    int16_t n = WiFi.scanNetworks();
    tft.fillScreen(TFT_BLACK);
    if (n == 0) {
        tft.drawString("no networks found", tft.width() / 2, tft.height() / 2);
    } else {
        tft.setTextDatum(TL_DATUM);
        tft.setCursor(0, 0);
        for (int i = 0; i < n; ++i) {
            sprintf(buff,
                    "[%d]:%s(%d)",
                    i + 1,
                    WiFi.SSID(i).c_str(),
                    WiFi.RSSI(i));
            Serial.println(buff);
            tft.println(buff);
        }
    }
    WiFi.mode(WIFI_OFF);
}


void drawProgressBar(uint16_t x0, uint16_t y0, uint16_t w, uint16_t h, uint8_t percentage, uint16_t frameColor, uint16_t barColor)
{
    if (percentage == 0) {
        tft.fillRoundRect(x0, y0, w, h, 3, TFT_BLACK);
    }
    uint8_t margin = 2;
    uint16_t barHeight = h - 2 * margin;
    uint16_t barWidth = w - 2 * margin;
    tft.drawRoundRect(x0, y0, w, h, 3, frameColor);
    tft.fillRect(x0 + margin, y0 + margin, barWidth * percentage / 100.0, barHeight, barColor);
}


void factoryTest()
{
    scanI2Cdevice();
    delay(2000);

    tft.fillScreen(TFT_BLACK);
    tft.drawString("RTC Interrupt self test", 0, 0);

    int yy = 2019, mm = 5, dd = 15, h = 2, m = 10, s = 0;
    rtc.begin(Wire);
    rtc.setDateTime(yy, mm, dd, h, m, s);
    delay(500);
    RTC_Date dt = rtc.getDateTime();
    if (dt.year != yy || dt.month != mm || dt.day != dd || dt.hour != h || dt.minute != m) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.fillScreen(TFT_BLACK);
        tft.drawString("Write DateTime FAIL", 0, 0);
    } else {
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.fillScreen(TFT_BLACK);
        tft.drawString("Write DateTime PASS", 0, 0);
    }

    delay(2000);

    //! RTC Interrupt Test
    pinMode(RTC_INT_PIN, INPUT_PULLUP); //need change to rtc_pin
    attachInterrupt(RTC_INT_PIN, [] {
        rtcIrq = 1;
    }, FALLING);

    rtc.disableAlarm();

    rtc.setDateTime(2019, 4, 7, 9, 5, 57);

    rtc.setAlarmByMinutes(6);

    rtc.enableAlarm();

    for (;;) {
        snprintf(buff, sizeof(buff), "%s", rtc.formatDateTime());
        Serial.print("\t");
        Serial.println(buff);
        tft.fillScreen(TFT_BLACK);
        tft.drawString(buff, 0, 0);

        if (rtcIrq) {
            rtcIrq = 0;
            detachInterrupt(RTC_INT_PIN);
            rtc.resetAlarm();
            break;
        }
        delay(1000);
    }
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.fillScreen(TFT_BLACK);
    tft.drawString("RTC Interrupt PASS", 0, 0);
    delay(2000);

    wifi_scan();
    delay(2000);
}

/*                                                                    //COMPILE ERROR
void setupWiFi()
{
#ifdef ARDUINO_OTA_UPDATE
    WiFiManager wifiManager;
    //set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
    wifiManager.setAPCallback(configModeCallback);
    wifiManager.setBreakAfterConfig(true);          // Without this saveConfigCallback does not get fired
    wifiManager.autoConnect("T-Wristband");
#endif
}
*/

void setupOTA()
{
#ifdef ARDUINO_OTA_UPDATE
    // Port defaults to 3232
    // ArduinoOTA.setPort(3232);

    // Hostname defaults to esp3232-[MAC]
    ArduinoOTA.setHostname("T-Wristband");

    // No authentication by default
    // ArduinoOTA.setPassword("admin");

    // Password can be set with it's md5 value as well
    // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
    // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
            type = "sketch";
        else // U_SPIFFS
            type = "filesystem";

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        Serial.println("Start updating " + type);
        otaStart = true;
        tft.fillScreen(TFT_BLACK);
        tft.drawString("Updating...", tft.width() / 2 - 20, 55 );
    })
    .onEnd([]() {
        Serial.println("\nEnd");
        delay(500);
    })
    .onProgress([](unsigned int progress, unsigned int total) {
        // Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
        int percentage = (progress / (total / 100));
        tft.setTextDatum(TC_DATUM);
        tft.setTextPadding(tft.textWidth(" 888% "));
        tft.drawString(String(percentage) + "%", 145, 35);
        drawProgressBar(10, 30, 120, 15, percentage, TFT_WHITE, TFT_BLUE);
    })
    .onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");

        tft.fillScreen(TFT_BLACK);
        tft.drawString("Update Failed", tft.width() / 2 - 20, 55 );
        delay(3000);
        otaStart = false;
        initial = 1;
        targetTime = millis() + 1000;
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(TL_DATUM);
        omm = 99;
    });

    ArduinoOTA.begin();
#endif
}


void setupADC()
{
    esp_adc_cal_characteristics_t adc_chars;
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize((adc_unit_t)ADC_UNIT_1, (adc_atten_t)ADC1_CHANNEL_6, (adc_bits_width_t)ADC_WIDTH_BIT_12, 1100, &adc_chars);
    //Check type of calibration value used to characterize ADC
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        Serial.printf("eFuse Vref:%u mV", adc_chars.vref);
        vref = adc_chars.vref;
    } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
        Serial.printf("Two Point --> coeff_a:%umV coeff_b:%umV\n", adc_chars.coeff_a, adc_chars.coeff_b);
    } else {
        Serial.println("Default Vref: 1100mV");
    }
}

void setupRTC()
{
    rtc.begin(Wire);
    //Check if the RTC clock matches, if not, use compile time
    rtc.check();

    RTC_Date datetime = rtc.getDateTime();
    hh = datetime.hour;
    mm = datetime.minute;
    ss = datetime.second;
}

void setup(void)
{
    Serial.begin(115200);

    tft.init();
    tft.setRotation(1);
    tft.setSwapBytes(true);
    tft.pushImage(0, 0,  160, 80, ttgo);

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(400000);

#ifdef FACTORY_HW_TEST
    factoryTest();
#endif

    setupRTC();

    // Turn on the IMU with configureIMU() (defined above)
    // check the return status of imu.begin() to make sure
    // it's connected.
    uint16_t status = setupIMU();
    if (status == false) {
        Serial.print("Failed to connect to IMU: 0x");
        Serial.println(status, HEX);
        while (1) ;
    }

    // After turning the IMU on, configure the interrupts:
    configureLSM9DS1Interrupts();

    setupADC();

//    setupWiFi();                            //COMPILE ERROR

    setupOTA();

    tft.fillScreen(TFT_BLACK);

    tft.setTextColor(TFT_YELLOW, TFT_BLACK); // Note: the new fonts do not draw the background colour

    targetTime = millis() + 1000;

    pinMode(TP_PIN_PIN, INPUT);
    //! Must be set to pull-up output mode in order to wake up in deep sleep mode
    pinMode(TP_PWR_PIN, PULLUP);
    digitalWrite(TP_PWR_PIN, HIGH);

    pinMode(LED_PIN, OUTPUT);

    pinMode(CHARGE_PIN, INPUT_PULLUP);
    attachInterrupt(CHARGE_PIN, [] {
        charge_indication = true;
    }, CHANGE);

    if (digitalRead(CHARGE_PIN) == LOW) {
        charge_indication = true;
    }
}

String getVoltage()
{
    uint16_t v = analogRead(BATT_ADC_PIN);
    float battery_voltage = ((float)v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0);
    return String(battery_voltage) + "V";
}

void RTC_Show()
{
    if (targetTime < millis()) {
        RTC_Date datetime = rtc.getDateTime();
        hh = datetime.hour;
        mm = datetime.minute;
        ss = datetime.second;
        // Serial.printf("hh:%d mm:%d ss:%d\n", hh, mm, ss);
        targetTime = millis() + 1000;
        if (ss == 0 || initial) {
            initial = 0;
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.setCursor (8, 60);
            tft.print(__DATE__); // This uses the standard ADAFruit small font
        }

        tft.setTextColor(TFT_BLUE, TFT_BLACK);
        tft.drawCentreString(getVoltage(), 120, 60, 1); // Next size up font 2


        // Update digital time
        uint8_t xpos = 6;
        uint8_t ypos = 0;
        if (omm != mm) { // Only redraw every minute to minimise flicker
            // Uncomment ONE of the next 2 lines, using the ghost image demonstrates text overlay as time is drawn over it
            tft.setTextColor(0x39C4, TFT_BLACK);  // Leave a 7 segment ghost image, comment out next line!
            //tft.setTextColor(TFT_BLACK, TFT_BLACK); // Set font colour to black to wipe image
            // Font 7 is to show a pseudo 7 segment display.
            // Font 7 only contains characters [space] 0 1 2 3 4 5 6 7 8 9 0 : .
            tft.drawString("88:88", xpos, ypos, 7); // Overwrite the text to clear it
            tft.setTextColor(0xFBE0, TFT_BLACK); // Orange
            omm = mm;

            if (hh < 10) xpos += tft.drawChar('0', xpos, ypos, 7);
            xpos += tft.drawNumber(hh, xpos, ypos, 7);
            xcolon = xpos;
            xpos += tft.drawChar(':', xpos, ypos, 7);
            if (mm < 10) xpos += tft.drawChar('0', xpos, ypos, 7);
            tft.drawNumber(mm, xpos, ypos, 7);
        }

        if (ss % 2) { // Flash the colon
            tft.setTextColor(0x39C4, TFT_BLACK);
            xpos += tft.drawChar(':', xcolon, ypos, 7);
            tft.setTextColor(0xFBE0, TFT_BLACK);
        } else {
            tft.drawChar(':', xcolon, ypos, 7);
        }
    }
}

void IMU_Show()
{
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    // Update the sensor values whenever new data is available
    if ( imu.gyroAvailable() ) {
        // To read from the gyroscope,  first call the
        // readGyro() function. When it exits, it'll update the
        // gx, gy, and gz variables with the most current data.
        imu.readGyro();
    }
    if ( imu.accelAvailable() ) {
        // To read from the accelerometer, first call the
        // readAccel() function. When it exits, it'll update the
        // ax, ay, and az variables with the most current data.
        imu.readAccel();
    }
    if ( imu.magAvailable() ) {
        // To read from the magnetometer, first call the
        // readMag() function. When it exits, it'll update the
        // mx, my, and mz variables with the most current data.
        imu.readMag();
    }

    snprintf(buff, sizeof(buff), "--  ACC  GYR   MAG");
    tft.drawString(buff, 0, 0);
    snprintf(buff, sizeof(buff), "x %.2f  %.2f  %.2f", imu.calcAccel(imu.ax), imu.calcGyro(imu.gx), imu.calcMag(imu.mx));
    tft.drawString(buff, 0, 16);
    snprintf(buff, sizeof(buff), "y %.2f  %.2f  %.2f", imu.calcAccel(imu.ay), imu.calcGyro(imu.gy), imu.calcMag(imu.my));
    tft.drawString(buff, 0, 32);
    snprintf(buff, sizeof(buff), "z %.2f  %.2f  %.2f", imu.calcAccel(imu.az), imu.calcGyro(imu.gz), imu.calcMag(imu.mz));
    tft.drawString(buff, 0, 48);
    delay(200);
}


void loop()
{
#ifdef ARDUINO_OTA_UPDATE
    ArduinoOTA.handle();
#endif

    //! If OTA starts, skip the following operation
    if (otaStart)
        return;

    if (charge_indication) {
        charge_indication = false;
        if (digitalRead(CHARGE_PIN) == LOW) {
            tft.pushImage(140, 55, 16, 16, charge);
        } else {
            tft.fillRect(140, 55, 16, 16, TFT_BLACK);
        }
    }

    if (digitalRead(TP_PIN_PIN) == HIGH) {
        if (!pressed) {
            initial = 1;
            targetTime = millis() + 1000;
            tft.fillScreen(TFT_BLACK);
            omm = 99;
            func_select = func_select + 1 > 2 ? 0 : func_select + 1;
            digitalWrite(LED_PIN, HIGH);
            delay(100);
            digitalWrite(LED_PIN, LOW);
            pressed = true;
            pressedTime = millis();
        } else {
            if (millis() - pressedTime > 3000) {
                tft.fillScreen(TFT_BLACK);
                tft.drawString("Reset WiFi Setting",  20, tft.height() / 2 );
                delay(3000);
//                wifiManager.resetSettings();                                                      //COMPILE ERROR
//                wifiManager.erase(true);                                                          //COMPILE ERROR
                esp_restart();
            }
        }
    } else {
        pressed = false;
    }

    switch (func_select) {
    case 0:
        digitalWrite(LED_PIN, HIGH);
        delay(300);
        digitalWrite(LED_PIN, LOW);
        RTC_Show();
        break;
    case 1:
        digitalWrite(LED_PIN, HIGH);
        delay(300);
        digitalWrite(LED_PIN, LOW);
        IMU_Show();
        break;
    case 2:
        digitalWrite(LED_PIN, HIGH);
        delay(300);
        digitalWrite(LED_PIN, LOW);
        imu.sleepGyro();
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Press again to wake up",  tft.width() / 2, tft.height() / 2 );
        Serial.println("Go to Sleep");
        delay(3000);
        tft.writecommand(ST7735_SLPIN);
        tft.writecommand(ST7735_DISPOFF);
        esp_sleep_enable_ext1_wakeup(GPIO_SEL_33, ESP_EXT1_WAKEUP_ANY_HIGH);
        esp_deep_sleep_start();
        break;
    default:
        break;
    }
}
