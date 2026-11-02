#include <Arduino.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Servo.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "face.h"
#include "page.h"
#include "secrets.h" // WIFI_SSID, WIFI_PASS

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64 // 32 for 0.91" (long thin), 64 for 0.96" (squarish)
#define OLED_ADDR 0x3C   // try 0x3D if this doesn't work

// L298N (ENA/ENB jumpers on, speed set by PWM on the IN pins)
#define IN1 D5 // left motor  (OUT1/OUT2)
#define IN2 D6
#define IN3 D7 // right motor (OUT3/OUT4)
#define IN4 D8

// MG995 neck servo, the head turns with the eyes
#define HEAD_PIN D0
#define HEAD_CENTER 90 // angle that faces straight ahead, nudge it if the head sits crooked
#define HEAD_RANGE 35  // how far it turns each way, negative if it turns the wrong way
#define HEAD_REST_MS 1000 // let go of the head after it's been still this long, stops the buzzing

// DFPlayer Mini plays the music off its sd card, its RX wired to D4 (Serial1, send only)
#define MP3_VOLUME 20      // 0..30
#define MP3_BOOT_MS 2000   // it reads the sd card after power-up and ignores commands until done
#define BIRD_SONGS 3       // sd card folder 01 holds 001_*.mp3, 002_*.mp3.., raise it when you add more

#define DRIVE_SPEED 180
#define TURN_SPEED 160
#define DRIVE_MS 3000    // forward/back stop on their own after this
#define TURN_MS 400      // how long a left/right turn lasts
#define ADDRESS_MS 10000 // how long the IP stays on screen after connecting
#define JOY_MIN_SPEED 90 // weakest PWM that still turns the wheels, raise it if small pushes only hum
#define JOY_MS 500       // joystick driving stops if the phone goes quiet this long

// dance moves as left, right speeds
#define D_LEFT {-TURN_SPEED, TURN_SPEED}
#define D_RIGHT {TURN_SPEED, -TURN_SPEED}
#define D_FWD {DRIVE_SPEED, DRIVE_SPEED}
#define D_BACK {-DRIVE_SPEED, -DRIVE_SPEED}
#define D_PAUSE {0, 0}

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
ESP8266WebServer server(80);
Servo head;

struct Command
{
    const char *name;
    const char *label;
    int left;
    int right;
    unsigned long ms; // auto-stop after this long, 0 = no limit
    Expression face;
};

// wiggle, shuffle forward and back, wiggle, then spin right
const int wiggle[][2] = {
    D_LEFT, D_RIGHT, D_LEFT, D_RIGHT,
    D_FWD, D_BACK, D_FWD, D_BACK,
    D_LEFT, D_RIGHT, D_LEFT, D_RIGHT,
    D_RIGHT, D_RIGHT, D_RIGHT, D_RIGHT, // spin, add or remove some to spin longer or shorter
};

// penguin walk: one wheel at a time zigzags it forward, pose, then waddle back
const int waddle[][2] = {
    {DRIVE_SPEED, 0}, {0, DRIVE_SPEED}, {DRIVE_SPEED, 0}, {0, DRIVE_SPEED},
    D_PAUSE, D_PAUSE,
    {-DRIVE_SPEED, 0}, {0, -DRIVE_SPEED}, {-DRIVE_SPEED, 0}, {0, -DRIVE_SPEED},
    D_PAUSE, D_PAUSE,
};

// fast shimmy, cha-cha-cha forward and back, then spin left
const int chacha[][2] = {
    D_LEFT, D_RIGHT, D_LEFT, D_RIGHT, D_LEFT, D_RIGHT, D_LEFT, D_RIGHT,
    D_PAUSE, D_PAUSE,
    D_FWD, D_PAUSE, D_FWD, D_PAUSE, D_FWD, D_FWD,
    D_BACK, D_PAUSE, D_BACK, D_PAUSE, D_BACK, D_BACK,
    D_LEFT, D_LEFT, D_LEFT, D_LEFT, D_LEFT, D_LEFT, D_LEFT, D_LEFT,
};

struct Dance
{
    const int (*moves)[2];
    int steps;
    unsigned long stepMs; // how long each move lasts
    int track;            // its mp3, music/make_music.py makes it exactly steps x stepMs long
};

const Dance dances[] = {
    {wiggle, sizeof(wiggle) / sizeof(wiggle[0]), 300, 12},
    {waddle, sizeof(waddle) / sizeof(waddle[0]), 250, 13},
    {chacha, sizeof(chacha) / sizeof(chacha[0]), 150, 14},
};
const int DANCES = sizeof(dances) / sizeof(dances[0]);

