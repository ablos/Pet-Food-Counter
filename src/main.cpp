#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <PubSubClient.h>
#include "config.h"
#include "icons.h"

// Structs
struct FeedingMoment 
{
    uint32_t dateTimeValue;

    String dateString() 
    {
        uint16_t date = dateTimeValue / 10000;
        
        if (date == 9999)
            return "?\?-?\?";

        char buffer[7];
        sprintf(buffer, "%02d-%02d", date / 100, date % 100);
        return String(buffer);
    }
    
    String timeString() 
    {
        uint16_t time = dateTimeValue % 10000;
        
        if (time == 9999)
            return "?\?:?\?";

        char buffer[7];
        sprintf(buffer, "%02d:%02d", time / 100, time % 100);
        return String(buffer);
    }
};

struct Memory 
{
    uint32_t crc32;
    uint32_t lastWakeTime;
    uint32_t feedings[4];
    uint8_t feedingCount;
    uint8_t pressCount;
    uint16_t padding;       // Extra padding for memory alignment
};

// Defenitions
#define OLED_POWER_PIN D7
#define VDIV_ENABLE_PIN D5
#define LONG_PRESS_PIN D6

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

// Function declarations
uint32_t calculateCRC32(const uint8_t *data, size_t length);
bool readMemory(Memory* data);
void writeMemory(Memory* data);
void decideAction(uint8_t pressCount);
float getBatteryVoltage();
void wakeDisplay();
void turnOffDisplay();
void updateDisplay();
bool isDisplayOn();
void waitForDisplayOff();
void addFeedingToMemory(uint32_t dateTimeValue);
uint32_t getLatestFeedingFromMemory();
void removeLatestFeedingFromMemory();
void clearAllFeedingsFromMemory();
bool connectMqtt(bool drawSpinner = false);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void addFeeding();
void removeFeeding();
void clearFeedings();
void printCenteredText(String text, int y);
void drawLoadingSpinner();
void print(String text);

// Variables
Memory memoryData;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
unsigned long displayStartTime = 0;
WiFiClient wifi;
PubSubClient mqtt(wifi);
uint32_t currentDateTime = 99999999;
float currentBatteryVoltage = 0;

void setup()
{
    system_phy_set_powerup_option(1);  // Minimal RF during boot
    WiFi.mode(WIFI_OFF);               // Explicit WiFi disable
    WiFi.forceSleepBegin();            // Force radio sleep

    bool validData = readMemory(&memoryData);
    uint32_t currentTime = millis();
    
    // Check if this wake is within multi-press window and update press count accordingly
    if (validData && (currentTime - memoryData.lastWakeTime < MULTI_PRESS_WINDOW)) 
        memoryData.pressCount++;
    else
        memoryData.pressCount = 1;

    // Save the updated count
    memoryData.lastWakeTime = currentTime;
    writeMemory(&memoryData);

    if (SERIAL_DEBUG_ON) 
    {
        Serial.begin(115200);
        Serial.printf("\n\nPress count: %d\n", memoryData.pressCount);
    }
    
    wakeDisplay();
    
    // Wait to see if more presses are coming
    delay(MULTI_PRESS_WINDOW + 50);
    
    // No more presses came, so reset presscount and execute action
    uint8_t pressCount = memoryData.pressCount;
    memoryData.pressCount = 0;
    writeMemory(&memoryData);
    decideAction(pressCount);

    // Return to deep sleep
    waitForDisplayOff();
    ESP.deepSleep(0);
}

void loop() {}

// Decides an action based on press count
void decideAction(uint8_t pressCount) 
{
    switch (pressCount) 
    {
        // Handle single press
        case 1: 
            print("Single press detected");
            pinMode(LONG_PRESS_PIN, INPUT);
            delay(LONG_PRESS_TIME);
            
            if (digitalRead(LONG_PRESS_PIN) == LOW) 
            {
                addFeeding();
            }
            else if (RESET_AFTER_FULL && memoryData.feedingCount == 4) 
            {
                clearAllFeedingsFromMemory();
                updateDisplay();
            }
            
            break;

        // Handle double press
        case 2:
            print("Double press detected");
            removeFeeding();
            break;

        // Handle quadriple press
        case 4:
            print("Quadriple press detected");
            clearFeedings();
            break;

        // Default behaviour
        default:
            print("Too many presses - treating as single");
            break;
    }
}

