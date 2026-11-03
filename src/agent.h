#pragma once
// Voice agent: hold PLUS to talk -> Groq (speech to text) -> n8n webhook (AI agent) -> OpenAI (text to speech).
void agentInit();                  // starts the worker task (needs sound + net up)
void agentEnter();                 // pose became active: repaint, bring the radio up
void agentLeave();                 // pose left: abort whatever is running, radio off
void agentTick(bool visible);      // animation + status; call every loop
void agentPress();                 // PLUS pushed down
void agentRelease();               // PLUS let go
void agentBoot();                  // BOOT: history browser when idle, cancels the turn when busy
void agentTouch();                 // screen tap: wakes Krakbot and makes it react; leaves the history
void agentSkinNext();               // long BOOT: switch character (look + voice)
void agentRepaint();                // something drew over the screen (overlay): paint everything again
