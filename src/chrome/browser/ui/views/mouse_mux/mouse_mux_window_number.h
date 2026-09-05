// MouseMux: a small, stable number per browser window.
//
// Shown in the window caption ("[2] Gmail") and in the control dialog's users
// list ("Window 2 \u00b7 Gmail"), so a person can tell which window a row is
// talking about.  Assigned the first time a window is asked about, in that
// order, and kept for as long as the window lives: closing window 1 must not
// turn window 2 into window 1 under somebody's feet.  Keyed by the browser's
// session id, which is unique for the life of the process.
//
// Header-only on purpose: two callers in different targets, no BUILD.gn edit.

#ifndef CHROME_BROWSER_UI_VIEWS_MOUSE_MUX_MOUSE_MUX_WINDOW_NUMBER_H_
#define CHROME_BROWSER_UI_VIEWS_MOUSE_MUX_MOUSE_MUX_WINDOW_NUMBER_H_

#include <map>

#include "components/sessions/core/session_id.h"

namespace mouse_mux {

inline int WindowNumberForSession(SessionID id) {
  static std::map<SessionID::id_type, int>* numbers =
      new std::map<SessionID::id_type, int>();
  static int next = 0;
  auto it = numbers->find(id.id());
  if (it != numbers->end()) {
    return it->second;
  }
  (*numbers)[id.id()] = ++next;
  return next;
}

}  // namespace mouse_mux

#endif  // CHROME_BROWSER_UI_VIEWS_MOUSE_MUX_MOUSE_MUX_WINDOW_NUMBER_H_
