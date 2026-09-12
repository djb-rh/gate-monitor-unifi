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
 *              - Between those two: once a button is pressed the panel does
 *                not know the new state yet (the gate takes a while to swing),
 *                so that button's pixel blinks slowly red/green and the LCD
 *                says Opening/Closing until the matching state event
 *                arrives from Home Assistant (or PENDING_TIMEOUT_MS passes,
 *                at which point we fall back to the last known state).
 *
 *              See the homeassistant/ yaml files for the Home Assistant side, and
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
// CONFIG
// ---------------------------------------------------------------------------

// HA_HOST (and optionally HA_PORT) come from src/ha-config.h, which is
// gitignored -- copy src/ha-config.h.example to create it. Keeping the address
// out of this file means the repo stays publishable as-is and your local
// settings don't show up as uncommitted changes every time you build.
#if __has_include("ha-config.h")
#include "ha-config.h"
#endif

#ifndef HA_HOST
#error "Missing src/ha-config.h -- copy src/ha-config.h.example to src/ha-config.h and set HA_HOST"
#endif

#ifndef HA_PORT
#define HA_PORT 8123
#endif

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

// How long a pixel blinks waiting for the gate to reach the requested state
// before we give up and go back to showing the last known state. The main
// gate is the slow one; 90s is comfortably longer than a full swing.
#define PENDING_TIMEOUT_MS 90000UL

// Blink half-period: this many ms red, then this many ms green, repeat.
#define BLINK_HALF_PERIOD_MS 600UL

// ---------------------------------------------------------------------------

int out = D7;

// use WS2811 in the Particle library because these (8mm diffused Adafruit 1734) are RGB addressed, whereas the other types are GRB
Adafruit_NeoPixel pixels = Adafruit_NeoPixel(2, out, WS2811);

void mainGateStateHandler(const char *eventName, const char *data);
void clubGateStateHandler(const char *eventName, const char *data);
void startPending(int *target, unsigned long *start, int wantedState);
void expirePending();
void updatePixels();
void lcdLine(int row, const char *label, const char *value);
void lcdMain(const char *value);
void lcdClub(const char *value);
void sendWebhook(const char *webhookId);

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

// A change we've asked for but haven't seen confirmed yet.
// 0 = nothing pending, 1 = waiting for open, 2 = waiting for closed.
int mainPendingTarget = 0;
int clubPendingTarget = 0;
unsigned long mainPendingStart = 0;
unsigned long clubPendingStart = 0;

// Last values actually pushed to the strip, so we only call pixels.show()
// when something really changed. 0xFFFFFFFF is "nothing shown yet".
uint32_t shownMainColor = 0xFFFFFFFF;
uint32_t shownClubColor = 0xFFFFFFFF;

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
            startPending(&mainPendingTarget, &mainPendingStart, 2);
            lcdMain("Closing");
        } else {
            // closed, or unknown -- default to open
            sendWebhook(WEBHOOK_MAIN_OPEN);
            startPending(&mainPendingTarget, &mainPendingStart, 1);
            lcdMain("Opening");
        }
    }

    if (buttonOne.clicks != 0) buttonOneClicks = buttonOne.clicks;

    if (buttonOneClicks == 1) {
        if (clubGateState == 1) {
            sendWebhook(WEBHOOK_CLUB_CLOSE);
            startPending(&clubPendingTarget, &clubPendingStart, 2);
            lcdClub("Closing");
        } else {
            sendWebhook(WEBHOOK_CLUB_OPEN);
            startPending(&clubPendingTarget, &clubPendingStart, 1);
            lcdClub("Opening");
        }
    }

    buttonZeroClicks = 0;
    buttonOneClicks = 0;

    expirePending();
    updatePixels();
}

// ---------------------------------------------------------------------------
// Pending-change bookkeeping + pixel rendering
// ---------------------------------------------------------------------------

// Mark a gate as "asked to change, waiting for Home Assistant to confirm".
// Pressing again while already pending just restarts the timeout.
void startPending(int *target, unsigned long *start, int wantedState) {
    *target = wantedState;
    *start = millis();
}

