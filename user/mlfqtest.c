// Tests for the MLFQ scheduler.
//   mlfqtest          rule tests; run with make CPUS=1 qemu
//   mlfqtest stress   fork/sleep/exit stress; run with the default CPUS=3

#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/pinfo.h"
#include "user/user.h"

#define BOOST 64

static struct pinfo buf[NPROC];
static int fails = 0;

static void
check(int ok, char *what)
{
  printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok)
    fails++;
}

// Fill *out with the entry for pid; return 0 on success.
static int
info(int pid, struct pinfo *out)
{
  int n = getpinfo(buf, NPROC);
  for (int i = 0; i < n; i++) {
    if (buf[i].pid == pid) {
      *out = buf[i];
      return 0;
    }
  }
  return -1;
}

static int
mylevel(void)
{
  struct pinfo me;
  if (info(getpid(), &me) < 0)
    return -1;
  return me.level;
}

static int
myrunticks(void)
{
  struct pinfo me;
  info(getpid(), &me);
  return me.runticks;
}

static void
spin(int nticks)
{
  int t0 = uptime();
  while (uptime() - t0 < nticks)
    ;
}

// Sleep until just after the next boost, so we start at level 0
// with no time used and have ~64 ticks before the next boost.
static void
sync_to_boost(void)
{
  int now = uptime();
  int target = (now / BOOST + 1) * BOOST;
  pause(target - now);
}

// Level a process should be at after k charged ticks, starting at level 0.
static int
expected_level(int k)
{
  if (k < 1)
    return 0;
  if (k < 3)
    return 1;
  if (k < 7)
    return 2;
  return 3;
}

// Rules 4, 5: a lone CPU-bound process is demoted after 1, 2, 4 ticks
// and level 3 stays at 3.
static void
test_demotion(void)
{
  printf("-- demotion\n");
  sync_to_boost();
  int r0 = myrunticks();
  int ok = 1, k = 0;
  while (k < 15) {
    struct pinfo me;
    info(getpid(), &me);
    k = me.runticks - r0;
    if (me.level != expected_level(k)) {
      printf("     after %d charged ticks: level %d, want %d\n", k, me.level,
             expected_level(k));
      ok = 0;
      break;
    }
  }
  check(ok, "levels follow 0 ->1 ->2 ->3 after 1, 2, 4 ticks");
  check(mylevel() == 3, "level 3 stays at 3");
}

// Rule 7: alone at level 3, we are not rescheduled on every tick.
static void
test_no_yield_every_tick(void)
{
  printf("-- no yield on every tick\n");
  struct pinfo a, b;
  sync_to_boost();
  spin(10); // reach level 3
  info(getpid(), &a);
  spin(40);
  info(getpid(), &b);
  int dn = b.nsched - a.nsched;
  printf("     40 ticks at level 3: nsched +%d\n", dn);
  check(dn >= 3 && dn <= 7, "rescheduled ~once per 8-tick slice");
}

// Rule 6: sleeping just after being charged does not reset used time.
static void
test_not_forgiven(void)
{
  printf("-- time is not forgiven\n");
  int pid = fork();
  if (pid == 0) {
    sync_to_boost();
    int r0 = myrunticks();
    for (int i = 0; i < 12; i++) {
      int t = uptime();
      while (uptime() == t) // run until a tick is charged to us
        ;
      pause(1); // then give up the CPU early
    }
    int k = myrunticks() - r0;
    int lvl = mylevel();
    printf("     charged %d ticks in short bursts: level %d\n", k, lvl);
    exit(k >= 7 && lvl == 3 ? 0 : 1);
  }
  int st = -1;
  wait(&st);
  check(st == 0, "short bursts still demote to level 3");
}

// Rule 8: a level-3 process is back at the top right after a boost.
static void
test_boost(void)
{
  printf("-- boost every 64 ticks\n");
  int now = uptime();
  int target = (now / BOOST + 1) * BOOST;
  if (target - now < 12)
    target += BOOST;
  while (uptime() < target - 1)
    ;
  int before = mylevel();
  while (uptime() < target)
    ;
  int after = mylevel();
  printf("     level at tick %d: %d, at tick %d: %d\n", target - 1, before,
         target, after);
  check(before == 3, "CPU-bound process at level 3 before boost");
  check(after <= 1, "boosted to the top at a multiple of 64");
}