// Checks the battery voltage
float getBatteryVoltage() 
{
    // Enable voltage divider
    pinMode(VDIV_ENABLE_PIN, OUTPUT);
    digitalWrite(VDIV_ENABLE_PIN, HIGH);
    delay(50);

    // Set up analog pin
    pinMode(A0, INPUT);
    
    // Take multiple readings and average them
    const int numReadings = 10;
    int totalReading = 0;

    for (int i = 0; i < numReadings; i++) 
    {
        totalReading += analogRead(A0);
        delay(10);
    }

    // Read and convert voltage
    int adcValue = totalReading / numReadings;
    float adcVoltage = (adcValue / 1024.0) * MCP_OUTPUT_VOLTAGE;
    float batteryVoltage = adcVoltage * ((330.0 + 680.0) / 680.0);
    
    // Disable voltage divider
    digitalWrite(VDIV_ENABLE_PIN, LOW);

    return batteryVoltage + VOLTAGE_OFFSET;
}

// Turns the display on after giving power with the transistor
void wakeDisplay() 
{
    // Power on OLED screen
    pinMode(OLED_POWER_PIN, OUTPUT);
    digitalWrite(OLED_POWER_PIN, HIGH);
    delay(10);

    // Initialize display
    display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
    
    // Update display
    updateDisplay();
}

// Turns the display off and cuts power with transistor
void turnOffDisplay() 
{
    // Shut down display
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    display.clearDisplay();
    delay(10);

    // Power down OLED screen
    pinMode(OLED_POWER_PIN, OUTPUT);
    digitalWrite(OLED_POWER_PIN, LOW);

    print("Display turned off");
}

// Prints text centered horizontally
void printCenteredText(String text, int y)
{
    int16_t x1, y1;
    uint16_t w, h;
    
    // Calculate text dimensions
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    
    // Calculate center position
    int centerX = (SCREEN_WIDTH - w) / 2;
    
    // Set cursor and print
    display.setCursor(centerX, y);
    display.print(text);
}

// Draws a rotating loading spinner
void drawLoadingSpinner() 
{
    // Clear display
    display.clearDisplay();

    // This creates an arc that expands and contracts while rotating
    unsigned long currentTime = millis();
    unsigned long cycle = currentTime % 2000;
    float rotation = (currentTime % 1000) * 2 * PI / 1000.0;
    
    // Arc length changes over time
    float arcLength;
    if (cycle < 1000)
        arcLength = (cycle / 1000.0) * (3 * PI / 2); // Expand from 0 to 270°
    else
        arcLength = ((2000 - cycle) / 1000.0) * (3 * PI / 2); // Contract back
    
    int numPoints = 10;
    int radius = 15;
    for (int i = 0; i < numPoints; i++)
    {
        float angle = rotation + (arcLength * i / numPoints);
        
        int x = 64 + radius * cos(angle);
        int y = 32 + radius * sin(angle);
        
        // Draw 2-pixel thick line
        display.drawPixel(x, y, SSD1306_WHITE);
        display.drawPixel(x + 1, y, SSD1306_WHITE);
        display.drawPixel(x, y + 1, SSD1306_WHITE);
        display.drawPixel(x + 1, y + 1, SSD1306_WHITE);
    }

    // Show to display
    display.display();
}

// Updates the display information
void updateDisplay() 
{
    // Clear buffer
    display.clearDisplay();
    
    // Set text color
    display.setTextColor(SSD1306_WHITE);
    
    // Retrieve latest feeding
    FeedingMoment latestMoment;
    latestMoment.dateTimeValue = getLatestFeedingFromMemory();

    // Print information
    if (memoryData.feedingCount == 0) 
    {
        // If feeding count is 0, print no food icon
        display.drawBitmap(40, 8, no_food_icon, 48, 48, SSD1306_WHITE);
    }
    else 
    {
        // Print food icon the same amount of times as feeding count
        int startPoint = 64 - (memoryData.feedingCount * 16);
        for (int i = 0; i < memoryData.feedingCount; i++) 
        {
            display.drawBitmap(i * 32 + startPoint, 0, food_icon, 32, 32, SSD1306_WHITE);
        }
        
        // Print lastest time
        display.setTextSize(2);
        printCenteredText(latestMoment.timeString(), 38);
        
        // Print lastest date
        display.setTextSize(1);
        printCenteredText(latestMoment.dateString(), 57);
    }

    // Put on display
    display.display();

    // Start display timer
    displayStartTime = millis();
}

