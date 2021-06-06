# VSC-weather
Migration of Arduino weather station to OTAA &amp; Feather M0 support.   This repo scope paused at the point of 
<ul>
<li> Link to MCCI LMIC library v3.3.0</li>
<li> Convert WS1.0 arduino code to VSC with unchanged functionality</li>
<li> Retain ABP activation and TTN v2</li>
</ul>

Separate repository to explore use of VSC+PlatformIO for the evolution of my original Arduino weather station code https://github.com/kbarrell/Arduino-WeatherStation. The change is prompted by failure of the humidity sensor used in the original implementation, BME280. The upgrade is intended to introduce a separate humidity sensor Sensiron SHT31-D which offers improved condensation protection plus a heater circuit that can be used to keep the sensor free of condensation.

The need to update the weatherstation software also provides an opportunity to adopt Visual Studio Code and the PlatformIO development environment, and to update the stations LoraWAN operation to OTA Activation and the latest LMIC library. While the station will continue to use Arduino Mega 2560, a secondary objective is to explore the code installation on a Feather M0 with integrated Lora radio.
