#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>
#include <sys/select.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#include <vector>

using namespace std;

#define DEVICE_PATH "/dev/input/by-path/platform-i8042-serio-0-event-kbd"

struct libevdev *dev;
struct libevdev_uinput *uidev;

typedef struct {
    int code;
    int value;
    double ts;
} KeyEvent;

vector<KeyEvent> key_queue;

int timer_fd;
struct itimerspec timeout = {
    .it_interval = { .tv_sec = 0, .tv_nsec = 0 },
    .it_value = { .tv_sec = 0, .tv_nsec = 1000000 * 50 },
};

void send_key(const KeyEvent &k) {
    libevdev_uinput_write_event(uidev, EV_KEY, k.code, k.value);
    libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
}

void handle(struct input_event *ev) {
    if (ev->type == EV_KEY) {
        KeyEvent k = {
            .code = ev->code,
            .value = ev->value,
            .ts = ev->time.tv_sec + ev->time.tv_usec / 1000000.0d,
        };
        if (k.code == KEY_LEFTSHIFT || k.code == KEY_LEFTMETA || k.code == KEY_F23) {
            // fprintf(stderr, "QUEUING [%f] code: %s, value: %d\n",
            //         k.ts,
            //         libevdev_event_code_get_name(ev->type, ev->code),
            //         ev->value);
            key_queue.push_back(k);
            timerfd_settime(timer_fd, 0, &timeout, NULL);
        } else if (ev->value != 2) {
            send_key(k);
        }
    }
}

void send_queue() {
    for (int n = key_queue.size(), i = 0; i < n; ++i) {
        auto &k = key_queue[i];
        if (k.code == KEY_F23) {
            k.code = KEY_RIGHTCTRL;
            if (k.value) {
                if (i >= 1) {
                    auto &a = key_queue[i - 1];
                    if (a.value && (a.code == KEY_LEFTMETA || a.code == KEY_LEFTSHIFT) && fabs(k.ts - a.ts) < 0.05) {
                        // fprintf(stderr, "dropping: %s/%d (%f)\n",
                        //         libevdev_event_code_get_name(EV_KEY, a.code), a.value, fabs(k.ts - a.ts));
                        a.code = 0;
                    }
                    if (i >= 2) {
                        auto &b = key_queue[i - 2];
                        if (b.value && (b.code == KEY_LEFTMETA || b.code == KEY_LEFTSHIFT) && fabs(a.ts - b.ts) < 0.05) {
                            // fprintf(stderr, "dropping: %s/%d (%f)\n",
                            //         libevdev_event_code_get_name(EV_KEY, b.code), b.value, fabs(a.ts - b.ts));
                            b.code = 0;
                        }
                    }
                }
                // } else {
                // if (i < n - 1) {
                //     auto &a = key_queue[i + 1];
                //     if (!a.value && (a.code == KEY_LEFTMETA || a.code == KEY_LEFTSHIFT) && fabs(a.ts - k.ts) < 0.01) {
                //         a.code = 0;
                //     }
                //     if (i < n - 2) {
                //         auto &b = key_queue[i + 2];
                //         if (!b.value && (b.code == KEY_LEFTMETA || b.code == KEY_LEFTSHIFT) && fabs(b.ts - a.ts) < 0.01) {
                //             b.code = 0;
                //         }
                //     }
                // }
            }
        }
        if (k.value == 2) k.value = 1;
    }
    for (auto &k : key_queue) {
        if (k.code) {
            send_key(k);
        }
    }
    key_queue.clear();
}

int main(int argc, char **argv) {
    int err;
    int fd, uifd;
    struct timespec tv = {
        .tv_sec = 5,
        .tv_nsec = 0,
    };

    usleep(100000);

    fd = open(DEVICE_PATH, O_RDONLY);
    if (fd < 0) return -errno;

    err = libevdev_new_from_fd(fd, &dev);
    if (err != 0) return err;

    err = libevdev_grab(dev, LIBEVDEV_GRAB);
    if (err != 0) return err;

    uifd = open("/dev/uinput", O_RDWR);
    if (uifd < 0) return -errno;

    err = libevdev_uinput_create_from_device(dev, uifd, &uidev);
    if (err != 0) return err;

    // XXX: needed?
    for (int code = 0; code < 256; ++code) {
        send_key({ .code = code, .value = 0 });
    }

    int pollfd = epoll_create1(0);
    {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(pollfd, EPOLL_CTL_ADD, fd, &ev);
    }

    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = timer_fd;
        epoll_ctl(pollfd, EPOLL_CTL_ADD, timer_fd, &ev);
    }

    struct epoll_event events[10];

    while (true) {
        int n = epoll_wait(pollfd, events, 10, 5000);
        for (int i = 0; i < n; ++i) {
            auto ev = events[i];
            if (ev.data.fd == fd) {
                // keyboard input
                while (libevdev_has_event_pending(dev)) {
                    struct input_event ev;
                    int status = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                    if (status == LIBEVDEV_READ_STATUS_SUCCESS) {
                        handle(&ev);
                    } else if (status == LIBEVDEV_READ_STATUS_SYNC) {
                        handle(&ev);
                        while (libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev) != -EAGAIN) {
                            handle(&ev);
                        }
                    }
                }
            } else if (ev.data.fd == timer_fd) {
                // queued events
                uint64_t tmp;
                read(timer_fd, &tmp, 8);
                send_queue();
            }
        }
    }

    libevdev_uinput_destroy(uidev);
    libevdev_free(dev);
    close(pollfd);
    close(uifd);
    close(fd);
}