// Checks if the display is on, if it is on for more than the wake time, turn it off
bool isDisplayOn()
{
    if (millis() - displayStartTime > SCREEN_WAKE_TIME) 
    {
        turnOffDisplay();
        return false;
    }

    return true;
}

// Wait until the display is off
void waitForDisplayOff() 
{
    while (isDisplayOn()) 
    {
        yield();
    }
}

// Add a new feeding moment
void addFeedingToMemory(uint32_t dateTimeValue) 
{
    if (memoryData.feedingCount >= 4)
        return;

    // Shift existing feedings right
    for (int i = 3; i > 0; i--)
        memoryData.feedings[i] = memoryData.feedings[i - 1];
    
    // Add new feedings at front
    memoryData.feedings[0] = dateTimeValue;
    if (memoryData.feedingCount < 4)
        memoryData.feedingCount++;

    writeMemory(&memoryData);
}

// Retrieve the latest feeding moment
uint32_t getLatestFeedingFromMemory() 
{
    if (memoryData.feedingCount > 0)
        return memoryData.feedings[0];

    return 99999999;
}

// Remove the latest feeding moment
void removeLatestFeedingFromMemory() 
{
    if (memoryData.feedingCount == 0)
        return;
        
    // Shift everything left
    for (int i = 0; i < 3; i++)
        memoryData.feedings[i] = memoryData.feedings[i + 1];

    memoryData.feedingCount--;
    memoryData.feedings[memoryData.feedingCount] = 0;

    writeMemory(&memoryData);
}

// Clears all feeding records
void clearAllFeedingsFromMemory() 
{
    memoryData.feedingCount = 0;

    for (int i = 0; i < 4; i++)
        memoryData.feedings[i] = 0;

    writeMemory(&memoryData);
}

// Connects to WiFi and MQTT
bool connectMqtt(bool drawSpinner) 
{
    print("Connecting to MQTT...");
    // First read battery voltage, since WiFi can create noise on the analog input
    currentBatteryVoltage = getBatteryVoltage();

    // Enable WiFi
    WiFi.forceSleepWake();
    WiFi.mode(WIFI_STA);
    
    // Configure static IP if provided
    if (strlen(STATIC_IP) > 0) 
    {
        IPAddress ip, gateway, subnet, dns;
        ip.fromString(STATIC_IP);
        gateway.fromString(GATEWAY_IP);
        subnet.fromString(SUBNET_MASK);
        dns.fromString(DNS_SERVER);

        WiFi.config(ip, gateway, subnet, dns);
    }

    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Wait for connection with 10 sec timeout
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT) 
    {
        if (drawSpinner)
            drawLoadingSpinner();
            
        delay(1);
    }
        
    // If we failed to connect after timeout return
    if (WiFi.status() != WL_CONNECTED) 
    {   
        // Show connection failed icon
        display.clearDisplay();
        display.drawBitmap(40, 8, connection_failed_icon, 48, 48, SSD1306_WHITE);
        display.display();
        delay(3000);
        return false;
    }
    
    // Show connection successful icon
    display.clearDisplay();
    display.drawBitmap(40, 8, connection_success_icon, 48, 48, SSD1306_WHITE);
    display.display();

    // Set up MQTT
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    
    // Connect to MQTT (with authentication if present)
    bool connected = false;
    
    if (strlen(MQTT_USER) > 0)
        connected = mqtt.connect(MQTT_NAME, MQTT_USER, MQTT_PASS);
    else
        connected = mqtt.connect(MQTT_NAME);

    print("Connection successfull: " + String(connected));

    return connected;
}

// Disconnects from MQTT and WiFi
void disconnectMqtt() 
{
    mqtt.disconnect();
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();
}

// This is called when a MQTT message is received
void mqttCallback(char* topic, byte* payload, unsigned int length) 
{
    // We receive the current time, parse it and store it
    currentDateTime = 0;
    for (unsigned int i = 0; i < length; i++)
    {
        if (payload[i] >= '0' && payload[i] <= '9')
            currentDateTime = currentDateTime * 10 + (payload[i] - '0');
    }

    print("Received datetime: " + String(currentDateTime));
}

