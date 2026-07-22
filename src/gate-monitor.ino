/*
 * Project gate-monitor (Unifi Access/Protect edition)
 * Description: Physical control panel (2 buttons, 20x4 LCD, 2 NeoPixels) for the
 *              main gate and clubhouse gate.
 *
 *              - Button press -> Photon POSTs to a Home Assistant local webhook
 *                (plain HTTP, LAN only). HA turns the actual Ubiquiti relay
 *                switch on/off. There's no good way for HA to receive a push
 *                from the Particle Cloud without extra infrastructure, so this
 *                direction stays local -- same idea as your existing
 *                local_only webhook automations (Unlock E-shop, etc).
 *
 *              - Gate state -> Home Assistant publishes an event to the
 *                Particle Cloud (main_gate_state / club_gate_state) whenever
 *                the Unifi Access binary_sensor changes, and this firmware
 *                subscribes to it exactly the way the original two-Photon
 *                setup did -- HA has just taken the place of the second
 *                Photon as the publisher. No local server, no static IP
 *                needed on this device for that direction.
 *
 *              See ../homeassistant/*.yaml for the Home Assistant side, and
 *              ../README.md for full setup instructions (including how to
 *              create the Particle access token HA needs).
 *
 * Author: Donnie Barnes <djb@donniebarnes.com>
 * Original Date: Dec 31, 2023
 * Unifi Access/Protect revision: Jul 2026
 */

#include <LiquidCrystal_I2C_Spark.h>
#include <clickButton.h>
#include <neopixel.h>

// ---------------------------------------------------------------------------
// CONFIG -- edit these for your network before flashing
// ---------------------------------------------------------------------------

// Local IP or hostname of your Home Assistant instance, used only for the
// outbound button-press webhooks.
#define HA_HOST "192.168.1.50"      // TODO: replace with your HA's local IP
#define HA_PORT 8123

// Webhook IDs -- must exactly match the automations in
// homeassistant/automations_gate_panel.yaml
#define WEBHOOK_MAIN_OPEN   "gatepanel-main-open-bJC4yulP6XuuD1jopP0Q"
#define WEBHOOK_MAIN_CLOSE  "gatepanel-main-close-vVdYVMVJnQlkhCqWHQy7"
#define WEBHOOK_CLUB_OPEN   "gatepanel-club-open-oy6DqZ0wh13crgNUvZjQ"
#define WEBHOOK_CLUB_CLOSE  "gatepanel-club-close-KOewgWR6nQC8ae3oGX01"
#define WEBHOOK_REQUEST_STATE "gatepanel-request-state-lpHSDjBJcVTTjEO39fT8"

// Particle Cloud event names -- must exactly match what Home Assistant
// publishes to in homeassistant/rest_commands_gate_panel.yaml
#define EVENT_MAIN_GATE_STATE "main_gate_state"
#define EVENT_CLUB_GATE_STATE "club_gate_state"

// ---------------------------------------------------------------------------

int out = D7;

// use WS2811 in the Particle library because these (8mm diffused Adafruit 1734) are RGB addressed, whereas the other types are GRB
Adafruit_NeoPixel pixels = Adafruit_NeoPixel(2, out, WS2811);

void mainGateStateHandler(const char *eventName, const char *data);
void clubGateStateHandler(const char *eventName, const char *data);

LiquidCrystal_I2C lcd(0x27,20,4);  // set the LCD address to 0x27 for a 20 chars and 4 line display

int buttonZeroPin = D3;
int buttonOnePin = D4;

ClickButton buttonZero(buttonZeroPin, LOW, CLICKBTN_PULLUP);
ClickButton buttonOne(buttonOnePin, LOW, CLICKBTN_PULLUP);

int buttonZeroClicks = 0;
int buttonOneClicks = 0;

// Known gate state, driven by Particle Cloud events published by Home
// Assistant (not assumed locally). 0 = unknown, 1 = open, 2 = closed
int mainGateState = 0;
int clubGateState = 0;

int bootRequestSent = 0;

