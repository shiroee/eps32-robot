#include "face.h"

#define EYE_W 36
#define EYE_H 36
#define EYE_R 8
#define EYE_GAP 14
#define LOOK_X 18 // max sideways eye travel (px)
#define LOOK_Y 10 // max up/down eye travel (px)
#define FRAME_MS 30
#define HAPPY_MS 1200 // happy squint after a move ends
#define SLEEP_MS 60000 // doze off after this long without a command
#define PLAY_MS 120000 // asleep this long, wake up and play
#define GAME_MS 20000  // switch to the next game after this long
#define BALL_R 3
#define BALL_GRAVITY 0.16f // px per frame per frame
#define WALL 2             // room at the sides for the pong paddles
#define PADDLE_H 14

namespace
{
    struct Pose
    {
        float lookX; // -1 left .. 1 right
        float lookY; // -1 up .. 1 down
        float width; // eye size multipliers
        float height;
        float lid;   // >0 focused (inner corners down), <0 worried (outer corners down)
        float happy; // 0..1, bottom cut away like ^ ^
    };

    // same order as Expression
    const Pose poses[] = {
        // lookX lookY width height lid happy
        {0, 0, 1.0, 1.0, 0, 0},       // Idle
        {0, -0.3, 1.1, 0.9, 0.6, 0},  // Forward: focused
        {0, 0.6, 0.9, 0.85, -0.6, 0}, // Backward: worried, looking down
        {-1, 0, 1.0, 0.9, 0, 0},      // Left
        {1, 0, 1.0, 0.9, 0, 0},       // Right
        {0, -0.2, 1.05, 1.0, 0, 0.75}, // Happy
        {0, 0.4, 1.1, 1.0, 0, 0},     // Sleep: resting low, shut via openTarget
        {0, 0, 0.9, 0.9, 0, 0.4},     // Play: each game changes it, see updatePlay
        {0, 0, 0.9, 0.9, 0, 0.75},    // Dance: updatePlay sways it
    };

    // mp3/NNNN_*.mp3 numbers made by music/make_music.py, same order as Expression, 0 = quiet
    const int tracks[] = {0, 1, 2, 3, 4, 5, 6, 0, 0}; // Play: gameTracks, Dance: main.cpp

    // played one after another while left alone
    enum class Game
    {
        Ball,     // bounce a ball, the eyes follow it
        Pong,     // endless rally between two paddles
        Dizzy,    // roll the eyes round and round
        Dance,    // hop to the beat with music notes
        Peekaboo, // shrink away, then pop back big
        Count,
    };

    const int gameTracks[] = {7, 8, 9, 10, 11}; // same order as Game

    struct Ball
    {
        float x, y, vx, vy;
    };

    struct Eye
    {
        int x, y, w, h;
    };

    struct Frame
    {
        Eye left, right;
        int lid;       // percent, see Pose
        int happy;     // percent
        int floater;   // char drifting up beside the eyes (z, music note), 0 = none
        int floatStep; // how far along it is
        int game;      // -1 = not playing
        int ballX, ballY;
    };

    Expression expression = Expression::Idle;
    Pose target = poses[0];
    Pose pose = poses[0];
    float open = 1; // 1 open .. 0 closed
    float openTarget = 1;
    bool blinkAgain = false;
    unsigned long nextBlink = 2000;
    bool glancing = false;
    unsigned long nextGlance = 3000;
    unsigned long glanceEnd = 0;
    unsigned long happyStart = 0;
    unsigned long lastSet = 0; // last faceSet, idle eyes doze off SLEEP_MS after it
    unsigned long lastFrame = 0;
    Game game = Game::Ball;
    unsigned long gameStart = 0;
    Ball ball;
    Frame lastDrawn;

    bool due(unsigned long now, unsigned long at)
    {
        return (long)(now - at) >= 0;
    }

    bool moving()
    {
        return (expression >= Expression::Forward && expression <= Expression::Right) || expression == Expression::Dance;
    }

    bool playing(Game g)
    {
        return expression == Expression::Play && game == g;
    }

    bool dancing()
    {
        return expression == Expression::Dance || playing(Game::Dance);
    }

    void ease(float &value, float to, float amount)
    {
        value += (to - value) * amount;
    }

    void updateBlink(unsigned long now)
    {
        if (expression == Expression::Sleep)
            return;
        if (openTarget == 1 && due(now, nextBlink))
        {
            openTarget = 0;
            blinkAgain = random(100) < 20;
            nextBlink = now + random(2000, 6000);
        }
        else if (openTarget == 0 && open < 0.05f)
        {
            openTarget = 1;
            if (blinkAgain)
            {
                blinkAgain = false;
                nextBlink = now + 150;
            }
        }
    }

