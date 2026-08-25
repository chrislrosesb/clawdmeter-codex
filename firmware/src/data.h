#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // utilization 0-100 (5h window Pro/Max; spending % Enterprise)
    int session_reset_mins;  // minutes until reset
    float weekly_pct;        // 7-day utilization (Pro/Max only; 0 for Enterprise)
    int weekly_reset_mins;   // minutes until weekly reset (Pro/Max only)
    char status[16];         // "allowed", "limited", etc.
    bool chime;              // play the session-reset chime; false unless daemon opts in
    bool enterprise;         // true = Enterprise spending-limit account
    int time_pct;            // 0-100: fraction of billing period elapsed (Enterprise)
    int period_days;         // total billing period length in days (Enterprise)
    char reset_date[12];     // formatted reset date e.g. "Jul 1" (Enterprise)
    long clock_epoch;        // local wall-clock epoch (s) from daemon; 0 = not provided
    int  clock_fmt;          // 12 or 24 (hour format from daemon); defaults to 24
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse

    // Codex (OpenAI) usage — the payload's optional "x" object, sourced from
    // Codex CLI's local session logs by the daemon. codex_valid false = the
    // payload carried no Codex data (host has no Codex, or opted out).
    bool  codex_valid;
    float codex_pct;           // primary window used %
    int   codex_reset_mins;    // minutes until primary window resets; -1 unknown
    int   codex_window_mins;   // primary window length (300 = 5h, 10080 = weekly)
    float codex_pct2;          // secondary window used %; -1 = plan has no secondary
    int   codex_reset_mins2;
    int   codex_window_mins2;
    long  codex_tokens_in;     // today's input tokens across local Codex sessions
    long  codex_tokens_out;    // today's output tokens
    long  codex_day_avg;       // avg total tokens/day over the prior 7 days; 0 = no history
};