// Rule 2: CPU-bound processes at the same level share the CPU evenly.
static void
test_round_robin(void)
{
  printf("-- round-robin within a level\n");
  enum { N = 3, TICKS = 150 };
  int pids[N];
  int deadline = uptime() + TICKS;
  for (int i = 0; i < N; i++) {
    pids[i] = fork();
    if (pids[i] == 0) {
      while (uptime() < deadline)
        ;
      int r = myrunticks();
      exit(r > 255 ? 255 : r);
    }
  }
  int got[N], total = 0;
  for (int i = 0; i < N; i++) {
    int st;
    int pid = wait(&st);
    got[i] = st;
    total += st;
    printf("     child %d: runticks %d\n", pid, st);
  }
  int ok = 1;
  for (int i = 0; i < N; i++)
    if (got[i] * N * 10 < total * 7 || got[i] * N * 10 > total * 13)
      ok = 0;
  check(ok, "each child gets its share (within 30%)");
}

// Rules 1, 7: a mostly-sleeping process stays high and gets the CPU
// promptly even with CPU-bound processes around.
static void
test_interactive(void)
{
  printf("-- interactive beats CPU-bound\n");
  enum { N = 2, ROUNDS = 30 };
  int pids[N];
  sync_to_boost(); // we start at level 0 like everyone else
  for (int i = 0; i < N; i++) {
    pids[i] = fork();
    if (pids[i] == 0) {
      for (;;)
        ;
    }
  }
  pause(10); // let the spinners sink while we sleep
  int t0 = uptime();
  int maxlvl = 0;
  for (int i = 0; i < ROUNDS; i++) {
    pause(1);
    int l = mylevel();
    if (l > maxlvl)
      maxlvl = l;
  }
  int elapsed = uptime() - t0;
  struct pinfo s;
  int slvl = info(pids[0], &s) == 0 ? s.level : -1;
  for (int i = 0; i < N; i++) {
    kill(pids[i]);
    wait(0);
  }
  printf("     %d x pause(1) took %d ticks; my max level %d, spinner level %d\n",
         ROUNDS, elapsed, maxlvl, slvl);
  check(elapsed <= ROUNDS * 2 + 5, "sleeper runs promptly after waking");
  check(maxlvl <= 1, "sleeper stays near the top");
}

static void
test_new_at_zero(void)
{
  printf("-- new processes start at level 0\n");
  spin(10); // parent sinks
  int pid = fork();
  if (pid == 0) {
    exit(mylevel());
  }
  int st = -1;
  wait(&st);
  check(st == 0, "forked child starts at level 0");
}

// ---------------------------------------------------------------- stress

static unsigned long rnd = 12345;
static int
rand(void)
{
  rnd = rnd * 1103515245 + 12345;
  return (rnd >> 16) & 0x7fff;
}

static int
nprocs(void)
{
  return getpinfo(buf, NPROC);
}

static void
stress(void)
{
  enum { ROUNDS = 8, KIDS = 20 };
  int base = nprocs();
  printf("stress: %d rounds x %d children, %d procs at start\n", ROUNDS, KIDS,
         base);
  for (int r = 0; r < ROUNDS; r++) {
    int made = 0;
    for (int i = 0; i < KIDS; i++) {
      rnd += r * 7919 + i;
      int what = rand() % 4;
      int pid = fork();
      if (pid < 0)
        break;
      if (pid == 0) {
        switch (what) {
        case 0:
          spin(1 + rand() % 5);
          break;
        case 1:
          pause(1 + rand() % 3);
          break;
        case 2: {
          int g = fork();
          if (g == 0) {
            spin(rand() % 3);
            exit(0);
          }
          if (g > 0)
            wait(0);
          break;
        }
        default:
          for (int j = 0; j < 5; j++) {
            getpinfo(buf, NPROC);
            pause(0);
          }
        }
        exit(0);
      }
      made++;
    }
    for (int i = 0; i < made; i++) {
      if (wait(0) < 0) {
        printf("FAIL: lost a child in round %d\n", r);
        exit(1);
      }
    }
    printf("  round %d: %d children reaped\n", r, made);
  }
  int end = nprocs();
  check(end == base, "process count back to where it started");
  check(wait(0) == -1, "no children left over");
}

int
main(int argc, char *argv[])
{
  if (argc > 1 && strcmp(argv[1], "stress") == 0) {
    stress();
  } else {
    test_new_at_zero();
    test_demotion();
    test_no_yield_every_tick();
    test_not_forgiven();
    test_boost();
    test_round_robin();
    test_interactive();
  }
  if (fails == 0)
    printf("mlfqtest: all tests passed\n");
  else
    printf("mlfqtest: %d test(s) FAILED\n", fails);
  exit(fails != 0);
}
