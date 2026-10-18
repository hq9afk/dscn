/*
 * dscn — double-tap Shift toggles Caps Lock; the Caps Lock key acts as Super the Super key is disabled, and Caps Lock changes are notified.
 * Every keyboard is grabbed and all its keys are forwarded through one virtual keyboard, which also carries the injected
 * KEY_CAPSLOCK tap. Compositors keep Caps Lock state per input device, so a tap injected from a *second* device would be
 * cancelled by the next real keystroke. The compositor's LED writes to the virtual keyboard drive both the physical
 * keyboards' LEDs and the notifications.
 */

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/input.h>
#include <poll.h>
#include <pwd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// Parameters

constexpr int DOUBLE_SHIFT_MS = 300;   // Max time between the presses of two taps of the same Shift key.

constexpr int NOTIFY_TIMEOUT_MS = 1200; // How long the Caps Lock notification stays on screen.
constexpr unsigned MIN_USER_UID = 1000; // Sessions below this uid (system accounts) are not notified.
constexpr const char *VIRTUAL_KBD_NAME = "dscn-kbd";
// IBM Model M behind a generic PS/2-to-USB adapter; it shows up as "Barcode Reader" so the checks below can't tell it apart.
constexpr int MODEL_M_VENDOR = 0x13ba, MODEL_M_PRODUCT = 0x0018;
// How often the loop wakes up with no data, to notice a signal. Just responsiveness, not precision.
constexpr int POLL_IDLE_MS = 500;

using Clock = std::chrono::steady_clock;

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

bool is_shift(unsigned code) { return code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT; }

/*
 * Double-tap detector - one per physical keyboard, fed key events in order.
 * A "clean tap" is a Shift press+release with no other key down at press time and no other key event in between.
 */

struct Detector {
    std::bitset<KEY_CNT> down;
    unsigned cand_key = 0; // Shift key currently pressed as a possible tap, 0 if none.
    Clock::time_point cand_press;
    unsigned prev_key = 0; // Shift key of the last clean tap, 0 if none.
    Clock::time_point prev_press;

    void reset() { *this = Detector{}; }

    // Returns true when this event completes a double tap.
    bool feed(unsigned code, int value, Clock::time_point now) {
        if (value == 2 || code >= KEY_CNT) // autorepeat
            return false;

        if (value == 1) {
            bool clean = is_shift(code) && !down.any();
            down.set(code);
            if (clean) {
                cand_key = code;
                cand_press = now;
            } else {
                cand_key = 0;
                prev_key = 0;
            }
            return false;
        }

        down.reset(code);
        if (!is_shift(code) || cand_key != code)
            return false;
        cand_key = 0;

        auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(cand_press - prev_press).count();
        if (prev_key == code && gap <= DOUBLE_SHIFT_MS) {
            prev_key = 0;
            return true;
        }
        prev_key = code;
        prev_press = cand_press;
        return false;
    }
};

// Devices

struct Keyboard {
    int fd = -1;
    struct libevdev *dev = nullptr;
    std::string path;
    bool dead = false;
    bool toggle_pending = false; // Double tap seen; Caps Lock is tapped once the current event batch has been forwarded.
    Detector det;

    Keyboard() = default;
    Keyboard(const Keyboard &) = delete;
    Keyboard &operator=(const Keyboard &) = delete;
    ~Keyboard() {
        if (dev)
            libevdev_free(dev);
        if (fd >= 0)
            close(fd);
    }
};

struct Context {
    std::vector<std::unique_ptr<Keyboard>> kbs;
    struct libevdev_uinput *vkbd = nullptr;
    int vkbd_fd = -1;                    // Raw uinput fd: the compositor's LED writes to vkbd come back through it.
    std::array<int, LED_CNT> leds{};     // Last LED values the compositor asked for.
};


// Notification - runs notify-send as each logged-in user (we are root, the session bus is theirs).
// ---------------------------------------------------------------------------

