# CSSA common room door sensor - ESP32 version

Following on from the roaring success of our Mk. 1 (Arduino Pro Mini -> USB
serial) and Mk. 2 (parallel port as pseudo-GPIO) door sensors, the ANU Computer
Science Students' Association and Widget Co. is proud to present the
**Notification System, Room Occupancy, Electromagnetic, Mk. 3**.

Powered by an ESP32-S2 eval board, this system integrates door sensing _and_
API updating in the one system, like the Mk. 2 computer-based script, thanks to
the ESP32's onboard Wi-Fi, without relying on the notoriously fragile Spotify
machine, as both previous systems had to.

To build this, you'll first need to install [ESP-IDF][1], then:

```console
$ idf.py set-target esp32s2
$ idf.py menuconfig
$ idf.py build
$ idf.py -p <port> flash
$ idf.py -p <port> monitor
```

You'll need to set some valid ANU credentials in `idf.py menuconfig`. Make sure
you don't accidentally push the file `sdkconfig` to Git - it'll have those
credentials in it! If you forget, poke Felix to scrub it from the Git history.

Once you've built and flashed once, substitute `app` and `app-flash` for
`build` and `flash`, to speed things up a little.

[1]: https://github.com/espressif/esp-idf
