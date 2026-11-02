#pragma once
#include <Arduino.h>

// remote control page served to the phone
const char PAGE[] PROGMEM = R"html(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Bot Remote</title>
<style>
body{margin:0;padding:16px;font-family:sans-serif;background:#111;color:#eee;text-align:center}
h1{font-size:20px;margin:8px 0}
#status{font-size:32px;font-weight:bold;margin:12px 0}
#hint{font-size:14px;color:#aaa;min-height:18px;margin:10px 0}
#joy{position:relative;width:240px;height:240px;margin:16px auto;border-radius:50%;background:#222;border:2px solid #444;touch-action:none}
#knob{position:absolute;left:50%;top:50%;width:80px;height:80px;margin:-40px 0 0 -40px;border-radius:50%;background:#555;pointer-events:none}
button{font-size:28px;padding:20px 0;border:0;border-radius:12px;background:#333;color:#eee;touch-action:manipulation}
#mic,#dance,#bird{width:100%;max-width:320px;font-size:20px;padding:16px;background:#246}
#dance{background:#629;margin-bottom:10px}
#bird{background:#274;margin-bottom:10px}
#mic.on{background:#2a6}
#mic:disabled{opacity:.4}
input{width:100%;max-width:320px;box-sizing:border-box;font-size:18px;padding:12px;border-radius:8px;border:1px solid #555;background:#222;color:#eee}
</style>
</head>
<body>
<h1>Bot Remote</h1>
<div id="status">STOP</div>
<div id="joy"><div id="knob"></div></div>
<button id="dance">Dance</button>
<button id="bird">Bird Song</button>
<button id="mic">Start voice</button>
<div id="hint">Say: forward, back, left, right, dance, stop</div>
<input id="typed" placeholder="Or tap here and use your keyboard mic" autocomplete="off">
<script>
const statusEl = document.getElementById('status');
const hint = document.getElementById('hint');
const mic = document.getElementById('mic');
const typed = document.getElementById('typed');

const WORDS = new Map([
  ['forward', 'forward'], ['go', 'forward'], ['ahead', 'forward'],
  ['back', 'back'], ['backward', 'back'], ['backwards', 'back'], ['reverse', 'back'],
  ['left', 'left'], ['right', 'right'],
  ['stop', 'stop'], ['halt', 'stop'],
  ['dance', 'dance'],
]);

// command words in what was heard, the last one wins
function parse(text) {
  const found = (text.toLowerCase().match(/[a-z]+/g) || []).filter(w => WORDS.has(w));
  return { cmd: found.length ? WORDS.get(found[found.length - 1]) : null, count: found.length };
}

function send(cmd) {
  statusEl.textContent = cmd.toUpperCase();
  fetch('/cmd?c=' + cmd).catch(() => { statusEl.textContent = 'NO CONNECTION'; });
}

document.getElementById('dance').addEventListener('click', () => send('dance'));

// each tap plays the next bird song, the joystick stops it
document.getElementById('bird').addEventListener('click', () => {
  fetch('/bird').then(r => r.text())
    .then(n => { hint.textContent = 'Bird song ' + n + ', tap again for the next one'; })
    .catch(() => { statusEl.textContent = 'NO CONNECTION'; });
});

// joystick: x right +, y forward +, each -255..255
const joy = document.getElementById('joy');
const knob = document.getElementById('knob');
let joyX = 0, joyY = 0, held = false, busy = false;

// one request at a time so a slow wifi link doesn't pile them up
function pushJoy() {
  if (busy) return;
  busy = true;
  fetch('/joy?x=' + joyX + '&y=' + joyY)
    .catch(() => { statusEl.textContent = 'NO CONNECTION'; })
    .finally(() => { busy = false; });
}

function moveJoy(e) {
  const r = joy.getBoundingClientRect();
  const max = r.width / 2;
  let dx = e.clientX - r.left - max;
  let dy = e.clientY - r.top - max;
  const d = Math.hypot(dx, dy);
  if (d > max) { dx *= max / d; dy *= max / d; }
  knob.style.transform = 'translate(' + dx + 'px,' + dy + 'px)';
  const dead = d < max * 0.15; // a wobbly thumb near the middle doesn't move the bot
  joyX = dead ? 0 : Math.round(dx / max * 255);
  joyY = dead ? 0 : Math.round(-dy / max * 255);
  pushJoy();
}

function releaseJoy() {
  if (!held) return;
  held = false;
  knob.style.transform = '';
  send('stop');
}

joy.addEventListener('pointerdown', e => {
  joy.setPointerCapture(e.pointerId);
  held = true;
  statusEl.textContent = 'JOYSTICK';
  moveJoy(e);
});
joy.addEventListener('pointermove', e => { if (held) moveJoy(e); });
joy.addEventListener('pointerup', releaseJoy);
joy.addEventListener('pointercancel', releaseJoy);

// keep sending while held, the bot stops itself if these stop arriving
setInterval(() => { if (held) pushJoy(); }, 150);

// keyboard mic: works on any phone, no https needed
let typingTimer;
typed.addEventListener('input', () => {
  clearTimeout(typingTimer);
  typingTimer = setTimeout(() => {
    const p = parse(typed.value);
    hint.textContent = 'Heard: ' + typed.value;
    if (p.cmd) {
      send(p.cmd);
      typed.value = '';
    }
  }, 400);
});

// voice button: browser speech recognition, blocked on plain http unless allowed in chrome://flags
const SR = window.SpeechRecognition || window.webkitSpeechRecognition;
let rec = null;
let listening = false;
let lastKey = '';

function setListening(on) {
  listening = on;
  mic.classList.toggle('on', on);
  mic.textContent = on ? 'Listening... tap to stop' : 'Start voice';
}

if (!SR) {
  mic.disabled = true;
  hint.textContent = 'Voice button not supported here, use the keyboard mic below';
} else {
  rec = new SR();
  rec.lang = 'en-US';
  rec.continuous = true;
  rec.interimResults = true;

  rec.onresult = e => {
    const i = e.results.length - 1;
    const text = e.results[i][0].transcript;
    const p = parse(text);
    const key = i + ':' + p.count;
    hint.textContent = 'Heard: ' + text;
    if (p.cmd && key !== lastKey) {
      lastKey = key;
      send(p.cmd);
    }
  };

  rec.onerror = e => {
    if (e.error === 'not-allowed' || e.error === 'service-not-allowed') {
      setListening(false);
      hint.textContent = 'Mic blocked on this page, use the keyboard mic below';
    } else if (e.error === 'network') {
      setListening(false);
      hint.textContent = 'Voice needs internet on your phone';
    }
  };

  // browsers stop listening after a pause, so start again
  rec.onend = () => {
    if (!listening) return;
    lastKey = '';
    setTimeout(() => {
      try { rec.start(); } catch (err) { setListening(false); }
    }, 250);
  };

  mic.addEventListener('click', () => {
    if (listening) {
      setListening(false);
      rec.stop();
      return;
    }
    setListening(true);
    lastKey = '';
    try { rec.start(); } catch (err) { setListening(false); }
  });
}
</script>
</body>
</html>
)html";
