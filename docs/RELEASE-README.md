# Chrome MouseMux Compliant Edition

![Chrome Logo](../logo.jpg)

![App Screenshot](screenshot.png)

*Latest revision: August 2026 — Build #56 — Chromium 151.0.7922.77*

> **New in Build #56: rebased onto Chromium 151.**
> Builds up to #55 were based on Chromium 146.0.7650.0. This build moves five
> major Chromium versions forward, picking up everything upstream changed in
> between — security fixes, rendering and performance work included.
>
> The MouseMux functionality is unchanged. No features were added or removed;
> the work was in adapting the integration to Chromium API changes. If you are
> upgrading from #55 and something behaves differently, it is worth reporting,
> because it was not an intended change.

## Multi-Seat Web Browser

The Chrome MouseMux Compliant Edition enables multiple users to each run their own independent browser session on a single computer. Each user can "claim" a browser window, getting their own cursor and full control over their browsing experience. This creates a true multi-seat browsing environment where several people can simultaneously browse the web, each with complete independence, all on one machine.

---

### About Chrome

Google Chrome is a fast, secure, and widely-used web browser developed by Google. Since its launch in 2008, Chrome has become one of the most popular browsers worldwide, known for its speed, simplicity, and powerful developer tools. It offers comprehensive features including tabbed browsing, a vast extension ecosystem through the Chrome Web Store, built-in security protections, seamless Google account integration, and sync capabilities across devices.

Chrome is built on the open-source Chromium project, which is freely available under various open-source licenses. Google's continuous development ensures Chrome stays at the forefront of web standards, performance optimization, and security updates.

Our MouseMux-specific modifications enable the multi-seat functionality while preserving everything that makes Chrome great. The changes are focused on allowing MouseMux to manage multiple independent browser sessions, each claimed by a different user.

---

### How It Works

When you launch Chrome with MouseMux integration enabled, a small **MouseMux Control Dialog** appears alongside the browser. This dialog shows connection status and provides controls for multi-seat operation.

Any connected user can **claim** the browser window by clicking on it. Once claimed, that browser instance belongs to that user.

To enter full multi-seat mode, press **Capture all**. That tells MouseMux to stop each captured device producing native Windows input, so every user's mouse and keyboard reach Chrome only as MouseMux events, routed to that user's own window.

Capture is not optional when several people work at once. Native input is what forces Windows to have a single active window, and a single active window is what makes one user's click extinguish another user's caret. With every device captured, that stops happening: each window keeps its own blinking caret and each user's keystrokes follow their own mouse.

Users are added by opening more windows, not by starting more browsers — see **Adding users, and adding seats** below.

Other users can launch additional Chrome instances and claim those for themselves. Each person ends up with their own browser, their own tabs, their own history for that session, and their own cursor controlling it all. The browsers run independently. One user's actions don't affect another user's session.

This differs fundamentally from simply sharing a browser. In traditional shared browsing, everyone fights over the same cursor, the same tabs, the same session. With the MouseMux Compliant Edition, each person has genuine independence. It's like having multiple computers, but on a single machine.

---

### MouseMux Control Dialog

The control dialog provides the following controls:

**MouseMux Connection** (toggle) — Connects to the MouseMux server. Must be on for multi-seat features to work.

**Native Input Blocking** (toggle) — Prevents the operating system's own mouse and keyboard events from reaching the browser. This is turned on automatically when you enter capture mode, ensuring only your dedicated input reaches the page without duplication.

**Lock Users To Their Window** (toggle) — Off, a user who clicks another window simply moves there. On, clicks landing outside a user's own window are ignored, giving each user one window they cannot leave. Cursors still move freely across the whole screen; only clicks are blocked. A user with no window claims the first one they click, and closing a window frees its user to claim another.

**Owners** (list) — One row per user, showing their device name, the window they are typing in, and whether they are captured. The row acts on that one user without disturbing the others:

| Row control | Effect |
|---|---|
| **Capture** / **Release** | Capture or release just this user's device |
| **Close win** | Close the window this user is working in |
| **Drop** | Remove this user's claim; their device stops driving Chrome until they click again |

An asterisk marks the primary owner — the one reported to the control server and to the older single-owner automation API.

**Capture all** / **Drop all** (buttons) — The same two actions applied to everyone. Capture all is normally what you want: several users can only work at once if every one of them is captured, because capture is what stops a device producing native Windows input.

**+ Window** / **+ Seat** (buttons) — **+ Window** copies the current tab into another window of this browser, already signed in; **+ Seat** starts a separate browser with its own profile and its own login. See **Windows and seats** below — they are not two flavours of the same thing.

**Release Hotkey** (dropdown) — Choose which keyboard shortcut exits capture mode (default: Shift+Escape). This is the escape hatch: it works even when injected input is not reaching the page, so the mice cannot be used to reach this dialog.

**Collapse** (button, footer) — Shrinks the dialog to a small strip out of the way. Click it again to restore. It deliberately leaves something on screen, so there is always a way back.

---

### Normal Mode vs Capture Mode

In **normal mode** (after claiming, before capturing), your mouse controls the browser but still moves freely across the screen. Other users could accidentally click on your window.

In **capture mode**, all your mouse and keyboard input is routed exclusively to this browser window. Other users cannot interfere with your session. This is the recommended way to use Chrome in a multi-seat environment.

To exit capture mode, press the release hotkey (default: Shift+Escape).

---

### Starting Chrome

**The quick way:** double-click **`start-mousemux-chrome.bat`**. It opens this
browser with its own profile, in the `user-data` folder beside it, and does not
touch any Chrome you already have installed. Sign in once there and every user
shares that session.

Start MouseMux first — and if MouseMux needs a browser launched through it to
open its input service, do that once and close it again before running this.

That `.bat` is only a shortcut. It runs `chrome.exe` with switches you could
type yourself, and takes no part in seat tracking. Anything that supplies its
own command line can ignore it.

`launcher.exe` was removed in 2.2.57: the browser now claims and holds its own
seat, which is the only thing the launcher ever did.

### Starting Chrome — command line

Minimum needed to bring up a working browser:

```
chrome.exe --enable-features=MouseMuxIntegration
```

In practice you want a control port and a profile as well:

```
chrome.exe --enable-features=MouseMuxIntegration
           --mousemux-control-port=52001
           --user-data-dir="C:\path\to\user-data-1"
           --no-first-run
```

| Option | Required | What it does |
|---|---|---|
| `--enable-features=MouseMuxIntegration` | **yes** | Turns the integration on. Without it Chrome starts as ordinary Chrome, with no dialog and no MouseMux connection. |
| `--mousemux-control-port=PORT` | no | Starts the control server on `127.0.0.1:PORT` for automation, **and identifies the seat**. Ports 52001–52064 are seats 1–64. |
| `--user-data-dir=PATH` | no | The profile. Each seat needs its own; Chrome refuses to start a second browser on a profile already in use. |
| `--no-first-run` | no | Skips the first-run experience on a fresh profile. |
| `--window-position=X,Y` | no | Where the window opens. |
| `--window-size=W,H` | no | How big it opens. |
| `--do-not-de-elevate` | no | Stops an elevated launch relaunching itself and letting the original process go. |

Note that `--mousemux-control-port` does double duty. Seat *N* is port
`52000 + N`, and a browser started on a port in that range claims seat *N* and
holds it until the process exits — including if it is killed. A browser started
outside that range (or with no control port) works normally but does not occupy
a seat.

### Using an existing Chrome profile

Omit `--user-data-dir` entirely and this browser uses the standard Chrome
profile — the same bookmarks, saved passwords, extensions and logins as the
Chrome already installed on the machine:

```
chrome.exe --enable-features=MouseMuxIntegration
```

Two things follow from that, and they matter:

**Regular Chrome must not be running.** Chrome allows one browser process per
profile. If ordinary Chrome already has that profile open, this one will hand
over its command line and exit, and you will simply get a new window in the
browser that was already running — without MouseMux. Close Chrome first.

**Only one MouseMux browser can use it.** A second `+ Seat` cannot share the
same profile, which is why seats get their own. Several users in the standard
profile is still perfectly possible — that is what **+ Window** is for, and all
those windows share the one profile.

To use a *specific* named profile rather than the default, point
`--user-data-dir` at the real Chrome user-data folder and pick the profile
inside it:

```
chrome.exe --enable-features=MouseMuxIntegration
           --user-data-dir="%LOCALAPPDATA%\Google\Chrome\User Data"
           --profile-directory="Profile 1"
```

A word of warning about that: a profile written by a different Chrome version
can be upgraded on first use, and Chrome does not downgrade profiles again.
If this build is newer than the installed Chrome, opening the profile here may
leave the installed Chrome unable to read it. Copy the folder first if that
profile matters.

The safest arrangement for a shared machine is the middle option: give this
browser its own `--user-data-dir`, sign in once, and let every user work in
windows of it.

### Windows and seats: which one do you need

This is the most important decision in setting the browser up, and it comes
down to one question: **should these people share a login, or not?**

**+ Window** adds a user to *this* browser. Every window of one browser shares
one identity: the same cookies, the same saved passwords, the same signed-in
accounts, the same extensions, the same history. Four people in four windows are
four people inside **one** login.

**+ Seat** starts a *separate* browser, with its own profile, its own control
dialog and its own everything. Two seats share nothing at all — different
logins, different cookies, different history — and a crash in one leaves the
other untouched.

So:

| You want | Use |
|---|---|
| Several people working in the same web application, under one company account | **+ Window** |
| Several people who must each sign in as themselves | **+ Seat** |
| Several people who should not see each other's browsing at all | **+ Seat** |
| One person handing a window to a colleague to work alongside them | **+ Window** |

Windows are the normal case for the situation this browser was built for: a
shared line-of-business application that permits one session, staffed by several
people at once.

#### What "+ Window" actually copies

Pressing **+ Window** does not open a blank window. It **copies the tab you are
currently on** into a new window — the same page, already signed in.

That is deliberate, and the reason is worth understanding, because it decides
the order you do things in.

A login lives in two places at once. Most of it lives in the *profile* — cookies
and saved passwords — and every window of one browser shares that automatically.
But some web applications keep their access token in the *tab* instead, in what
browsers call session storage. That part is not shared with a new window,
because a brand-new window starts with an empty one.

For an ordinary site, either way works. For a site that permits **one signed-in
session at a time**, a blank new window is a disaster: the second sign-in ends
the first, and the person who was already working is thrown out. Copying the tab
carries the session storage across with it, so the new window arrives already
inside the session that is running, and nothing new is signed in.

**Consequence: sign in first, then hand out windows.** A copy can only carry a
session that already exists. If you press + Window before signing in, you get
copies of a signed-out page, which is exactly the blank window you were trying
to avoid.

Two smaller points that follow from the same mechanism:

- Each new window opens on whatever page the first window is showing at that
  moment. Park it on the page people should start from.
- The copy reloads the page rather than photographing it. Anything the site was
  holding purely in memory — a half-filled form, an open dialog — is not carried
  over. The session is.

#### What a seat is, and when the choice is made for you

A seat is a whole separate browser process. Chrome allows **one browser process
per profile**, so two seats can never share a profile — that is a rule of
Chrome's, not a limitation we chose, and it is the reason seats cannot be used
to give several people one login. If you need one login, you need windows.

Seats are numbered 1–64 and cascade 60 pixels down and right of each other so
they stay visually distinct. Chrome picks the lowest free seat itself, and each
seat gets its own control dialog, because a seat is a peer rather than something
the first dialog manages.

