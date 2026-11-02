#pragma once
// Focus/break timer. Keeps counting while another pose is showing.
void pomodoroEnter();                 // pose became active: repaint everything
void pomodoroTick(bool visible);      // call every loop; repaints only what changed
void pomodoroTouch(int x, int y);     // tap in rotation-0 coordinates

// Same actions the touch buttons and the physical keys trigger.
enum PomoAction { POMO_MAIN, POMO_RESET, POMO_NEXT, POMO_UP, POMO_DOWN };
void pomodoroAction(PomoAction a);