// If the gate never reports the state we asked for, stop blinking eventually
// and go back to displaying whatever we last actually knew.
void expirePending() {
    unsigned long now = millis();

    if (mainPendingTarget != 0 && (now - mainPendingStart) > PENDING_TIMEOUT_MS) {
        mainPendingTarget = 0;
        lcdMain(mainGateState == 1 ? "Open" : (mainGateState == 2 ? "Closed" : "DUNNO"));
    }

    if (clubPendingTarget != 0 && (now - clubPendingStart) > PENDING_TIMEOUT_MS) {
        clubPendingTarget = 0;
        lcdClub(clubGateState == 1 ? "Open" : (clubGateState == 2 ? "Closed" : "DUNNO"));
    }
}

// Pack a color the same way Adafruit_NeoPixel does, so we can compare against
// what's already on the strip.
uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// What color should this gate's button be right now? Pending gates alternate
// red/green on the shared blink phase; settled ones are solid.
uint32_t colorFor(int state, int pendingTarget, int blinkPhase) {
    if (pendingTarget != 0) {
        return blinkPhase ? rgb(0,55,0) : rgb(55,0,0);
    }
    if (state == 1) return rgb(0,55,0);     // open
    if (state == 2) return rgb(55,0,0);     // closed
    return rgb(0,0,255);                    // blue is don't know
}

void updatePixels() {
    // Both buttons share one phase so a simultaneous pair blinks together.
    int blinkPhase = (millis() / BLINK_HALF_PERIOD_MS) & 1;

    uint32_t mainColor = colorFor(mainGateState, mainPendingTarget, blinkPhase);
    uint32_t clubColor = colorFor(clubGateState, clubPendingTarget, blinkPhase);

    if (mainColor == shownMainColor && clubColor == shownClubColor) return;

    pixels.setPixelColor(0, mainColor);
    pixels.setPixelColor(1, clubColor);
    pixels.show();

    shownMainColor = mainColor;
    shownClubColor = clubColor;
}

// ---------------------------------------------------------------------------
// LCD helpers -- pad to the full 20 columns so shorter words don't leave
// leftovers from whatever was printed before.
// ---------------------------------------------------------------------------
void lcdLine(int row, const char *label, const char *value) {
    char buf[21];
    snprintf(buf, sizeof(buf), "%s%s", label, value);
    for (int i = strlen(buf); i < 20; i++) buf[i] = ' ';
    buf[20] = '\0';
    lcd.setCursor(0, row);
    lcd.print(buf);
}

void lcdMain(const char *value) { lcdLine(1, "Main Gate: ", value); }
void lcdClub(const char *value) { lcdLine(0, "CH Gate: ", value); }

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
// data="open" or data="closed". Reaching the state we were waiting on clears
// the pending blink; any other state leaves it blinking (the gate is still
// on its way).
// ---------------------------------------------------------------------------
void mainGateStateHandler(const char *eventName, const char *data)
{
    if (!data) return;

    if (!strncmp(data, "open", 4)) {
        mainGateState = 1;
    } else if (!strncmp(data, "closed", 6)) {
        mainGateState = 2;
    } else {
        return;
    }

    if (mainPendingTarget == mainGateState) mainPendingTarget = 0;

    if (mainPendingTarget == 0) {
        lcdMain(mainGateState == 1 ? "Open" : "Closed");
    }

    updatePixels();
}

void clubGateStateHandler(const char *eventName, const char *data)
{
    if (!data) return;

    if (!strncmp(data, "open", 4)) {
        clubGateState = 1;
    } else if (!strncmp(data, "closed", 6)) {
        clubGateState = 2;
    } else {
        return;
    }

    if (clubPendingTarget == clubGateState) clubPendingTarget = 0;

    if (clubPendingTarget == 0) {
        lcdClub(clubGateState == 1 ? "Open" : "Closed");
    }

    updatePixels();
}