    // idle eyes look around now and then
    void updateGlance(unsigned long now)
    {
        if (expression != Expression::Idle)
            return;
        if (glancing && due(now, glanceEnd))
        {
            glancing = false;
            target.lookX = 0;
            target.lookY = 0;
            nextGlance = now + random(2000, 6000);
        }
        else if (!glancing && due(now, nextGlance))
        {
            glancing = true;
            target.lookX = random(-100, 101) / 100.0f;
            target.lookY = random(-50, 51) / 100.0f;
            glanceEnd = now + random(600, 1800);
        }
    }

    void startGame(Game g, unsigned long now)
    {
        game = g;
        gameStart = now;
        ball = {BALL_R + WALL, BALL_R, 2, 1}; // drops in from the top left corner
    }

    // bounce the ball off the screen edges (gravity 0 = straight lines), the eyes follow it
    void moveBall(int screenW, int screenH, float gravity)
    {
        ball.x += ball.vx;
        ball.vy += gravity;
        ball.y += ball.vy;
        if (ball.x < BALL_R + WALL || ball.x > screenW - 1 - BALL_R - WALL)
        {
            ball.vx = -ball.vx;
            ball.x = constrain(ball.x, BALL_R + WALL, screenW - 1 - BALL_R - WALL);
        }
        if (ball.y < BALL_R)
        {
            ball.y = BALL_R;
            ball.vy = fabsf(ball.vy);
        }
        if (ball.y > screenH - 1 - BALL_R)
        {
            ball.y = screenH - 1 - BALL_R;
            // with gravity: a fresh kick each bounce so it never dies down
            ball.vy = gravity ? -random(30, 43) / 10.0f : -ball.vy;
        }
        target.lookX = (ball.x - screenW / 2) / (screenW / 2);
        target.lookY = (ball.y - screenH / 2) / (screenH / 2);
    }

    void updatePlay(unsigned long now, int screenW, int screenH)
    {
        if (expression == Expression::Dance)
            target.lookX = sinf((now - lastSet) / 1000.0f * PI); // same sway as the Dance game
        if (expression != Expression::Play)
            return;
        if (now - gameStart >= GAME_MS)
            startGame(Game((static_cast<int>(game) + 1) % static_cast<int>(Game::Count)), now);
        float t = (now - gameStart) / 1000.0f; // seconds into this game
        target = poses[static_cast<int>(Expression::Play)];
        switch (game)
        {
        case Game::Ball:
            moveBall(screenW, screenH, BALL_GRAVITY);
            break;
        case Game::Pong:
            moveBall(screenW, screenH, 0);
            break;
        case Game::Dizzy:
            target.lookX = cosf(t * 6);
            target.lookY = sinf(t * 6);
            target.happy = 0;
            break;
        case Game::Dance: // sway here, layout() adds the hops
            target.lookX = sinf(t * PI);
            target.happy = 0.75f;
            break;
        case Game::Peekaboo:
        {
            bool hidden = fmodf(t, 2.5f) < 1.2f;
            target.width = target.height = hidden ? 0 : 1.2f;
            target.happy = hidden ? 0 : 0.75f;
            break;
        }
        default:
            break;
        }
    }

    Frame layout(int screenW, int screenH, unsigned long now)
    {
        float bob = dancing()                       ? -fabsf(sinf(now / 160.0f)) * 6 // hop to the beat
                    : moving()                        ? sinf(now / 90.0f) * 2
                    : expression == Expression::Sleep ? sinf(now / 700.0f) * 2          // slow breathing
                                                      : 0;
        int w = int(EYE_W * pose.width);
        // the eye on the side being looked at gets a bit bigger
        int lh = max(2, int(EYE_H * pose.height * (1 - 0.12f * pose.lookX) * open));
        int rh = max(2, int(EYE_H * pose.height * (1 + 0.12f * pose.lookX) * open));
        int cx = screenW / 2 + int(pose.lookX * LOOK_X);
        int cy = screenH / 2 + int(pose.lookY * LOOK_Y + bob);

        Frame f;
        f.left = {cx - EYE_GAP / 2 - w, cy - lh / 2, w, lh};
        f.right = {cx + EYE_GAP / 2, cy - rh / 2, w, rh};
        f.lid = int(pose.lid * 100);
        f.happy = int(pose.happy * 100);
        f.floater = expression == Expression::Sleep ? 'z' : dancing() ? 14 : 0; // 14 = music note in the built-in font
        f.floatStep = f.floater ? (now / 100) % 48 : 0;
        f.game = expression == Expression::Play ? static_cast<int>(game) : -1;
        f.ballX = int(ball.x);
        f.ballY = int(ball.y);
        return f;
    }