void notify_user(uid_t uid, bool on) {
    struct passwd *pw = getpwuid(uid);
    if (!pw)
        return;
    gid_t gid = pw->pw_gid;
    std::string runtime_dir = "/run/user/" + std::to_string(uid);
    std::string bus_path = runtime_dir + "/bus";
    std::string bus_addr = "unix:path=" + bus_path;
    std::string timeout = std::to_string(NOTIFY_TIMEOUT_MS);

    if (fork() != 0)
        return;

    // child: drop to the user, then hand over to notify-send
    signal(SIGCHLD, SIG_DFL);
    if (setgroups(0, nullptr) != 0 || setgid(gid) != 0 || setuid(uid) != 0)
        _exit(126);
    if (access(bus_path.c_str(), F_OK) != 0) // not a session with a bus (we can only see this after dropping root)
        _exit(0);
    setenv("XDG_RUNTIME_DIR", runtime_dir.c_str(), 1);
    setenv("DBUS_SESSION_BUS_ADDRESS", bus_addr.c_str(), 1);
    execlp("notify-send", "notify-send", "-a", "dscn", "-u", "low", "-i", "input-keyboard", "-t", timeout.c_str(), "-h",
           "string:x-canonical-private-synchronous:dscn-caps", "Caps Lock", on ? "ON" : "OFF", static_cast<char *>(nullptr));
    _exit(127);
}

void notify_caps(bool on) {
    printf("Caps Lock %s\n", on ? "ON" : "OFF");
    fflush(stdout);

    DIR *dir = opendir("/run/user");
    if (!dir)
        return;
    std::vector<uid_t> uids;
    for (struct dirent *entry; (entry = readdir(dir)) != nullptr;) {
        char *end = nullptr;
        unsigned long uid = std::strtoul(entry->d_name, &end, 10);
        if (end != entry->d_name && *end == '\0' && uid >= MIN_USER_UID)
            uids.push_back(static_cast<uid_t>(uid));
    }
    closedir(dir);
    for (uid_t uid : uids)
        notify_user(uid, on);
}

// ---------------------------------------------------------------------------
// LEDs - the compositor writes them to the virtual keyboard; mirror them onto the physical ones.

void write_led(const Keyboard &kb, unsigned code, int value) {
    input_event evs[2]{};
    evs[0].type = EV_LED;
    evs[0].code = static_cast<__u16>(code);
    evs[0].value = value;
    evs[1].type = EV_SYN;
    evs[1].code = SYN_REPORT;
    ssize_t rc = write(kb.fd, evs, sizeof evs);
    (void)rc; // best effort: a keyboard without that LED just ignores it
}

void on_led(Context &ctx, unsigned code, int value) {
    if (code >= LED_CNT)
        return;
    bool changed = ctx.leds[code] != value;
    ctx.leds[code] = value;
    for (const auto &kb : ctx.kbs)
        write_led(*kb, code, value);
    if (changed && code == LED_CAPSL)
        notify_caps(value != 0);
}

void pump_leds(Context &ctx) {
    input_event ev;
    while (read(ctx.vkbd_fd, &ev, sizeof ev) == static_cast<ssize_t>(sizeof ev)) {
        if (ev.type == EV_LED)
            on_led(ctx, ev.code, ev.value);
    }
}

// Virtual keyboard

struct libevdev_uinput *create_virtual_keyboard() {
    struct libevdev *vdev = libevdev_new();
    libevdev_set_name(vdev, VIRTUAL_KBD_NAME);
    libevdev_set_id_bustype(vdev, BUS_VIRTUAL);
    // Every key code except the BTN_* block, so no mouse/joystick buttons make libinput see a pointer.
    for (int code = KEY_ESC; code <= KEY_MAX; ++code) {
        if (code < BTN_MISC || code >= KEY_OK)
            libevdev_enable_event_code(vdev, EV_KEY, code, nullptr);
    }
    libevdev_enable_event_code(vdev, EV_MSC, MSC_SCAN, nullptr);
    // The compositor drives the keyboard LEDs through these; we copy them to the physical keyboards.
    libevdev_enable_event_code(vdev, EV_LED, LED_NUML, nullptr);
    libevdev_enable_event_code(vdev, EV_LED, LED_CAPSL, nullptr);
    libevdev_enable_event_code(vdev, EV_LED, LED_SCROLLL, nullptr);

    struct libevdev_uinput *ui = nullptr;
    int rc = libevdev_uinput_create_from_device(vdev, LIBEVDEV_UINPUT_OPEN_MANAGED, &ui);
    libevdev_free(vdev);
    return rc < 0 ? nullptr : ui;
}

