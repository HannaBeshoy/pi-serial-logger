# Lab-day test sheet (15 minutes, lab day 2)

**Need:** Raspberry Pi 4 or 5 with Raspberry Pi OS, the Arduino with the sensor-node sketch from the java-serial-monitor project, a USB cable, a laptop on the same network.

1. On the Pi: `git clone https://github.com/HannaBeshoy/pi-serial-logger && cd pi-serial-logger && make && make test`.
2. Plug in the Arduino; `ls /dev/ttyACM* /dev/ttyUSB*` shows the port. `sudo usermod -aG dialout $USER` once (log out and in).
3. `./build/logger -d /dev/ttyACM0 -v`: lines appear; the first boot line is counted as noise.
4. From the laptop: `printf 'LAST\n' | nc <pi-ip> 5600` and `echo -n STATS | nc -u -w1 <pi-ip> 5601`.
5. Rotation: `mv ~/sensor-logs/<today>.csv ~/old.csv; kill -HUP <logger pid>`: a new file with a header appears.
6. Pull the USB cable: the logger reports the port closed. Ctrl-C: "writer exit status 0".
7. Install the service (`deploy/sensor-logger.service`), reboot, check `systemctl status sensor-logger`.

**Record:** the terminal output, a STATS line, the first lines of the CSV file, and a photo of the Pi with the Arduino.
