# The phone

What the listener is on a phone, thought through rather than shrunk. The desktop is the one human
surface; the phone is the same client, the same world and the same record, at thumb size and in a
pocket. A design, 2026-11-11, with what is built marked.

## Two modes, one record

**TOUCH** is the phone's own shape: one conversation, bare, with the line and a bar across the bottom
you hold while you speak. [ people ] picks who to talk to; [ desk ] returns to the tabs. Built.

**DESK** is the desktop's panes as tabs at phone width: CHAT, GRAPH, PRINCIPALS, HISTORY, SESSION,
PANES, SETTINGS. It is where you go to inspect, approve in bulk, read the log, pair, and set things;
TOUCH is where you live. Built.

The mode is a pref of the phone (`view.touch`), not of the person: the same person is in DESK on the
Mac and in TOUCH on the phone. Built.

## The fold

A foldable has two screens and a change between them. The rule: **the outer screen is TOUCH, the inner
screen is DESK, unless the person said otherwise on that screen.** Fold it and you are holding a
conversation; open it and you are at a desk. The mode pref becomes two prefs, keyed by the screen's
width class (narrow under 500 pt, wide from there), and the active one follows the screen.

The mechanics: on a fold or unfold iOS resizes the window's scene; SDL reports it as a resize and a
safe-area change; the frame reads the window size every frame and the safe area from UIKit, so the
layout follows. What must not happen: a window pinned to the size it was created at. The window is
created at the screen's size in the device's own orientation and must stay resizable under SDL's
fullscreen on iOS; the phone's console logs every screen change (`screen: event …`) so a fold that does
not follow can be seen. Open: verified on the simulator for rotation, not yet on the device for the
fold.

## The line and the keyboard

The line is where words go, typed or spoken. The keyboard rises on a tap on the line and never on
its own; the panes shrink above it so nothing leaves the screen. The keyboard's own dictation key
works in the line like any typing. The line keeps what was typed through a reconnect and a mode
switch. Built, except the second sentence of the last clause.

## Voice

The bar in TOUCH, and [ TALK ] beside the line in DESK, are the same gesture: hold, speak, release.
The recording goes to the world as one `speech.transcribe`, the world hands it to a Model with
`model/stt`, and the text lands in the line to read before sending. Nothing is sent by voice alone;
the person always sees the words first. No Model that hears: the bar says so and does nothing.
Built; untried on the device's microphone.

Later: the reply read back (`speech.say`, a Model with `model/tts`), as a switch in TOUCH, off by
default, so a walk can be a conversation.

## Cards

An approval on the phone is the same card as on the desk: one line a person can decide on, APPROVE
and DENY as brackets. In TOUCH the card sits in the conversation where it arrived, at the same size
as the bar's text, so a thumb lands on it. A card that needs reading (a diff, a command with a path)
opens in DESK with one tap on its summary. Mostly built; the thumb-size pass is open.

## Notices

The notices chip in the bottom line shows the count; a tap opens the list. A loud notice is the
phone's own notification when the app is in the background, which needs the relay the punch list
names; until then, the chip. Open.

## Who you are talking to

TOUCH opens on root. [ people ] shows the PRINCIPALS list; a tap switches the conversation and
returns. The name at the top is always who the line sends to. A swipe between conversations is a
later gesture, once there are usually more than two.

## Text at phone width

Nothing runs past the edge. A column that would overflow is cut with an ellipsis and its full text is
a tap away; a hint wraps; a path is cut from the left so the file name stays. The GRAPH's "n
evaluations · tokens" line and the PANES hint are the known offenders. Open.

## Pairing

A phone is paired, never given a token by hand: DEVICES → pair phone → begin pairing on a desktop
listener shows a code and a QR; the Camera opens the app on SETTINGS with the world and code filled
in; `claim` trades it for a token of this phone alone, labelled and revocable from DEVICES. Built;
the Camera path untried on the device.

## What is not on the phone

Terminals, files, TeX and search run in the world and the phone draws them, so PANES is the same as
on the desk; but editing a file on a phone is a thing nobody wants. Shells and files stay readable,
and the editor opens read-only below 500 pt. Sign in with ChatGPT needs a loopback callback a phone
cannot hold; the phone says so and points at a desktop. Both as designed.

## Order of work

1. The fold: confirm the resize path on the device from its console; the per-screen mode pref.
2. Text at phone width: ellipsis and wrap rules in the shared widgets, so the desk gets them too.
3. Cards at thumb size in TOUCH.
4. The reply read back.