bool tap_capslock(struct libevdev_uinput *ui) {
    int rc = 0;
    rc |= libevdev_uinput_write_event(ui, EV_KEY, KEY_CAPSLOCK, 1);
    rc |= libevdev_uinput_write_event(ui, EV_SYN, SYN_REPORT, 0);
    rc |= libevdev_uinput_write_event(ui, EV_KEY, KEY_CAPSLOCK, 0);
    rc |= libevdev_uinput_write_event(ui, EV_SYN, SYN_REPORT, 0);
    return rc == 0;
}

// Keyboard discovery

// A plain keyboard: has letters and Shift, and no pointer axes (a keyboard+mouse combo node would lose its mouse when grabbed).
bool is_keyboard(struct libevdev *dev) {
    const char *name = libevdev_get_name(dev);
    if (name && std::strcmp(name, VIRTUAL_KBD_NAME) == 0)
        return false;
    if (!libevdev_has_event_code(dev, EV_KEY, KEY_A))
        return false;
    if (libevdev_get_id_vendor(dev) == MODEL_M_VENDOR && libevdev_get_id_product(dev) == MODEL_M_PRODUCT)
        return true;
    return libevdev_has_event_code(dev, EV_KEY, KEY_LEFTSHIFT) && !libevdev_has_event_type(dev, EV_REL) &&
           !libevdev_has_event_type(dev, EV_ABS);
}

void add_keyboard(Context &ctx, const std::string &path) {
    for (const auto &kb : ctx.kbs) {
        if (kb->path == path)
            return;
    }

    int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC); // writable: LEDs are mirrored onto it
    if (fd < 0)
        return;
    struct libevdev *dev = nullptr;
    if (libevdev_new_from_fd(fd, &dev) < 0) {
        close(fd);
        return;
    }
    if (!is_keyboard(dev)) {
        libevdev_free(dev);
        close(fd);
        return;
    }
    // exclusive: the compositor sees only the virtual keyboard
    if (libevdev_grab(dev, LIBEVDEV_GRAB) < 0) {
        fprintf(stderr, "Cannot grab %s (already grabbed?)\n", path.c_str());
        libevdev_free(dev);
        close(fd);
        return;
    }

    auto kb = std::make_unique<Keyboard>();
    kb->fd = fd;
    kb->dev = dev;
    kb->path = path;
    printf("Keyboard: %s (%s)\n", libevdev_get_name(dev) ? libevdev_get_name(dev) : "?", path.c_str());
    for (unsigned led : {LED_NUML, LED_CAPSL, LED_SCROLLL})
        write_led(*kb, led, ctx.leds[led]);
    ctx.kbs.push_back(std::move(kb));
}

