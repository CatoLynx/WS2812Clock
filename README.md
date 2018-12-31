# WS2812 RGB Clock
![A picture of the clock, showing the time 15:44, each digit being a different colour.](/images/clock.jpg?raw=true)
This is a simple RGB clock build I made, using WS2812 adressable LED strips, cut up to form digits, and an ESP8266 as the controller. It includes a simple web interface.

## What can it do?
Already implemented:

* Display the time!
* Use different colours per digit
* Use different colours per value of the digit (e.g. 1 is blue, 7 is red etc.)
* Switch between two modes of operation (Day and Night mode) based on a set time
* Easy sketch upload using ArduinoOTA

## What can't it do?
Not yet implemented:

* Auto-brightness based on a light sensor
* Visual alarm (e.g. flashing)
* Audible alarm (beeper)
* Sensible timekeeping (it currently sends a NTP request every 10 seconds)

## License
Just use it, it's public domain.