Nothing stops you combining the two: three seats, each with two windows, is six
people working under three logins.

---

### The Control Dialog

Each seat has its own control dialog, positioned beside the browser window it belongs to.

**Collapse** shrinks it to a small strip showing just the MouseMux icon and an Expand button, for when it is in the way. Clicking Expand brings the whole dialog back. The strip stays where you left the dialog.

The build number and date appear in the lower left of the dialog, which is the quickest way to confirm which build is running.

---

### Pen and Touch Input

*Experimental in Build #56 — not yet verified on hardware.*

MouseMux sends pen and touch devices down a separate channel from mice, carrying pressure, tilt and barrel rotation. This build receives that channel and delivers it to web pages as pointer input: a page reads `pointerType` as `"pen"` or `"touch"`, along with `pressure`, `tiltX`, `tiltY` and `twist` values. A touchpad is deliberately reported as a mouse, since it moves a cursor rather than making contact with the screen.

**This has not been confirmed on a pen tablet or touchscreen.** Treat it as untested rather than as a working feature.

Native input blocking deliberately does **not** cover pen and touch in this build. Windows delivers those through a different message family than mouse input, and suppressing that family is only safe once injection is known to work — if it did not, a touchscreen would stop responding entirely, with no way back short of restarting the browser. So while native input blocking is on, a pen or touchscreen keeps working through the normal Windows path. That is weaker isolation, and a far safer failure mode.

Two limits worth knowing:

- Input arrives as pointer events, not touch events. Multi-touch gestures — pinch to zoom, two-finger scrolling, fling — do not work. Sites built around touch gestures, such as map interfaces, will not respond to them.
- Pen movement is sampled rather than continuous, so extremely fast strokes may show slight angularity.

---

### Use Cases

Each of these is really a choice between windows and seats — see **Windows and
seats** above. Sharing one account means windows; being yourself means seats.

**Shared Workstations.** In offices or labs where multiple people share a single computer, each person can have their own browser session. No need to log out, close tabs, or worry about someone else seeing your browsing. Claim a browser, do your work, release it when done.

**Computer Labs and Libraries.** Educational institutions can set up workstations where multiple students work simultaneously. Each student claims their own Chrome instance for research, assignments, or general browsing. Instructors can browse alongside students, looking up reference materials or demonstrating techniques.

**Collaborative Research.** Research teams can work together on a shared workstation, each person browsing different sources, comparing information, and gathering materials. Everyone sees their own browser with their own cursor, but they're all in the same physical space, able to discuss and share findings in real-time.

**Family Computers.** A shared family computer becomes more practical when each family member can have their own browser session running simultaneously. Parents and children can browse independently without interfering with each other's sessions.

**Kiosk and Display Systems.** Multi-user kiosks can serve several people at once. Each user claims a browser instance and browses independently. When they walk away, the session can be released for the next user.

**Training and Demonstrations.** Trainers can browse reference materials while trainees follow along in their own browser instances. Everyone participates actively rather than passively watching a single shared screen.

---

### Technical Details

Input does not arrive through Windows. MouseMux reports every device's movement
and keystrokes to this browser over a local connection, and the browser decides
where each event belongs — which is why several people can type at once, and why
Windows' rule of one active window at a time stops applying.

A user is a **device pair**: one mouse and one keyboard, assigned to the same
user inside MouseMux. That pairing is what makes typing work. The mouse claims a
window by clicking in it, and the keyboard follows its own mouse. If a keyboard
is not assigned to a user in MouseMux, nothing in what we receive identifies
whose keystrokes it is sending, and with several people working the browser will
ignore it rather than risk typing into somebody else's window. Check that
pairing first if someone's typing goes astray — the control dialog shows each
user's keyboard on their row, and where typing is actually landing on the line
beneath.