void setup() {

    pinMode(buttonZeroPin, INPUT_PULLUP);
    pinMode(buttonOnePin, INPUT_PULLUP);

    buttonZero.debounceTime = 20;
    buttonZero.multiclickTime = 250;
    buttonZero.longClickTime = 1000;

    buttonOne.debounceTime = 20;
    buttonOne.multiclickTime = 250;
    buttonOne.longClickTime = 1000;

    pixels.begin();
    // blue is don't know
    pixels.setPixelColor(0, 0,0,255);
    pixels.setPixelColor(1, 0,0,255);
    pixels.show();

    // Listen for Home Assistant publishing gate state to the Particle Cloud
    // (MY_DEVICES restricts this to private events from your own account).
    Particle.subscribe(EVENT_MAIN_GATE_STATE, mainGateStateHandler, MY_DEVICES);
    Particle.subscribe(EVENT_CLUB_GATE_STATE, clubGateStateHandler, MY_DEVICES);

    Particle.variable("mainGateState", mainGateState);
    Particle.variable("clubGateState", clubGateState);

    Serial.begin(115200);
    lcd.init();  //initialize the lcd
    lcd.backlight();  //open the backlight

    lcd.setCursor (0, 0 );
    lcd.print("CH Gate: DUNNO");
    lcd.setCursor (0, 1 );
    lcd.print("Main Gate: DUNNO");

}

void loop() {

    // Once the Photon is actually connected to the Particle Cloud (not just
    // WiFi), ask Home Assistant to (re-)publish current state so our
    // subscribe handlers above are guaranteed to be live to catch the
    // response. Mirrors the old "getstate" publish on boot. Only fires once.
    if (!bootRequestSent) {
        if (Particle.connected()) {
            sendWebhook(WEBHOOK_REQUEST_STATE);
            bootRequestSent = 1;
        }
    }

    buttonZero.Update();
    buttonOne.Update();

    if (buttonZero.clicks != 0) buttonZeroClicks = buttonZero.clicks;

    if (buttonZeroClicks == 1) {
        if (mainGateState == 1) {
            // known open -> close it
            sendWebhook(WEBHOOK_MAIN_CLOSE);
        } else {
            // closed, or unknown -- default to open
            sendWebhook(WEBHOOK_MAIN_OPEN);
        }
    }

    if (buttonOne.clicks != 0) buttonOneClicks = buttonOne.clicks;

    if (buttonOneClicks == 1) {
        if (clubGateState == 1) {
            sendWebhook(WEBHOOK_CLUB_CLOSE);
        } else {
            sendWebhook(WEBHOOK_CLUB_OPEN);
        }
    }

    buttonZeroClicks = 0;
    buttonOneClicks = 0;
}

// ---------------------------------------------------------------------------
// Outbound: fire a Home Assistant local webhook, plain HTTP, no auth needed
// (the automation's local_only:true keeps it off the public internet).
// Fire-and-forget -- we don't wait for/parse the response.
// ---------------------------------------------------------------------------
void sendWebhook(const char *webhookId) {
    TCPClient client;
    if (client.connect(HA_HOST, HA_PORT)) {
        client.print("POST /api/webhook/");
        client.print(webhookId);
        client.print(" HTTP/1.1\r\n");
        client.print("Host: ");
        client.print(HA_HOST);
        client.print("\r\n");
        client.print("Content-Length: 0\r\n");
        client.print("Connection: close\r\n\r\n");
        // give the socket a moment to flush before we tear it down
        client.flush();
    }
    client.stop();
}

// ---------------------------------------------------------------------------
// Inbound: Particle Cloud event handlers. Home Assistant publishes
// data="open" or data="closed".
// ---------------------------------------------------------------------------
void mainGateStateHandler(const char *eventName, const char *data)
{
    if (!data) return;

    lcd.setCursor(0, 1);
    if (!strncmp(data, "open", 4)) {
        mainGateState = 1;
        lcd.print("Main Gate: Open     ");
        pixels.setPixelColor(0, 0,55,0);
    } else if (!strncmp(data, "closed", 6)) {
        mainGateState = 2;
        lcd.print("Main Gate: Closed   ");
        pixels.setPixelColor(0, 55,0,0);
    }
    pixels.show();
}

void clubGateStateHandler(const char *eventName, const char *data)
{
    if (!data) return;

    lcd.setCursor(0, 0);
    if (!strncmp(data, "open", 4)) {
        clubGateState = 1;
        lcd.print("CH Gate: Open     ");
        pixels.setPixelColor(1, 0,55,0);
    } else if (!strncmp(data, "closed", 6)) {
        clubGateState = 2;
        lcd.print("CH Gate: Closed   ");
        pixels.setPixelColor(1, 55,0,0);
    }
    pixels.show();
}
