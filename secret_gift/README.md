This set of code will be for driving an ESP32 based sticky note display with remote access and storage

In the Arduino IDE, select Tools → Board → ESP32S3 Dev Module (and set PSRAM: "OPI PSRAM")

CrowPanel 4.2 E-Ink Message Archive

FILES
- CrowPanel42_Receiver.ino: ESP32-S3 receiver, SD archive, rotary navigation.
- index.html: browser sender with message text box and live preview.
- mqtt.min.js: obtain the MQTT.js browser build and place beside index.html.

BEFORE UPLOADING
1. Format a microSD card as FAT32 and insert it in the CrowPanel TF slot.
2. Edit WIFI_SSID and WIFI_PASSWORD in the sketch.
3. Ensure MQTT_TOPIC matches in both files. Change it to a long random topic.
4. Install GxEPD2 and PubSubClient in Arduino IDE.
5. Select the correct ESP32-S3 board and upload the sketch.
6. Serve index.html from localhost or HTTPS with mqtt.min.js in the same folder.

CONTROLS
- Rotary UP (GPIO6): previous / older message.
- Rotary DOWN (GPIO4): next / newer message.
- Rotary press (GPIO5): jump to newest message.

ARCHIVE
- Messages are saved on the card as /000001.dat, /000002.dat, ...
- Each file contains date, sender text and the 15,000-byte monochrome bitmap.
- The newest valid message is loaded at startup.
- If the SD card is missing or cannot be written, incoming messages are not shown as archived.

PROTOCOL
V1|YYYY-MM-DD|message text|30000 hex characters

NOTES
- Date is taken from the sender computer's local calendar date at send time.
- Receiver displays date only in the upper-left header.
- Sender text is limited to 80 basic ASCII characters because the display's built-in
  graphics font does not support arbitrary Unicode.
- The display header overlays the top 43 pixels of the transmitted drawing.
- Public MQTT broker/topic is not private. Use a unique random topic and do not send
  sensitive content.
- Test on the exact board revision before relying on it. The SD bus is configured as
  a separate HSPI bus using GPIO39/13/40/10; the e-paper remains on the existing SPI bus.


Screen Link: https://www.amazon.com/ELECROW-CrowPanel-Compatible-Resolution-Interface/dp/B0G43FCHFX/ref=sr_1_4?adgrpid=199750665802&dib=eyJ2IjoiMSJ9.tA7Yv0iqr_zr7o8nRXr2TYj37qYeOa4K9shjDq_4kgU-kyFHSSBQ3D2GtrQ24sECAvd9icLAmkLQqTahnOKu-qW1wXQZBNr5t9EsUK1uDNwEBYfRwx4WQWhUBCMYzXyoehhBqmBtnl18D1XpVyGfuA6zlQp7IXtt3vjcyWOU0cyZhLHrowjQB8LpZBHKICabiOkfmNYXyPxOXyyb4oOXiTpE2E0r3EJ3pKepP36Qpyg.eTLXcQSMqXIH24LQn-kEBA8--TWpj59gBS1Xmv8-xIQ&dib_tag=se&hvadid=816680132799&hvdev=c&hvexpln=0&hvlocphy=9032027&hvnetw=g&hvocijid=18193007628430394844--&hvqmt=b&hvrand=18193007628430394844&hvtargid=kwd-1596785251581&hydadcr=13924_13840273_2527137&keywords=e%2Bpaper%2Bdisplay%2Besp32&mcid=51ea2eb3599a38faa03f05f1277145b9&qid=1794724543&sr=8-4&th=1