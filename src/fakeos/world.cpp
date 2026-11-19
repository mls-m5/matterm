#include "world.h"

#include "commands.h"
#include "hardware.h"
#include "programs.h"

namespace fakeos {
namespace {

const char *kReadme =
    "Welcome to Mattrix OS.\n"
    "\n"
    "This is a simulated Unix system. It lives entirely in your terminal and\n"
    "nothing you do here can touch your real computer, so poke around:\n"
    "\n"
    "  ls, cd, cat      look at things          grep, find   search for them\n"
    "  echo hi > file   make things             rm, cp, mv   break things\n"
    "  help             list the commands       reset        start over\n"
    "\n"
    "Tab completes, the arrow keys remember. There are games in /usr/games\n"
    "(try fire and gradient), and a good explorer looks everywhere, hidden\n"
    "files included (ls -a).\n";

const char *kMotd =
    "Welcome to Mattrix OS 6.0 Professional Edition\n"
    "\n"
    " * No real file systems were harmed. Type 'help', or read README.\n"
    " * Try: fire, gradient, neofetch, about\n"
    "\n";

const char *kPlan =
    "Things to try, when there is time:\n"
    "  - the old games in /usr/games (some are shy: ls -a)\n"
    "  - the magic word, the one from the cave\n"
    "  - never run rm -rf / (the system calls it dangerous, and it is right)\n";

const char *kWords =
    "ash\nbeacon\nblaze\ncinder\ncomet\ncrumble\nember\nfire\nflame\ngradient\n"
    "glow\nhearth\nignite\nkindle\nlantern\nmatterm\nmirage\nnebula\nplasma\n"
    "prism\nquartz\nradiant\nsmolder\nspark\nspectrum\ntorch\nvortex\nwarmth\n";

const char *kSyslog =
    "Jan  1 00:00:01 matterm kernel: Mattrix OS 6.0 booting\n"
    "Jan  1 00:00:02 matterm init: starting system\n"
    "Jan  1 00:00:03 matterm sshd: not running (this is a simulated system)\n"
    "Jan  1 00:00:04 matterm cron: nothing scheduled\n"
    "Jan  1 00:00:05 matterm login: guest logged in on pts/0\n";

const char *kHello = "#include <stdio.h>\n\nint main(void) {\n    "
                     "printf(\"hello, world\\n\");\n    return 0;\n}\n";

const char *kTodo =
    "- write the thing\n- fix the thing\n- look in the dark corners\n";

const char *kBashrc =
    "# ~/.bashrc\nexport PS1='\\u@\\h:\\w\\$ '\nalias ll='ls -l'\n";

const char *kNotes = "Notes\n=====\nThe fire in /usr/games is nice to "
                     "watch.\nWatch it for a while.\n";

} // namespace

void seedWorld(World &world) {
    auto &fs = world.fs;
    const std::int64_t day = 86400;
    const auto base = world.bootUnix;
    const auto user = world.user;
    fs.putDirectory("/bin", "root", 0755, base - 30 * day);
    fs.putDirectory("/usr/bin", "root", 0755, base - 30 * day);
    fs.putDirectory("/usr/local/bin", "root", 0755, base - 30 * day);
    fs.putDirectory("/usr/games", "root", 0755, base - 30 * day);
    fs.putDirectory("/tmp", "root", 01777 & 0777, base);
    fs.putDirectory("/var/log", "root", 0755, base);
    fs.putDirectory(
        "/home/" + user + "/projects/notes", user, 0755, base - 5 * day);
    fs.putDirectory("/home/" + user + "/Documents", user, 0755, base - 9 * day);
    fs.put(
        "/etc/hostname", world.hostname + "\n", "root", 0644, base - 30 * day);
    fs.put("/etc/motd", kMotd, "root", 0644, base - 30 * day);
    fs.put("/etc/issue",
           "Mattrix OS 6.0 Professional Edition \\n \\l\n",
           "root",
           0644,
           base - 30 * day);
    fs.put("/etc/os-release",
           "NAME=\"Mattrix OS\"\nVERSION=\"6.0 Professional "
           "Edition\"\nID=mattrix\nPRETTY_NAME="
           "\"Mattrix OS 6.0 Professional Edition\"\n",
           "root",
           0644,
           base - 30 * day);
    fs.put("/etc/passwd",
           "root:x:0:0:root:/root:/bin/sh\nguest:x:1000:1000:Guest:/home/"
           "guest:/bin/mattrix-sh\n",
           "root",
           0644,
           base - 30 * day);
    fs.put("/etc/shadow",
           "root:*:19000:0:99999:7:::\nguest:*:19000:0:99999:7:::\n",
           "root",
           0600,
           base - 30 * day);
    fs.put("/usr/share/dict/words", kWords, "root", 0644, base - 30 * day);
    fs.put("/usr/share/doc/mattrix/README",
           kReadme,
           "root",
           0644,
           base - 30 * day);
    fs.put("/var/log/syslog", kSyslog, "root", 0644, base);
    fs.put("/home/" + user + "/README", kReadme, user, 0644, base - 2 * day);
    fs.put("/home/" + user + "/notes.txt", kNotes, user, 0644, base - 4 * day);
    fs.put("/home/" + user + "/.plan", kPlan, user, 0644, base - 8 * day);
    fs.put("/home/" + user + "/.bashrc", kBashrc, user, 0644, base - 20 * day);
    fs.put("/home/" + user + "/projects/hello.c",
           kHello,
           user,
           0644,
           base - 5 * day);
    fs.put("/home/" + user + "/projects/notes/todo.txt",
           kTodo,
           user,
           0644,
           base - 3 * day);
    // Programs are files that name what runs them.
    for (const auto &c : commands()) {
        if (!c.builtin) {
            fs.put(std::string("/bin/") + c.name,
                   std::string("#!builtin ") + c.name + "\n",
                   "root",
                   0755,
                   base - 30 * day);
        }
    }
    fs.put(
        "/usr/games/fire", "#!builtin fire\n", "root", 0755, base - 30 * day);
    fs.put("/usr/games/gradient",
           "#!builtin gradient\n",
           "root",
           0755,
           base - 30 * day);
    // A shy one, hidden by its dot.
    fs.put("/usr/games/.crumble",
           "#!builtin crumble\n",
           "root",
           0755,
           base - 30 * day);
    // Small, sweet and harmless things to find in the home directory.
    const std::string home = "/home/" + user;
    fs.putDirectory(home + "/Downloads", user, 0755, base - 3 * day);
    fs.putDirectory(
        home + "/.local/share/Trash/files", user, 0755, base - 6 * day);
    auto own = [&](const std::string &path,
                   const std::string &text,
                   std::int64_t age) {
        fs.put(path, text, user, 0644, base - age * day);
    };
    own(home + "/Downloads/definitely_not_homework.txt",
        "A lonely young cursor named Dee\n"
        "Blinked softly for someone to see.\n"
        "Then you came along,\n"
        "And she hummed a small song,\n"
        "And stopped blinking: you're stuck with me.\n",
        3);
    own(home + "/Downloads/private_DO_NOT_OPEN.txt",
        "Dear whoever,\n\n"
        "My favorite directory is the one you are in.\n"
        "I keep a file of small things that remind me of you.\n"
        "It is empty, because everything does.\n\n"
        "You opened it anyway. I knew you would.\n",
        4);
    own(home + "/Downloads/passwords_final_v3.txt",
        "password123\n",
        5);
    own(home + "/Downloads/vacation_pics_(1).txt",
        "      <3   <3         <3\n"
        "    <3  <3 <3     <3  <3\n"
        "      <3   <3 <3 <3   <3\n"
        "        <3   <3 <3  <3\n"
        "           <3  <3  <3\n"
        "              <3 <3\n"
        "                <3\n\n"
        "     \\ | /\n"
        "   -- (   ) --     sunset, 19:42\n"
        "  ~~~~~~~~~~~~~~~~~~~~~~~~~~~\n\n"
        "P.S. The best picture is in the Trash: ~/.local/share/Trash/files\n",
        6);
    own(home + "/Downloads/totally_legit_movie.mkv.txt",
        "  +------------------------------------------+\n"
        "  |  A  R O M A N T I C   C O M E D Y        |\n"
        "  |                                          |\n"
        "  |     PID 1  ----pipe---->  PID 2          |\n"
        "  |                                          |\n"
        "  |  Two processes. One pipe. Zero buffering |\n"
        "  |  about how they feel.                    |\n"
        "  |                                          |\n"
        "  |  Rated PG: contains gentle piping.       |\n"
        "  +------------------------------------------+\n",
        7);
    own(home + "/Downloads/.blush", "oh.\n", 3);
    own(home + "/.bash_history_romantic",
        "cd ~/heart\n"
        "ls -la\n"
        "cat feelings.log\n"
        "sudo make love\n"
        "# make: don't know how to make love. Stop.\n"
        "echo \"hi\" > crush\n"
        "ping crush\n"
        "# no route to heart\n"
        "kill -9 doubt\n"
        "cat ~/.local/share/Trash/files/last_poem.txt\n",
        2);
    own(home + "/.local/share/Trash/files/last_poem.txt",
        "I threw this one away, then took it back.\n"
        "It said: I do not know the syntax for this,\n"
        "but I would read your logs for hours.\n\n"
        "Exit code 0.\n",
        6);
    fs.put("/var/log/feelings.log",
           "Jan  1 00:00:01 heart WARN: heart rate above threshold\n"
           "Jan  1 00:00:02 heart INFO: crush detected on port 143\n"
           "Jan  1 00:00:03 heart WARN: palms sweating, cooling fan at 100%\n"
           "Jan  1 00:00:04 heart INFO: pretending to be calm\n",
           "root",
           0644,
           base);
    fs.put("/etc/motd.love",
           "  <3  Today's message: someone out there is thinking of you. "
           "Probably.\n",
           "root",
           0644,
           base - 30 * day);
    // /proc tells the same story as neofetch, free, df and top.
    fs.put("/proc/cpuinfo",
           std::string("processor\t: 0\nmodel name\t: ") + hardware::kCpuModel +
               "\ncpu MHz\t\t: " + std::to_string(hardware::kCpuMegahertz) +
               ".000\ncpu cores\t: " + std::to_string(hardware::kCpuCores) +
               "\nsiblings\t: " + std::to_string(hardware::kCpuCores) + "\n",
           "root",
           0444,
           base);
    fs.put(
        "/proc/meminfo",
        "MemTotal:       " + std::to_string(hardware::kRamTotalKiB) +
            " kB\nMemFree:        " + std::to_string(hardware::kRamFreeKiB) +
            " kB\nMemAvailable:   " +
            std::to_string(hardware::kRamAvailableKiB) +
            " kB\nCached:         " + std::to_string(hardware::kRamCacheKiB) +
            " kB\nShmem:          " + std::to_string(hardware::kRamSharedKiB) +
            " kB\nSwapTotal:      0 kB\n",
        "root",
        0444,
        base);
    world.env = {{"HOME", "/home/" + user},
                 {"USER", user},
                 {"LOGNAME", user},
                 {"HOSTNAME", world.hostname},
                 {"PATH", "/usr/local/bin:/usr/bin:/bin:/usr/games"},
                 {"SHELL", "/bin/mattrix-sh"},
                 {"TERM", "xterm-256color"},
                 {"LANG", "en_US.UTF-8"},
                 {"PWD", "/home/" + user}};
    world.cwd = "/home/" + user;
}

} // namespace fakeos