// Sends the latest data over MQTT
void sendUpdate() 
{
    if (mqtt.connected()) 
    {
        print("Sending update...");

        String json = "{\"count\":" + String(memoryData.feedingCount) + ", \"datetime\":" + String(getLatestFeedingFromMemory()) + ", \"battery-voltage\":" + String(currentBatteryVoltage) + "}";

        mqtt.publish(MQTT_SEND, json.c_str(), true);
    }
}

// Adds a feeding moment, both synced to MQTT and to memory
void addFeeding() 
{
    print("Adding feeding...");

    // Try to connect to MQTT
    bool connected = connectMqtt(true);
    if (connected)
    {
        // Subscribe to the time topic
        mqtt.subscribe(MQTT_RECV);
        
        // Wait for the retained message with 3 sec timeout
        unsigned long start = millis();
        while (millis() - start < MQTT_TIMEOUT && currentDateTime == 99999999) 
        {
            mqtt.loop();
            yield();
        }
    }
    
    // Check if current date is still as last feeding date, if not reset feedings
    if (memoryData.feedingCount > 0 && getLatestFeedingFromMemory() != 99999999 && currentDateTime != 99999999 && currentDateTime / 10000 != getLatestFeedingFromMemory() / 10000)
        clearAllFeedingsFromMemory();

    // Add feeding with current time to memory
    addFeedingToMemory(currentDateTime);
    
    // Update the display
    updateDisplay();
    
    // Send update over MQTT
    sendUpdate();

    // Disconnect MQTT
    disconnectMqtt();
    
    // Set display start time to now, so there is still enough time to view the updated value
    displayStartTime = millis();
}

// Removes the last feeding moment both from MQTT and memory
void removeFeeding() 
{
    // Dont do anything if feeding count is already at 0
    if (memoryData.feedingCount == 0)
        return;

    print("Removing feeding...");

    // Remove the latest feeding from memory
    removeLatestFeedingFromMemory();
    
    // Update the display
    updateDisplay();
    
    // Try to connect to MQTT and send update
    if (connectMqtt())
        sendUpdate();
        
    // Disconnect MQTT
    disconnectMqtt();
}

void clearFeedings() 
{
    // Dont do anything if feeding count is already at 0
    if (memoryData.feedingCount == 0)
        return;

    print("Clearing all feedings...");
    
    // Clear all feedings from memory
    clearAllFeedingsFromMemory();
    
    // Update the display
    updateDisplay();
    
    // Try to connect to MQTT and send update
    if (connectMqtt())
        sendUpdate();
        
    // Disconnect MQTT
    disconnectMqtt();
}

// Reads RTC memory
bool readMemory(Memory* data) 
{
    if (ESP.rtcUserMemoryRead(0, (uint32_t*)data, sizeof(Memory))) 
    {
        uint32_t crcOfData = calculateCRC32((uint8_t *)&data->lastWakeTime, sizeof(Memory) - sizeof(data->crc32));
        
        if (crcOfData == data->crc32 && data->feedingCount <= 4)
            return true;
    }
    
    // Initialize with defaults if invalid
    memset(data, 0, sizeof(Memory));

    return false;
}

// Writes RTC memory
void writeMemory(Memory* data) 
{
    data->crc32 = calculateCRC32((uint8_t *)&data->lastWakeTime, sizeof(Memory) - sizeof(data->crc32));
    ESP.rtcUserMemoryWrite(0, (uint32_t *)data, sizeof(Memory));
}

// Calculates CRC for data validity
uint32_t calculateCRC32(const uint8_t *data, size_t length) 
{
    uint32_t crc = 0xffffffff;
    
    while (length--) 
    {
        uint8_t c = *data++;

        for (uint32_t i = 0x80; i > 0; i >>= 1) 
        {
            bool bit = crc & 0x80000000;
            
            if (c & i)
                bit = !bit;

            crc <<= 1;
            
            if (bit)
                crc ^= 0x04c11db7;
        }
    }

    return crc;
}

void print(String text) 
{
    if (SERIAL_DEBUG_ON)
        Serial.println(text);
}