const Command commands[] = {
    {"stop", "STOP", 0, 0, 0, Expression::Idle},
    {"dance", "DANCE", 0, 0, 0, Expression::Dance}, // loop() plays the moves and stops at the end
    {"forward", "FORWARD", DRIVE_SPEED, DRIVE_SPEED, DRIVE_MS, Expression::Forward},
    {"back", "BACKWARD", -DRIVE_SPEED, -DRIVE_SPEED, DRIVE_MS, Expression::Backward},
    {"left", "LEFT", -TURN_SPEED, TURN_SPEED, TURN_MS, Expression::Left},
    {"right", "RIGHT", TURN_SPEED, -TURN_SPEED, TURN_MS, Expression::Right},
};

// speeds and face filled in by handleJoystick
Command joystick = {"joystick", "JOYSTICK", 0, 0, JOY_MS, Expression::Idle};

enum class Screen
{
    Connecting,
    Address,
    Face,
};

const Command *current = &commands[0];
unsigned long startedAt = 0;
int danceIndex = -1; // dance playing now
int danceStep = -1;  // dance move now driving
bool wasConnected = false;
Screen screen = Screen::Connecting;
bool showAddress = false;
unsigned long connectedAt = 0;
int headAngle = -1; // -1 = not attached yet, the first loop centers it
unsigned long headMovedAt = 0;
int playing = 0; // mp3 track playing now, 0 = quiet
int birdSong = 0;     // last bird song played, 1..BIRD_SONGS
bool birding = false; // a bird song is playing over the face's music

// speed: -255 (full reverse) .. 255 (full forward), 0 = brake
void setMotor(uint8_t pinA, uint8_t pinB, int speed)
{
    speed = constrain(speed, -255, 255);
    if (speed > 0)
    {
        analogWrite(pinA, speed);
        digitalWrite(pinB, LOW);
    }
    else if (speed < 0)
    {
        digitalWrite(pinA, LOW);
        analogWrite(pinB, -speed);
    }
    else
    {
        digitalWrite(pinA, LOW);
        digitalWrite(pinB, LOW);
    }
}

void drive(int left, int right)
{
    setMotor(IN1, IN2, left);
    setMotor(IN3, IN4, right);
}

// DFPlayer frame: 7E FF 06 cmd 00(no reply) arg checksum EF
void mp3(uint8_t cmd, uint16_t arg)
{
    uint8_t frame[10] = {0x7E, 0xFF, 0x06, cmd, 0x00, uint8_t(arg >> 8), uint8_t(arg), 0, 0, 0xEF};
    uint16_t sum = 0;
    for (int i = 1; i < 7; i++)
        sum -= frame[i];
    frame[7] = sum >> 8;
    frame[8] = sum & 0xFF;
    Serial1.write(frame, sizeof frame);
}

void run(const Command *cmd)
{
    drive(cmd->left, cmd->right);
    current = cmd;
    startedAt = millis();
    if (birding)
    {
        birding = false; // any command ends the bird song
        playing = -1;    // so loop() puts the face's music back, or stops
    }
    if (cmd->face == Expression::Dance)
    {
        danceIndex = (danceIndex + 1) % DANCES; // each dance command plays the next dance
        danceStep = -1;
    }
    showAddress = false; // jump straight to the face
    faceSet(cmd->face);
    Serial.println(cmd->label);
}

void handleCommand()
{
    String name = server.arg("c");
    for (const Command &cmd : commands)
    {
        if (name == cmd.name)
        {
            run(&cmd);
            server.send(200, "text/plain", "ok");
            return;
        }
    }
    server.send(400, "text/plain", "unknown command");
}

void handleBirdSong()
{
    birdSong = birdSong % BIRD_SONGS + 1; // each press plays the next one, after the last comes the first
    birding = true;
    mp3(0x0F, 0x0100 | birdSong); // play 01/<birdSong>
    server.send(200, "text/plain", String(birdSong));
    Serial.print("BIRD SONG ");
    Serial.println(birdSong);
}

// stretch 1..255 up to JOY_MIN_SPEED..255 so small pushes still move the wheels
int boost(int speed)
{
    if (speed == 0)
        return 0;
    int s = map(abs(speed), 1, 255, JOY_MIN_SPEED, 255);
    return speed > 0 ? s : -s;
}

