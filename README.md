# WS2812 RGB Clock
![The clock, showing the time 23:08 in a cyan colour.](/images/front.jpg?raw=true)
This is a simple RGB clock build I made, using WS2812 adressable LED strips, cut up to form digits, and an ESP8266 as the controller. It includes a simple web interface.

## What can it do?
Already implemented:

* Display the time!
* Use different colours per digit
* Use different colours per value of the digit (e.g. 1 is blue, 7 is red etc.)
* Switch between two modes of operation (Day and Night mode) based on a set time
* Easy sketch upload using ArduinoOTA
* MQTT control and Home Assistant integration as a light

## What can't it do?
Not yet implemented:

* Auto-brightness based on a light sensor
* Visual alarm (e.g. flashing)
* Audible alarm (beeper)
* Sensible timekeeping (it currently sends a NTP request every 5 seconds)

## License
I couldn't be bothered to do the whole GPL stuff so I hereby put the entire contents of this repository in the public domain. Use it however you want!

## Gallery
![The clock face with the cover removed, with overlaid arrows showing the wiring sequence.](/images/wiring_sequence.jpg?raw=true)
![The clock face with the cover removed.](/images/front_open.jpg?raw=true)
![A close-up of the wiring on the edges of the segments.](/images/wiring.jpg?raw=true)
![The backside of the clock, showing a small power supply and an even smaller control board.](/images/back.jpg?raw=true)
![A close-up of the small control board, the ESP8266 module being unplugged and sitting next to the board.](/images/electronics.jpg?raw=true)
![A close-up of the wire connections to the power supply, showing 230V mains wires and 5V low-voltage wires.](/images/power_supply.jpg?raw=true)
![A close-up of one of the six hex standoffs for mounting the face plate.](/images/standoff.jpg?raw=true)