#include "world.h"

#include "commands.h"
#include "programs.h"

namespace fakeos {
namespace {

const char *kReadme =
    "Welcome to Mattrix OS.\n"
    "\n"
    "This is a pretend Unix system. It lives entirely in your browser and\n"
    "nothing you do here can touch your real computer, so poke around:\n"
    "\n"
    "  ls, cd, cat      look at things          grep, find   search for them\n"
    "  echo hi > file   make things             rm, cp, mv   break things\n"
    "  help             list the commands       reset        start over\n"
    "\n"
    "Tab completes, the arrow keys remember. Some programs do not live in "
    "your\n"
    "PATH. A good explorer looks everywhere.\n";

const char *kMotd =
    "Welcome to Mattrix OS 1.0 (pretend GNU/Linux 6.8.0-mattrix x86_64)\n"
    "\n"
    " * Everything here is make-believe. Type 'help', or read README.\n"
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
    "Jan  1 00:00:01 matterm kernel: Mattrix OS 6.8.0 booting\n"
    "Jan  1 00:00:02 matterm init: starting system\n"
    "Jan  1 00:00:03 matterm sshd: not running (this is a pretend system)\n"
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
           "Mattrix OS (pretend GNU/Linux) \\n \\l\n",
           "root",
           0644,
           base - 30 * day);
    fs.put("/etc/os-release",
           "NAME=\"Mattrix OS\"\nVERSION=\"1.0\"\nID=mattrix\nPRETTY_NAME="
           "\"Mattrix OS 1.0 (pretend)\"\n",
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
    // A shy one, hidden by its dot.
    fs.put("/usr/games/.crumble",
           "#!builtin crumble\n",
           "root",
           0755,
           base - 30 * day);
    world.env = {{"HOME", "/home/" + user},
                 {"USER", user},
                 {"LOGNAME", user},
                 {"HOSTNAME", world.hostname},
                 {"PATH", "/usr/local/bin:/usr/bin:/bin"},
                 {"SHELL", "/bin/mattrix-sh"},
                 {"TERM", "xterm-256color"},
                 {"LANG", "en_US.UTF-8"},
                 {"PWD", "/home/" + user}};
    world.cwd = "/home/" + user;
}

} // namespace fakeos