void handleJoystick()
{
    int x = constrain(server.arg("x").toInt(), -255, 255); // right is +
    int y = constrain(server.arg("y").toInt(), -255, 255); // forward is +
    server.send(200, "text/plain", "ok");

    if (x == 0 && y == 0)
    {
        if (current != &commands[0])
            run(&commands[0]);
        return;
    }

    int left = y + x;
    int right = y - x;
    int biggest = max(max(abs(left), abs(right)), 255); // scale diagonals down instead of clipping
    joystick.left = boost(left * 255 / biggest);
    joystick.right = boost(right * 255 / biggest);

    Expression face = abs(y) >= abs(x) ? (y > 0 ? Expression::Forward : Expression::Backward)
                                       : (x > 0 ? Expression::Right : Expression::Left);
    if (current != &joystick || face != joystick.face)
    {
        joystick.face = face;
        run(&joystick); // new face only when the direction changes
    }
    else
    {
        drive(joystick.left, joystick.right);
        startedAt = millis(); // holding the stick keeps the auto-stop from firing
    }
}

Screen pickScreen()
{
    if (!WiFi.isConnected())
        return Screen::Connecting;
    if (showAddress && millis() - connectedAt < ADDRESS_MS)
        return Screen::Address;
    showAddress = false;
    return Screen::Face;
}

void showText(Screen s)
{
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 16);
    if (s == Screen::Connecting)
    {
        display.println("Connecting WiFi...");
        display.println(WIFI_SSID);
    }
    else
    {
        display.println("Open on your phone:");
        display.println();
        display.println(WiFi.localIP());
    }
    display.display();
}

void setup()
{
    // stop the motors first so they never run while the rest boots
    pinMode(IN1, OUTPUT);
    pinMode(IN2, OUTPUT);
    pinMode(IN3, OUTPUT);
    pinMode(IN4, OUTPUT);
    analogWriteRange(255);
    analogWriteFreq(1000);
    drive(0, 0);

    Serial.begin(115200);
    Serial1.begin(9600); // DFPlayer on D4
    Wire.begin(D2, D1); // SDA, SCL
    randomSeed(ESP.random());

    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
    {
        Serial.println("OLED not found - check wiring and address");
        while (true)
            delay(1000);
    }
    display.setTextColor(SSD1306_WHITE);

    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP); // quicker replies to the phone
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    server.on("/", []()
              { server.send_P(200, "text/html", PAGE); });
    server.on("/cmd", handleCommand);
    server.on("/joy", handleJoystick);
    server.on("/bird", handleBirdSong);
    server.begin();

    showText(screen);

    delay(MP3_BOOT_MS); // WiFi keeps connecting meanwhile
    mp3(0x06, MP3_VOLUME); // set volume
}

void loop()
{
    server.handleClient();

    // also stops the bot if the phone loses connection mid-move
    if (current->ms && millis() - startedAt >= current->ms)
        run(&commands[0]);

    if (current->face == Expression::Dance)
    {
        const Dance &dance = dances[danceIndex];
        int step = (millis() - startedAt) / dance.stepMs;
        if (step >= dance.steps)
            run(&commands[0]);
        else if (step != danceStep)
        {
            danceStep = step;
            drive(dance.moves[step][0], dance.moves[step][1]);
        }
    }

    if (WiFi.isConnected() != wasConnected)
    {
        wasConnected = WiFi.isConnected();
        if (wasConnected)
        {
            Serial.print("Open http://");
            Serial.println(WiFi.localIP());
            showAddress = true;
            connectedAt = millis();
        }
    }

    Screen next = pickScreen();
    bool changed = next != screen;
    screen = next;
    if (screen == Screen::Face)
        faceUpdate(display, changed);
    else if (changed)
        showText(screen);

    int track = current->face == Expression::Dance ? dances[danceIndex].track : faceTrack();
    if (!birding && track != playing)
    {
        playing = track;
        if (track)
            mp3(0x12, track); // play mp3/<track>, replaces whatever was playing
        else
            mp3(0x16, 0); // stop
    }

    int angle = HEAD_CENTER + int(faceLookX() * HEAD_RANGE);
    if (angle != headAngle)
    {
        headAngle = angle;
        headMovedAt = millis();
        // also wakes it if it was resting; MG995 sweeps ~180 degrees over 500..2500us, so angles are real degrees
        head.attach(HEAD_PIN, 500, 2500, angle);
    }
    else if (head.attached() && millis() - headMovedAt >= HEAD_REST_MS)
        head.detach(); // no pulses, no buzz; the gears hold a sideways-turning head in place
}