void scan_keyboards(Context &ctx) {
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return;
    std::vector<std::string> names;
    for (struct dirent *entry; (entry = readdir(dir)) != nullptr;) {
        if (std::strncmp(entry->d_name, "event", 5) == 0)
            names.emplace_back(entry->d_name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    for (const auto &name : names)
        add_keyboard(ctx, "/dev/input/" + name);
}

void handle_hotplug(Context &ctx, int ifd) {
    alignas(struct inotify_event) char buf[4096];
    for (;;) {
        ssize_t n = read(ifd, buf, sizeof buf);
        if (n <= 0)
            break;
        for (char *p = buf; p < buf + n;) {
            auto *ev = reinterpret_cast<struct inotify_event *>(p);
            if (ev->len && std::strncmp(ev->name, "event", 5) == 0)
                add_keyboard(ctx, std::string("/dev/input/") + ev->name);
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
}

// Event pump

// Forwards one event to the virtual keyboard, mapping Caps Lock to Super, dropping Super, and watching Shift on the way.
void handle_event(Context &ctx, Keyboard &kb, input_event ev, bool syncing) {
    if (ev.type == EV_KEY) {
        if (ev.value == 2) // autorepeat: the compositor does its own
            return;
        if (ev.code == KEY_LEFTMETA || ev.code == KEY_RIGHTMETA) // Super is disabled
            return;
        if (ev.code == KEY_CAPSLOCK) // Caps Lock key acts as Super
            ev.code = KEY_LEFTMETA;
        if (!syncing && kb.det.feed(ev.code, ev.value, Clock::now()))
            kb.toggle_pending = true;
    } else if (ev.type == EV_SYN) {
        if (ev.code != SYN_REPORT) // SYN_DROPPED is ours to handle, not to forward
            return;
    } else if (ev.type != EV_MSC) {
        return; // LED echoes, repeat settings, sounds
    }

    if (libevdev_uinput_write_event(ctx.vkbd, ev.type, ev.code, ev.value) != 0)
        fprintf(stderr, "Could not write to virtual keyboard.\n");

    // only after the batch holding the second Shift release is out, so the Caps Lock tap is its own frame
    if (ev.type == EV_SYN && kb.toggle_pending) {
        kb.toggle_pending = false;
        if (!tap_capslock(ctx.vkbd))
            fprintf(stderr, "Could not write to virtual keyboard.\n");
    }
}

// Reads everything pending on one keyboard. Returns false if the device is gone.
bool pump(Context &ctx, Keyboard &kb) {
    unsigned int flags = LIBEVDEV_READ_FLAG_NORMAL;
    bool syncing = false;
    for (;;) {
        input_event ev;
        int rc = libevdev_next_event(kb.dev, flags, &ev);
        if (rc == -EAGAIN)
            return true;
        if (rc == LIBEVDEV_READ_STATUS_SYNC) { // SYN_DROPPED: key state is unknown, drain the resync
            if (!syncing)
                kb.det.reset();
            syncing = true;
            flags = LIBEVDEV_READ_FLAG_SYNC;
        } else if (rc != LIBEVDEV_READ_STATUS_SUCCESS) {
            if (rc == -EINTR)
                continue;
            if (rc != -ENODEV)
                fprintf(stderr, "libevdev_next_event(%s): %s\n", kb.path.c_str(), strerror(-rc));
            return false;
        }
        handle_event(ctx, kb, ev, syncing);
    }
}

} // namespace

int main() {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // notify-send children are fire-and-forget; don't leave zombies
    struct sigaction sc{};
    sc.sa_handler = SIG_IGN;
    sc.sa_flags = SA_NOCLDWAIT;
    sigaction(SIGCHLD, &sc, nullptr);

    Context ctx;
    ctx.vkbd = create_virtual_keyboard();
    if (!ctx.vkbd) {
        fprintf(stderr, "ERROR: could not create virtual keyboard (is the uinput module loaded?).\n");
        return 1;
    }
    ctx.vkbd_fd = libevdev_uinput_get_fd(ctx.vkbd);
    fcntl(ctx.vkbd_fd, F_SETFL, fcntl(ctx.vkbd_fd, F_GETFL) | O_NONBLOCK);

    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0 || inotify_add_watch(ifd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0)
        fprintf(stderr, "WARNING: cannot watch /dev/input, hot-plugged keyboards will be ignored.\n");

    scan_keyboards(ctx);
    printf("Running \xe2\x80\x94 double-tap Shift (<= %d ms) toggles Caps Lock. Ctrl-C to stop.\n", DOUBLE_SHIFT_MS);
    fflush(stdout);

    while (!g_stop) {
        std::vector<struct pollfd> pfds;
        pfds.push_back({ifd, POLLIN, 0});
        pfds.push_back({ctx.vkbd_fd, POLLIN, 0});
        for (const auto &kb : ctx.kbs)
            pfds.push_back({kb->fd, POLLIN, 0});

        if (poll(pfds.data(), pfds.size(), POLL_IDLE_MS) <= 0)
            continue;

        if (pfds[1].revents & POLLIN)
            pump_leds(ctx);

        for (size_t i = 0; i < ctx.kbs.size(); ++i) {
            short revents = pfds[i + 2].revents;
            Keyboard &kb = *ctx.kbs[i];
            if ((revents & POLLIN) && !pump(ctx, kb))
                kb.dead = true;
            if (revents & (POLLHUP | POLLERR | POLLNVAL))
                kb.dead = true;
        }
        ctx.kbs.erase(std::remove_if(ctx.kbs.begin(), ctx.kbs.end(), [](const auto &kb) { return kb->dead; }), ctx.kbs.end());

        if (ifd >= 0 && (pfds[0].revents & POLLIN))
            handle_hotplug(ctx, ifd);
    }

    ctx.kbs.clear();
    libevdev_uinput_destroy(ctx.vkbd);
    if (ifd >= 0)
        close(ifd);
    printf("\nShutting down.\n");
    return 0;
}