**Capture is a requirement, not a convenience.** While a device is captured,
MouseMux stops it producing ordinary Windows input, and everything it does
reaches the browser through us instead. Without capture, Windows is still moving
focus around behind the browser's back — and that is precisely what makes one
person's click stop another person's typing. Capture everyone, or the rest of
this does not hold.

**Lock Users To Their Window** confines each user to the window they first
clicked in. Cursors still move freely across all monitors — a cursor stopping at
an invisible wall reads as broken hardware — but clicks outside a user's own
window are ignored. With several people side by side this is usually what you
want.

What is shared, and what is not:

| | Between windows of one browser | Between seats |
|---|---|---|
| Cookies and signed-in accounts | Shared | Not shared |
| Saved passwords, bookmarks, extensions | Shared | Not shared |
| History and downloads | Shared | Not shared |
| Session storage, where some sites keep their token | Only through **+ Window**, which copies it | Not shared |
| The page each person is on | Independent | Independent |

One limit worth knowing: Chrome allows **one context menu open at a time for the
whole browser**. If two people right-click at the same moment, the second menu
replaces the first. Dropdown lists inside pages are unaffected — everybody can
have one open at once.

---

### Performance Considerations

Running multiple browser instances requires more system resources than a single browser. Each Chrome instance consumes memory for its tabs, media, and rendering. Modern systems handle this well, but keep the following in mind:

- **Memory:** Each Chrome instance uses 200-500MB base, plus additional memory per tab
- **CPU:** Multiple browsers increase CPU load, especially with media-heavy sites
- **Recommended:** 16GB RAM for 3-4 simultaneous browser instances with moderate tab counts
- **Minimum:** 8GB RAM for 2 browser instances with light usage

For optimal performance, close unused tabs and browser instances when not needed.

---

### Getting Started

1. Ensure MouseMux is running — **MouseMux V3 (3.0.x series)**
2. Run **`start-mousemux-chrome.bat`** — or start `chrome.exe` yourself with
   `--enable-features=MouseMuxIntegration` (see **Starting Chrome** above)
3. The MouseMux Control Dialog appears — turn on the **MouseMux** toggle
4. **Sign in to the site everyone will share** — once, in this first window.
   Do this before the next step: **+ Window** copies the tab you are on, so it
   can only hand out a session that already exists
5. Press **+ Window** once for each additional user. Each new window opens on
   the same page, already signed in
6. Each user clicks in their own window to claim it — their name appears in the
   **Owners** list, with the window they are working in and the keyboard
   assigned to them
7. Press **Capture all** — required for several users at once, because capture
   is what stops each device producing native Windows input
8. Everyone works at the same time, each with their own cursor and caret
9. Optionally turn on **Lock Users To Their Window** so nobody can click into
   somebody else's window
10. Press the release hotkey (default: Shift+Escape) to release capture

---

### Privacy and session management

Be clear which arrangement you are running, because the two have opposite
privacy properties.

**Windows of one browser are not private from each other.** They share one
identity by design. Anybody in any window can open the history, read the saved
passwords, or use the signed-in account. That is exactly what you want when a
team shares a company account, and unacceptable if the people involved should
not see each other's work. Use seats for that.

**Seats are private from each other.** Separate profiles mean separate cookies,
separate history, separate saved passwords. Nothing crosses between them.

Either way:

- Sign out of sensitive sites before handing the machine to someone else.
- A profile started with `--user-data-dir` keeps everything between runs. That
  is usually what you want for a fixed installation — sign in once and it stays
  signed in — and exactly what you do *not* want on a shared or public machine.
- Where nothing should persist, give each run a fresh `--user-data-dir`, or
  delete the folder between shifts.

---

*Chrome is a trademark of Google LLC. The MouseMux Compliant Edition is a derivative work created to enable multi-seat browser functionality. This distribution includes Chrome binaries along with MouseMux integration components.*
