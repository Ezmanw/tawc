//! The home screen's terminal: a pending shell goes in use on input and
//! back to pending once it's an untouched prompt again with nothing else
//! in its session (notes/terminal.md "Pending vs in use"). Typed into
//! through the real IME path (`input`), so MainActivity must be visible.

use std::time::{Duration, Instant};

use tawc_integration::adb;

fn wait_state(want: &str) {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let state = adb::terminal_state().expect("terminal-state");
        if state == want {
            return;
        }
        assert!(Instant::now() < deadline, "terminal-state {state:?}, want {want:?}");
        std::thread::sleep(Duration::from_millis(100));
    }
}

/// `want` for all of `window` (no flip-flop to pending).
fn hold_state(want: &str, window: Duration) {
    let end = Instant::now() + window;
    while Instant::now() < end {
        assert_eq!(adb::terminal_state().expect("terminal-state"), want);
        std::thread::sleep(Duration::from_millis(200));
    }
}

/// Type `line` (no spaces: `input text` splits on them; use `%s`).
fn type_text(text: &str) {
    adb::shell(&format!("input text '{text}'")).expect("input text");
}

fn key(code: u32) {
    adb::shell(&format!("input keyevent {code}")).expect("input keyevent");
}

const ENTER: u32 = 66;
const DEL: u32 = 67;

fn run(line: &str) {
    type_text(line);
    key(ENTER);
}

#[test]
fn test_terminal_returns_to_pending_when_idle() {
    tawc_integration::helpers::test_init();
    adb::shell("am start -n me.phie.tawc/.MainActivity").expect("start MainActivity");
    adb::home_pane("terminal").expect("home-pane terminal");
    wait_state("pending");
    // Let bash print its first prompt before typing.
    std::thread::sleep(Duration::from_secs(1));

    // Typed, then erased: back to pending, hold released.
    type_text("ab");
    wait_state("inUse:1");
    assert!(
        adb::session_state().expect("session-state").iter().any(|r| r.starts_with("terminal ")),
        "in-use shell holds no session reason"
    );
    key(DEL);
    key(DEL);
    wait_state("pending");
    let reasons = adb::session_state().expect("session-state");
    assert!(!reasons.iter().any(|r| r.starts_with("terminal ")), "demoted shell still holds: {reasons:?}");

    // A command ran: erasing a later line doesn't count, `clear` does.
    run("true");
    type_text("x");
    key(DEL);
    hold_state("inUse:1", Duration::from_secs(2));
    run("clear");
    wait_state("pending");

    // A background job keeps it in use through `clear`.
    run("sleep%s300%s&");
    run("clear");
    hold_state("inUse:1", Duration::from_secs(2));
    // Separate lines: bash prints the job's "Terminated" before the
    // next prompt.
    run("kill%s%1");
    run("clear");
    wait_state("pending");

    // So does a foreground program that clears the screen.
    run("clear;cat");
    hold_state("inUse:1", Duration::from_secs(2));
    adb::shell("input keycombination 113 31").expect("ctrl-c");
    run("clear");
    wait_state("pending");

    adb::home_pane("apps").expect("home-pane apps");
    wait_state("none");
}