    void drawEye(Adafruit_SSD1306 &d, const Eye &e, int lid, int happy, bool isLeft)
    {
        int r = min(EYE_R, min(e.w, e.h) / 2);
        d.fillRoundRect(e.x, e.y, e.w, e.h, r, SSD1306_WHITE);

        // slanted lid: focused lowers the inner corner, worried the outer one
        int depth = abs(lid) * e.h / 200;
        if (depth > 0)
        {
            bool lowRight = (lid > 0) == isLeft;
            int tipX = lowRight ? e.x + e.w : e.x - 1;
            d.fillTriangle(e.x - 1, e.y - 1, e.x + e.w, e.y - 1, tipX, e.y + depth, SSD1306_BLACK);
        }

        // happy: a circle bites the bottom off, leaving an arc
        int cut = happy * e.h / 100;
        if (cut > 0)
        {
            int radius = e.w * 3 / 5;
            d.fillCircle(e.x + e.w / 2, e.y + e.h - cut + radius, radius, SSD1306_BLACK);
        }
    }

    // three of c drifting up from the right eye, growing as they rise
    void drawFloaters(Adafruit_SSD1306 &d, int c, int step)
    {
        for (int i = 0; i < 3; i++)
        {
            int t = (step + i * 16) % 48;
            // x stays under 116 so a size 2 char still fits on screen, inverse so it shows over an eye
            d.drawChar(d.width() - 26 + t / 4, d.height() / 2 - 10 - t / 2, c, SSD1306_INVERSE, SSD1306_INVERSE, t < 24 ? 1 : 2);
        }
    }

    // the ball, plus paddles for pong
    void drawToys(Adafruit_SSD1306 &d, const Frame &f)
    {
        if (f.game == static_cast<int>(Game::Pong))
        {
            int y = constrain(f.ballY - PADDLE_H / 2, 0, d.height() - PADDLE_H);
            d.fillRect(0, y, WALL, PADDLE_H, SSD1306_WHITE);
            d.fillRect(d.width() - WALL, y, WALL, PADDLE_H, SSD1306_WHITE);
        }
        if (f.game == static_cast<int>(Game::Ball) || f.game == static_cast<int>(Game::Pong))
            d.fillCircle(f.ballX, f.ballY, BALL_R, SSD1306_INVERSE); // black where it crosses an eye
    }
}

Expression faceSet(Expression e)
{
    // a little happy squint when a move ends
    if (e == Expression::Idle && moving())
    {
        e = Expression::Happy;
        happyStart = millis();
    }
    if (e == Expression::Sleep)
        openTarget = 0.1f; // shut to a thin line
    else if (expression == Expression::Sleep)
        openTarget = 1; // wake up
    if (e == Expression::Play)
        startGame(Game::Ball, millis());
    expression = e;
    target = poses[static_cast<int>(e)];
    glancing = false;
    nextGlance = millis() + random(2000, 5000);
    lastSet = millis();
    return e;
}

void faceUpdate(Adafruit_SSD1306 &display, bool force)
{
    unsigned long now = millis();
    if (!force && now - lastFrame < FRAME_MS)
        return;
    lastFrame = now;

    if (expression == Expression::Happy && now - happyStart >= HAPPY_MS)
        faceSet(Expression::Idle);
    if (expression == Expression::Idle && now - lastSet >= SLEEP_MS)
        faceSet(Expression::Sleep);
    else if (expression == Expression::Sleep && now - lastSet >= PLAY_MS)
        faceSet(Expression::Play);
    updateBlink(now);
    updateGlance(now);
    updatePlay(now, display.width(), display.height());

    ease(pose.lookX, target.lookX, 0.25f);
    ease(pose.lookY, target.lookY, 0.25f);
    ease(pose.width, target.width, 0.25f);
    ease(pose.height, target.height, 0.25f);
    ease(pose.lid, target.lid, 0.25f);
    ease(pose.happy, target.happy, 0.25f);
    ease(open, openTarget, expression == Expression::Sleep ? 0.03f : 0.6f); // droop shut slowly, pop open fast

    Frame f = layout(display.width(), display.height(), now);
    if (!force && memcmp(&f, &lastDrawn, sizeof f) == 0)
        return;
    lastDrawn = f;

    display.clearDisplay();
    drawEye(display, f.left, f.lid, f.happy, true);
    drawEye(display, f.right, f.lid, f.happy, false);
    if (f.floater)
        drawFloaters(display, f.floater, f.floatStep);
    drawToys(display, f);
    display.display();
}

float faceLookX()
{
    return pose.lookX;
}

int faceTrack()
{
    if (expression == Expression::Play)
        return gameTracks[static_cast<int>(game)];
    return tracks[static_cast<int>(expression)];
}
